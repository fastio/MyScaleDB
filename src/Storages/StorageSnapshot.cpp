#include <Storages/StorageSnapshot.h>
#include <Storages/LightweightDeleteDescription.h>
#include <Storages/IStorage.h>
#include <DataTypes/ObjectUtils.h>
#include <DataTypes/NestedUtils.h>
#include <Storages/StorageView.h>
#include <sparsehash/dense_hash_set>
#include <VectorIndex/Utils/HybridSearchUtils.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_FOUND_COLUMN_IN_BLOCK;
    extern const int EMPTY_LIST_OF_COLUMNS_QUERIED;
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int COLUMN_QUERIED_MORE_THAN_ONCE;
}

void StorageSnapshot::init()
{
    for (const auto & [name, type] : storage.getVirtuals())
        virtual_columns[name] = type;

    if (storage.hasLightweightDeletedMask())
        system_columns[LightweightDeleteDescription::FILTER_COLUMN.name] = LightweightDeleteDescription::FILTER_COLUMN.type;
}

NamesAndTypesList StorageSnapshot::getColumns(const GetColumnsOptions & options) const
{
    auto all_columns = getMetadataForQuery()->getColumns().get(options);

    if (options.with_extended_objects)
        extendObjectColumns(all_columns, object_columns, options.with_subcolumns);

    NameSet column_names;
    if (options.with_virtuals)
    {
        /// Virtual columns must be appended after ordinary,
        /// because user can override them.
        if (!virtual_columns.empty())
        {
            for (const auto & column : all_columns)
                column_names.insert(column.name);

            for (const auto & [name, type] : virtual_columns)
                if (!column_names.contains(name))
                    all_columns.emplace_back(name, type);
        }
    }

    if (options.with_system_columns)
    {
        if (!system_columns.empty() && column_names.empty())
        {
            for (const auto & column : all_columns)
                column_names.insert(column.name);
        }

        for (const auto & [name, type] : system_columns)
            if (!column_names.contains(name))
                all_columns.emplace_back(name, type);
    }

    return all_columns;
}

NamesAndTypesList StorageSnapshot::getColumnsByNames(const GetColumnsOptions & options, const Names & names) const
{
    NamesAndTypesList res;
    for (const auto & name : names)
    {
        auto column = tryGetColumn(options, name);
        if (column)
        {
            res.push_back(*column);
            continue;
        }

        /// Check special columns for text/vector/hybrid search after get columns from table columns
        if (isDistance(name) || isTextSearch(name) || isHybridSearch(name) || isScoreColumnName(name))
        {
            res.emplace_back(name, std::make_shared<DataTypeFloat32>());
        }
        else if (name == SCORE_TYPE_COLUMN.name)
        {
            res.emplace_back(SCORE_TYPE_COLUMN);
        }
        else if (isBatchDistance(name))
        {
            auto id_type = std::make_shared<DataTypeUInt32>();
            auto distance_type = std::make_shared<DataTypeFloat32>();
            DataTypes types;
            types.emplace_back(id_type);
            types.emplace_back(distance_type);
            auto type = std::make_shared<DataTypeTuple>(types);
            res.emplace_back(name, type);
        }
        else
            throw Exception(ErrorCodes::NO_SUCH_COLUMN_IN_TABLE, "There is no column {} in table", name);
    }

    return res;
}

std::optional<NameAndTypePair> StorageSnapshot::tryGetColumn(const GetColumnsOptions & options, const String & column_name) const
{
    const auto & columns = getMetadataForQuery()->getColumns();
    auto column = columns.tryGetColumn(options, column_name);
    if (column && (!column->type->hasDynamicSubcolumns() || !options.with_extended_objects))
        return column;

    if (options.with_extended_objects)
    {
        auto object_column = object_columns.tryGetColumn(options, column_name);
        if (object_column)
            return object_column;
    }

    if (options.with_virtuals)
    {
        auto it = virtual_columns.find(column_name);
        if (it != virtual_columns.end())
            return NameAndTypePair(column_name, it->second);
    }

    if (options.with_system_columns)
    {
        auto it = system_columns.find(column_name);
        if (it != system_columns.end())
            return NameAndTypePair(column_name, it->second);
    }

    return {};
}

NameAndTypePair StorageSnapshot::getColumn(const GetColumnsOptions & options, const String & column_name) const
{
    auto column = tryGetColumn(options, column_name);
    if (!column)
        throw Exception(ErrorCodes::NO_SUCH_COLUMN_IN_TABLE, "There is no column {} in table", column_name);

    return *column;
}

Block StorageSnapshot::getSampleBlockForColumns(const Names & column_names, const NameToNameMap & parameter_values) const
{
    Block res;

    const auto & columns = getMetadataForQuery()->getColumns();
    for (const auto & column_name : column_names)
    {
        std::string substituted_column_name = column_name;

        /// substituted_column_name is used for parameterized view (which are created using query parameters
        /// and SELECT is used with substitution of these query parameters )
        if (!parameter_values.empty())
            substituted_column_name = StorageView::replaceValueWithQueryParameter(column_name, parameter_values);

        auto column = columns.tryGetColumnOrSubcolumn(GetColumnsOptions::All, substituted_column_name);
        auto object_column = object_columns.tryGetColumnOrSubcolumn(GetColumnsOptions::All, substituted_column_name);
        if (column && !object_column)
        {
            res.insert({column->type->createColumn(), column->type, column_name});
        }
        else if (object_column)
        {
            res.insert({object_column->type->createColumn(), object_column->type, column_name});
        }
        else if (auto it = virtual_columns.find(column_name); it != virtual_columns.end())
        {
            /// Virtual columns must be appended after ordinary, because user can
            /// override them.
            const auto & type = it->second;
            res.insert({type->createColumn(), type, column_name});
        }
        else if (isDistance(column_name) || isTextSearch(column_name) || isHybridSearch(column_name) || isScoreColumnName(column_name))
        {
            auto type = std::make_shared<DataTypeFloat32>();
            res.insert({type->createColumn(), type, column_name});
        }
        else if (column_name == SCORE_TYPE_COLUMN.name)
        {
            res.insert({SCORE_TYPE_COLUMN.type->createColumn(), SCORE_TYPE_COLUMN.type, SCORE_TYPE_COLUMN.name});
        }
        else if (isBatchDistance(column_name))
        {
            auto id_type = std::make_shared<DataTypeUInt32>();
            auto distance_type = std::make_shared<DataTypeFloat32>();
            DataTypes types;
            types.emplace_back(id_type);
            types.emplace_back(distance_type);
            auto type = std::make_shared<DataTypeTuple>(types);
            res.insert({type->createColumn(), type, column_name});
        }
        else
        {
            throw Exception(ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK,
                "Column {} not found in table {}", backQuote(substituted_column_name), storage.getStorageID().getNameForLogs());
        }
    }
    return res;
}

ColumnsDescription StorageSnapshot::getDescriptionForColumns(const Names & column_names) const
{
    ColumnsDescription res;
    const auto & columns = getMetadataForQuery()->getColumns();
    for (const auto & name : column_names)
    {
        auto column = columns.tryGetColumnOrSubcolumnDescription(GetColumnsOptions::All, name);
        auto object_column = object_columns.tryGetColumnOrSubcolumnDescription(GetColumnsOptions::All, name);
        if (column && !object_column)
        {
            res.add(*column, "", false, false);
        }
        else if (object_column)
        {
            res.add(*object_column, "", false, false);
        }
        else if (auto it = virtual_columns.find(name); it != virtual_columns.end())
        {
            /// Virtual columns must be appended after ordinary, because user can
            /// override them.
            const auto & type = it->second;
            res.add({name, type});
        }
        else
        {
            throw Exception(ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK,
                            "Column {} not found in table {}", backQuote(name), storage.getStorageID().getNameForLogs());
        }
    }

    return res;
}

namespace
{
    using DenseHashSet = google::dense_hash_set<StringRef, StringRefHash>;
}

void StorageSnapshot::check(const Names & column_names) const
{
    const auto & columns = getMetadataForQuery()->getColumns();
    auto options = GetColumnsOptions(GetColumnsOptions::AllPhysical).withSubcolumns();

    if (column_names.empty())
    {
        auto list_of_columns = listOfColumns(columns.get(options));
        throw Exception(ErrorCodes::EMPTY_LIST_OF_COLUMNS_QUERIED,
            "Empty list of columns queried. There are columns: {}", list_of_columns);
    }

    DenseHashSet unique_names;
    unique_names.set_empty_key(StringRef());

    for (const auto & name : column_names)
    {
        bool has_column = columns.hasColumnOrSubcolumn(GetColumnsOptions::AllPhysical, name)
            || object_columns.hasColumnOrSubcolumn(GetColumnsOptions::AllPhysical, name)
            || virtual_columns.contains(name);

        if (!has_column)
        {
            auto list_of_columns = listOfColumns(columns.get(options));
            throw Exception(ErrorCodes::NO_SUCH_COLUMN_IN_TABLE,
                "There is no column with name {} in table {}. There are columns: {}",
                backQuote(name), storage.getStorageID().getNameForLogs(), list_of_columns);
        }

        if (unique_names.count(name))
            throw Exception(ErrorCodes::COLUMN_QUERIED_MORE_THAN_ONCE, "Column {} queried more than once", name);

        unique_names.insert(name);
    }
}

DataTypePtr StorageSnapshot::getConcreteType(const String & column_name) const
{
    auto object_column = object_columns.tryGetColumnOrSubcolumn(GetColumnsOptions::All, column_name);
    if (object_column)
        return object_column->type;

    return metadata->getColumns().get(column_name).type;
}

}
