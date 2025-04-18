#pragma once

#include <base/types.h>
#include <Common/isLocalAddress.h>
#include <Common/MultiVersion.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/RemoteHostFilter.h>
#include <Common/ThreadPool.h>
#include <Core/Block.h>
#include <Core/NamesAndTypes.h>
#include <Core/Settings.h>
#include <Core/UUID.h>
#include <IO/AsyncReadCounters.h>
#include <Interpreters/ClientInfo.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/MergeTreeTransactionHolder.h>
#include <IO/IResourceManager.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/IAST_fwd.h>
#include <Processors/ResizeProcessor.h>
#include <Processors/Transforms/ReadFromMergeTreeDependencyTransform.h>
#include <Server/HTTP/HTTPContext.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorage_fwd.h>

#include <VectorIndex/Storages/VSDescription.h>

#include "config.h"

#include <boost/container/flat_set.hpp>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>


namespace Poco::Net { class IPAddress; }
namespace zkutil { class ZooKeeper; }

struct OvercommitTracker;

namespace DB
{

struct ContextSharedPart;
class ContextAccess;
struct User;
using UserPtr = std::shared_ptr<const User>;
struct EnabledRolesInfo;
class EnabledRowPolicies;
struct RowPolicyFilter;
using RowPolicyFilterPtr = std::shared_ptr<const RowPolicyFilter>;
class EnabledQuota;
struct QuotaUsage;
class AccessFlags;
struct AccessRightsElement;
class AccessRightsElements;
enum class RowPolicyFilterType;
class EmbeddedDictionaries;
class ExternalDictionariesLoader;
class ExternalUserDefinedExecutableFunctionsLoader;
class IUserDefinedSQLObjectsLoader;
class InterserverCredentials;
using InterserverCredentialsPtr = std::shared_ptr<const InterserverCredentials>;
class InterserverIOHandler;
class BackgroundSchedulePool;
class MergeList;
class MovesList;
class ReplicatedFetchList;
class Cluster;
class Compiler;
class MarkCache;
class MMappedFileCache;
class UncompressedCache;
class ProcessList;
class QueryStatus;
using QueryStatusPtr = std::shared_ptr<QueryStatus>;
class Macros;
struct Progress;
struct FileProgress;
class Clusters;
class QueryCache;
class QueryLog;
class QueryThreadLog;
class QueryViewsLog;
class PartLog;
class TextLog;
class TraceLog;
class MetricLog;
class AsynchronousMetricLog;
class OpenTelemetrySpanLog;
class ZooKeeperLog;
class SessionLog;
class BackupsWorker;
class TransactionsInfoLog;
class ProcessorsProfileLog;
class FilesystemCacheLog;
class FilesystemReadPrefetchesLog;
class AsynchronousInsertLog;
class VIEventLog;
class IAsynchronousReader;
struct MergeTreeSettings;
struct InitialAllRangesAnnouncement;
struct ParallelReadRequest;
struct ParallelReadResponse;
class StorageS3Settings;
class IDatabase;
class DDLWorker;
class ITableFunction;
class Block;
class ActionLocksManager;
using ActionLocksManagerPtr = std::shared_ptr<ActionLocksManager>;
class ShellCommand;
class ICompressionCodec;
class AccessControl;
class Credentials;
class GSSAcceptorContext;
struct SettingsConstraintsAndProfileIDs;
class SettingsProfileElements;
class RemoteHostFilter;
struct StorageID;
class IDisk;
using DiskPtr = std::shared_ptr<IDisk>;
class DiskSelector;
using DiskSelectorPtr = std::shared_ptr<const DiskSelector>;
using DisksMap = std::map<String, DiskPtr>;
class IStoragePolicy;
using StoragePolicyPtr = std::shared_ptr<const IStoragePolicy>;
using StoragePoliciesMap = std::map<String, StoragePolicyPtr>;
class StoragePolicySelector;
using StoragePolicySelectorPtr = std::shared_ptr<const StoragePolicySelector>;
template <class Queue>
class MergeTreeBackgroundExecutor;

/// Scheduling policy can be changed using `background_merges_mutations_scheduling_policy` config option.
/// By default concurrent merges are scheduled using "round_robin" to ensure fair and starvation-free operation.
/// Previously in heavily overloaded shards big merges could possibly be starved by smaller
/// merges due to the use of strict priority scheduling "shortest_task_first".
class DynamicRuntimeQueue;
using MergeMutateBackgroundExecutor = MergeTreeBackgroundExecutor<DynamicRuntimeQueue>;
using MergeMutateBackgroundExecutorPtr = std::shared_ptr<MergeMutateBackgroundExecutor>;

class RoundRobinRuntimeQueue;
using OrdinaryBackgroundExecutor = MergeTreeBackgroundExecutor<RoundRobinRuntimeQueue>;
using OrdinaryBackgroundExecutorPtr = std::shared_ptr<OrdinaryBackgroundExecutor>;
struct PartUUIDs;
using PartUUIDsPtr = std::shared_ptr<PartUUIDs>;
class KeeperDispatcher;
class Session;
struct WriteSettings;

class IInputFormat;
class IOutputFormat;
using InputFormatPtr = std::shared_ptr<IInputFormat>;
using OutputFormatPtr = std::shared_ptr<IOutputFormat>;
class IVolume;
using VolumePtr = std::shared_ptr<IVolume>;
struct NamedSession;
struct BackgroundTaskSchedulingSettings;

#if USE_NLP
    class SynonymsExtensions;
    class Lemmatizers;
#endif

class Throttler;
using ThrottlerPtr = std::shared_ptr<Throttler>;

class ZooKeeperMetadataTransaction;
using ZooKeeperMetadataTransactionPtr = std::shared_ptr<ZooKeeperMetadataTransaction>;

class AsynchronousInsertQueue;

/// Callback for external tables initializer
using ExternalTablesInitializer = std::function<void(ContextPtr)>;

/// Callback for initialize input()
using InputInitializer = std::function<void(ContextPtr, const StoragePtr &)>;
/// Callback for reading blocks of data from client for function input()
using InputBlocksReader = std::function<Block(ContextPtr)>;

/// Used in distributed task processing
using ReadTaskCallback = std::function<String()>;

using MergeTreeAllRangesCallback = std::function<void(InitialAllRangesAnnouncement)>;
using MergeTreeReadTaskCallback = std::function<std::optional<ParallelReadResponse>(ParallelReadRequest)>;

class TemporaryDataOnDiskScope;
using TemporaryDataOnDiskScopePtr = std::shared_ptr<TemporaryDataOnDiskScope>;

class ParallelReplicasReadingCoordinator;
using ParallelReplicasReadingCoordinatorPtr = std::shared_ptr<ParallelReplicasReadingCoordinator>;

#if USE_ROCKSDB
class MergeTreeMetadataCache;
using MergeTreeMetadataCachePtr = std::shared_ptr<MergeTreeMetadataCache>;
#endif

/// An empty interface for an arbitrary object that may be attached by a shared pointer
/// to query context, when using ClickHouse as a library.
struct IHostContext
{
    virtual ~IHostContext() = default;
};

using IHostContextPtr = std::shared_ptr<IHostContext>;

/// A small class which owns ContextShared.
/// We don't use something like unique_ptr directly to allow ContextShared type to be incomplete.
struct SharedContextHolder
{
    ~SharedContextHolder();
    SharedContextHolder();
    explicit SharedContextHolder(std::unique_ptr<ContextSharedPart> shared_context);
    SharedContextHolder(SharedContextHolder &&) noexcept;

    SharedContextHolder & operator=(SharedContextHolder &&) noexcept;

    ContextSharedPart * get() const { return shared.get(); }
    void reset();

private:
    std::unique_ptr<ContextSharedPart> shared;
};


/** A set of known objects that can be used in the query.
  * Consists of a shared part (always common to all sessions and queries)
  *  and copied part (which can be its own for each session or query).
  *
  * Everything is encapsulated for all sorts of checks and locks.
  */
class Context: public std::enable_shared_from_this<Context>
{
private:
    ContextSharedPart * shared;

    ClientInfo client_info;
    ExternalTablesInitializer external_tables_initializer_callback;

    InputInitializer input_initializer_callback;
    InputBlocksReader input_blocks_reader;

    std::optional<UUID> user_id;
    std::shared_ptr<std::vector<UUID>> current_roles;
    std::shared_ptr<const SettingsConstraintsAndProfileIDs> settings_constraints_and_current_profiles;
    std::shared_ptr<const ContextAccess> access;
    std::shared_ptr<const EnabledRowPolicies> row_policies_of_initial_user;
    String current_database;
    Settings settings;  /// Setting for query execution.

    using ProgressCallback = std::function<void(const Progress & progress)>;
    ProgressCallback progress_callback;  /// Callback for tracking progress of query execution.

    using FileProgressCallback = std::function<void(const FileProgress & progress)>;
    FileProgressCallback file_progress_callback; /// Callback for tracking progress of file loading.

    std::weak_ptr<QueryStatus> process_list_elem;  /// For tracking total resource usage for query.
    bool has_process_list_elem = false;     /// It's impossible to check if weak_ptr was initialized or not
    StorageID insertion_table = StorageID::createEmpty();  /// Saved insertion table in query context
    bool is_distributed = false;  /// Whether the current context it used for distributed query

    String default_format;  /// Format, used when server formats data by itself and if query does not have FORMAT specification.
                            /// Thus, used in HTTP interface. If not specified - then some globally default format is used.

    String insert_format; /// Format, used in insert query.

    TemporaryTablesMapping external_tables_mapping;
    Scalars scalars;
    /// Used to store constant values which are different on each instance during distributed plan, such as _shard_num.
    Scalars special_scalars;

    /// Used in s3Cluster table function. With this callback, a worker node could ask an initiator
    /// about next file to read from s3.
    std::optional<ReadTaskCallback> next_task_callback;
    /// Used in parallel reading from replicas. A replica tells about its intentions to read
    /// some ranges from some part and initiator will tell the replica about whether it is accepted or denied.
    std::optional<MergeTreeReadTaskCallback> merge_tree_read_task_callback;
    std::optional<MergeTreeAllRangesCallback> merge_tree_all_ranges_callback;
    UUID parallel_replicas_group_uuid{UUIDHelpers::Nil};

    /// This parameter can be set by the HTTP client to tune the behavior of output formats for compatibility.
    UInt64 client_protocol_version = 0;

    /// Record entities accessed by current query, and store this information in system.query_log.
    struct QueryAccessInfo
    {
        QueryAccessInfo() = default;

        QueryAccessInfo(const QueryAccessInfo & rhs)
        {
            std::lock_guard<std::mutex> lock(rhs.mutex);
            databases = rhs.databases;
            tables = rhs.tables;
            columns = rhs.columns;
            projections = rhs.projections;
            views = rhs.views;
        }

        QueryAccessInfo(QueryAccessInfo && rhs) = delete;

        QueryAccessInfo & operator=(QueryAccessInfo rhs)
        {
            swap(rhs);
            return *this;
        }

        void swap(QueryAccessInfo & rhs)
        {
            std::swap(databases, rhs.databases);
            std::swap(tables, rhs.tables);
            std::swap(columns, rhs.columns);
            std::swap(projections, rhs.projections);
            std::swap(views, rhs.views);
        }

        /// To prevent a race between copy-constructor and other uses of this structure.
        mutable std::mutex mutex{};
        std::set<std::string> databases{};
        std::set<std::string> tables{};
        std::set<std::string> columns{};
        std::set<std::string> projections{};
        std::set<std::string> views{};
    };

    QueryAccessInfo query_access_info;

    /// Record names of created objects of factories (for testing, etc)
    struct QueryFactoriesInfo
    {
        QueryFactoriesInfo() = default;

        QueryFactoriesInfo(const QueryFactoriesInfo & rhs)
        {
            std::lock_guard<std::mutex> lock(rhs.mutex);
            aggregate_functions = rhs.aggregate_functions;
            aggregate_function_combinators = rhs.aggregate_function_combinators;
            database_engines = rhs.database_engines;
            data_type_families = rhs.data_type_families;
            dictionaries = rhs.dictionaries;
            formats = rhs.formats;
            functions = rhs.functions;
            storages = rhs.storages;
            table_functions = rhs.table_functions;
        }

        QueryFactoriesInfo(QueryFactoriesInfo && rhs) = delete;

        QueryFactoriesInfo & operator=(QueryFactoriesInfo rhs)
        {
            swap(rhs);
            return *this;
        }

        void swap(QueryFactoriesInfo & rhs)
        {
            std::swap(aggregate_functions, rhs.aggregate_functions);
            std::swap(aggregate_function_combinators, rhs.aggregate_function_combinators);
            std::swap(database_engines, rhs.database_engines);
            std::swap(data_type_families, rhs.data_type_families);
            std::swap(dictionaries, rhs.dictionaries);
            std::swap(formats, rhs.formats);
            std::swap(functions, rhs.functions);
            std::swap(storages, rhs.storages);
            std::swap(table_functions, rhs.table_functions);
        }

        std::unordered_set<std::string> aggregate_functions;
        std::unordered_set<std::string> aggregate_function_combinators;
        std::unordered_set<std::string> database_engines;
        std::unordered_set<std::string> data_type_families;
        std::unordered_set<std::string> dictionaries;
        std::unordered_set<std::string> formats;
        std::unordered_set<std::string> functions;
        std::unordered_set<std::string> storages;
        std::unordered_set<std::string> table_functions;

        mutable std::mutex mutex;
    };

    /// Needs to be changed while having const context in factories methods
    mutable QueryFactoriesInfo query_factories_info;
    /// Query metrics for reading data asynchronously with IAsynchronousReader.
    mutable std::shared_ptr<AsyncReadCounters> async_read_counters;

    /// TODO: maybe replace with temporary tables?
    StoragePtr view_source;                 /// Temporary StorageValues used to generate alias columns for materialized views
    Tables table_function_results;          /// Temporary tables obtained by execution of table functions. Keyed by AST tree id.

    ContextWeakMutablePtr query_context;
    ContextWeakMutablePtr session_context;  /// Session context or nullptr. Could be equal to this.
    ContextWeakMutablePtr global_context;   /// Global context. Could be equal to this.

    /// XXX: move this stuff to shared part instead.
    ContextMutablePtr buffer_context;  /// Buffer context. Could be equal to this.

    /// A flag, used to distinguish between user query and internal query to a database engine (MaterializedPostgreSQL).
    bool is_internal_query = false;
    bool is_detach_query = false;

    inline static ContextPtr global_context_instance;

    /// Temporary data for query execution accounting.
    TemporaryDataOnDiskScopePtr temp_data_on_disk;

    /// TODO: will be enhanced similar as scalars.
    /// Used when vector scan func exists in right joined table
    mutable MutableVSDescriptionsPtr right_vector_scan_descs;

    mutable TextSearchInfoPtr right_text_search_info;
    mutable HybridSearchInfoPtr right_hybrid_search_info;

public:
    /// Some counters for current query execution.
    /// Most of them are workarounds and should be removed in the future.
    struct KitchenSink
    {
        std::atomic<size_t> analyze_counter = 0;

        KitchenSink() = default;

        KitchenSink(const KitchenSink & rhs)
            : analyze_counter(rhs.analyze_counter.load())
        {}

        KitchenSink & operator=(const KitchenSink & rhs)
        {
            analyze_counter = rhs.analyze_counter.load();
            return *this;
        }
    };

    KitchenSink kitchen_sink;

    ParallelReplicasReadingCoordinatorPtr parallel_reading_coordinator;



private:
    using SampleBlockCache = std::unordered_map<std::string, Block>;
    mutable SampleBlockCache sample_block_cache;

    PartUUIDsPtr part_uuids; /// set of parts' uuids, is used for query parts deduplication
    PartUUIDsPtr ignored_part_uuids; /// set of parts' uuids are meant to be excluded from query processing

    NameToNameMap query_parameters;   /// Dictionary with query parameters for prepared statements.
                                                     /// (key=name, value)

    IHostContextPtr host_context;  /// Arbitrary object that may used to attach some host specific information to query context,
                                   /// when using ClickHouse as a library in some project. For example, it may contain host
                                   /// logger, some query identification information, profiling guards, etc. This field is
                                   /// to be customized in HTTP and TCP servers by overloading the customizeContext(DB::ContextPtr)
                                   /// methods.

    ZooKeeperMetadataTransactionPtr metadata_transaction;    /// Distributed DDL context. I'm not sure if it's a suitable place for this,
                                                    /// but it's the easiest way to pass this through the whole stack from executeQuery(...)
                                                    /// to DatabaseOnDisk::commitCreateTable(...) or IStorage::alter(...) without changing
                                                    /// thousands of signatures.
                                                    /// And I hope it will be replaced with more common Transaction sometime.

    MergeTreeTransactionPtr merge_tree_transaction;     /// Current transaction context. Can be inside session or query context.
                                                        /// It's shared with all children contexts.
    MergeTreeTransactionHolder merge_tree_transaction_holder;   /// It will rollback or commit transaction on Context destruction.

    /// Use copy constructor or createGlobal() instead
    Context();
    Context(const Context &);
    Context & operator=(const Context &);

public:
    /// Create initial Context with ContextShared and etc.
    static ContextMutablePtr createGlobal(ContextSharedPart * shared);
    static ContextMutablePtr createCopy(const ContextWeakPtr & other);
    static ContextMutablePtr createCopy(const ContextMutablePtr & other);
    static ContextMutablePtr createCopy(const ContextPtr & other);
    static SharedContextHolder createShared();

    ~Context();

    String getPath() const;
    String getFlagsPath() const;
    String getUserFilesPath() const;
    String getDictionariesLibPath() const;
    String getUserScriptsPath() const;
    String getVectorIndexCachePath() const;
    String getTantivyIndexCachePath() const;

    /// A list of warnings about server configuration to place in `system.warnings` table.
    Strings getWarnings() const;

    VolumePtr getTemporaryVolume() const; /// TODO: remove, use `getTempDataOnDisk`

    TemporaryDataOnDiskScopePtr getTempDataOnDisk() const;
    void setTempDataOnDisk(TemporaryDataOnDiskScopePtr temp_data_on_disk_);

    void setPath(const String & path);
    void setFlagsPath(const String & path);
    void setUserFilesPath(const String & path);
    void setDictionariesLibPath(const String & path);
    void setUserScriptsPath(const String & path);
    void setVectorIndexCachePath(const String & path);
    void setTantivyIndexCachePath(const String & path);

    void addWarningMessage(const String & msg) const;

    void setTemporaryStorageInCache(const String & cache_disk_name, size_t max_size);
    void setTemporaryStoragePolicy(const String & policy_name, size_t max_size);
    void setTemporaryStoragePath(const String & path, size_t max_size);

    using ConfigurationPtr = Poco::AutoPtr<Poco::Util::AbstractConfiguration>;

    /// Global application configuration settings.
    void setConfig(const ConfigurationPtr & config);
    const Poco::Util::AbstractConfiguration & getConfigRef() const;

    AccessControl & getAccessControl();
    const AccessControl & getAccessControl() const;

    /// Sets external authenticators config (LDAP, Kerberos).
    void setExternalAuthenticatorsConfig(const Poco::Util::AbstractConfiguration & config);

    /// Creates GSSAcceptorContext instance based on external authenticator params.
    std::unique_ptr<GSSAcceptorContext> makeGSSAcceptorContext() const;

    /** Take the list of users, quotas and configuration profiles from this config.
      * The list of users is completely replaced.
      * The accumulated quota values are not reset if the quota is not deleted.
      */
    void setUsersConfig(const ConfigurationPtr & config);
    ConfigurationPtr getUsersConfig();

    /// Sets the current user assuming that he/she is already authenticated.
    /// WARNING: This function doesn't check password!
    void setUser(const UUID & user_id_);

    UserPtr getUser() const;
    String getUserName() const;
    std::optional<UUID> getUserID() const;

    void setQuotaKey(String quota_key_);

    void setCurrentRoles(const std::vector<UUID> & current_roles_);
    void setCurrentRolesDefault();
    boost::container::flat_set<UUID> getCurrentRoles() const;
    boost::container::flat_set<UUID> getEnabledRoles() const;
    std::shared_ptr<const EnabledRolesInfo> getRolesInfo() const;

    void setCurrentProfile(const String & profile_name);
    void setCurrentProfile(const UUID & profile_id);
    std::vector<UUID> getCurrentProfiles() const;
    std::vector<UUID> getEnabledProfiles() const;

    /// Checks access rights.
    /// Empty database means the current database.
    void checkAccess(const AccessFlags & flags) const;
    void checkAccess(const AccessFlags & flags, std::string_view database) const;
    void checkAccess(const AccessFlags & flags, std::string_view database, std::string_view table) const;
    void checkAccess(const AccessFlags & flags, std::string_view database, std::string_view table, std::string_view column) const;
    void checkAccess(const AccessFlags & flags, std::string_view database, std::string_view table, const std::vector<std::string_view> & columns) const;
    void checkAccess(const AccessFlags & flags, std::string_view database, std::string_view table, const Strings & columns) const;
    void checkAccess(const AccessFlags & flags, const StorageID & table_id) const;
    void checkAccess(const AccessFlags & flags, const StorageID & table_id, std::string_view column) const;
    void checkAccess(const AccessFlags & flags, const StorageID & table_id, const std::vector<std::string_view> & columns) const;
    void checkAccess(const AccessFlags & flags, const StorageID & table_id, const Strings & columns) const;
    void checkAccess(const AccessRightsElement & element) const;
    void checkAccess(const AccessRightsElements & elements) const;

    std::shared_ptr<const ContextAccess> getAccess() const;

    RowPolicyFilterPtr getRowPolicyFilter(const String & database, const String & table_name, RowPolicyFilterType filter_type) const;

    /// Finds and sets extra row policies to be used based on `client_info.initial_user`,
    /// if the initial user exists.
    /// TODO: we need a better solution here. It seems we should pass the initial row policy
    /// because a shard is allowed to not have the initial user or it might be another user
    /// with the same name.
    void enableRowPoliciesOfInitialUser();

    std::shared_ptr<const EnabledQuota> getQuota() const;
    std::optional<QuotaUsage> getQuotaUsage() const;

    /// Resource management related
    ResourceManagerPtr getResourceManager() const;
    ClassifierPtr getClassifier() const;

    /// We have to copy external tables inside executeQuery() to track limits. Therefore, set callback for it. Must set once.
    void setExternalTablesInitializer(ExternalTablesInitializer && initializer);
    /// This method is called in executeQuery() and will call the external tables initializer.
    void initializeExternalTablesIfSet();

    /// When input() is present we have to send columns structure to client
    void setInputInitializer(InputInitializer && initializer);
    /// This method is called in StorageInput::read while executing query
    void initializeInput(const StoragePtr & input_storage);

    /// Callback for read data blocks from client one by one for function input()
    void setInputBlocksReaderCallback(InputBlocksReader && reader);
    /// Get callback for reading data for input()
    InputBlocksReader getInputBlocksReaderCallback() const;
    void resetInputCallbacks();

    ClientInfo & getClientInfo() { return client_info; }
    const ClientInfo & getClientInfo() const { return client_info; }

    enum StorageNamespace
    {
         ResolveGlobal = 1u,                                           /// Database name must be specified
         ResolveCurrentDatabase = 2u,                                  /// Use current database
         ResolveOrdinary = ResolveGlobal | ResolveCurrentDatabase,     /// If database name is not specified, use current database
         ResolveExternal = 4u,                                         /// Try get external table
         ResolveAll = ResolveExternal | ResolveOrdinary                /// If database name is not specified, try get external table,
                                                                       ///    if external table not found use current database.
    };

    String resolveDatabase(const String & database_name) const;
    StorageID resolveStorageID(StorageID storage_id, StorageNamespace where = StorageNamespace::ResolveAll) const;
    StorageID tryResolveStorageID(StorageID storage_id, StorageNamespace where = StorageNamespace::ResolveAll) const;
    StorageID resolveStorageIDImpl(StorageID storage_id, StorageNamespace where, std::optional<Exception> * exception) const;

    Tables getExternalTables() const;
    void addExternalTable(const String & table_name, TemporaryTableHolder && temporary_table);
    std::shared_ptr<TemporaryTableHolder> removeExternalTable(const String & table_name);

    const Scalars & getScalars() const;
    const Block & getScalar(const String & name) const;
    void addScalar(const String & name, const Block & block);
    bool hasScalar(const String & name) const;

    const Block * tryGetSpecialScalar(const String & name) const;
    void addSpecialScalar(const String & name, const Block & block);

    const QueryAccessInfo & getQueryAccessInfo() const { return query_access_info; }
    void addQueryAccessInfo(
        const String & quoted_database_name,
        const String & full_quoted_table_name,
        const Names & column_names,
        const String & projection_name = {},
        const String & view_name = {});


    /// Supported factories for records in query_log
    enum class QueryLogFactories
    {
        AggregateFunction,
        AggregateFunctionCombinator,
        Database,
        DataType,
        Dictionary,
        Format,
        Function,
        Storage,
        TableFunction
    };

    const QueryFactoriesInfo & getQueryFactoriesInfo() const { return query_factories_info; }
    void addQueryFactoriesInfo(QueryLogFactories factory_type, const String & created_object) const;

    /// For table functions s3/file/url/hdfs/input we can use structure from
    /// insertion table depending on select expression.
    StoragePtr executeTableFunction(const ASTPtr & table_expression, const ASTSelectQuery * select_query_hint = nullptr);

    void addViewSource(const StoragePtr & storage);
    StoragePtr getViewSource() const;

    String getCurrentDatabase() const;
    String getCurrentQueryId() const { return client_info.current_query_id; }

    /// Id of initiating query for distributed queries; or current query id if it's not a distributed query.
    String getInitialQueryId() const;

    void setCurrentDatabase(const String & name);
    /// Set current_database for global context. We don't validate that database
    /// exists because it should be set before databases loading.
    void setCurrentDatabaseNameInGlobalContext(const String & name);
    void setCurrentQueryId(const String & query_id);

    void killCurrentQuery() const;

    bool hasInsertionTable() const { return !insertion_table.empty(); }
    void setInsertionTable(StorageID db_and_table) { insertion_table = std::move(db_and_table); }
    const StorageID & getInsertionTable() const { return insertion_table; }

    void setDistributed(bool is_distributed_) { is_distributed = is_distributed_; }
    bool isDistributed() const { return is_distributed; }

    String getDefaultFormat() const;    /// If default_format is not specified, some global default format is returned.
    void setDefaultFormat(const String & name);

    String getInsertFormat() const;
    void setInsertFormat(const String & name);

    MultiVersion<Macros>::Version getMacros() const;
    void setMacros(std::unique_ptr<Macros> && macros);

    Settings getSettings() const;
    void setSettings(const Settings & settings_);

    /// Set settings by name.
    void setSetting(std::string_view name, const String & value);
    void setSetting(std::string_view name, const Field & value);
    void applySettingChange(const SettingChange & change);
    void applySettingsChanges(const SettingsChanges & changes);

    /// Checks the constraints.
    void checkSettingsConstraints(const SettingsProfileElements & profile_elements) const;
    void checkSettingsConstraints(const SettingChange & change) const;
    void checkSettingsConstraints(const SettingsChanges & changes) const;
    void checkSettingsConstraints(SettingsChanges & changes) const;
    void clampToSettingsConstraints(SettingsChanges & changes) const;
    void checkMergeTreeSettingsConstraints(const MergeTreeSettings & merge_tree_settings, const SettingsChanges & changes) const;

    /// Reset settings to default value
    void resetSettingsToDefaultValue(const std::vector<String> & names);

    /// Returns the current constraints (can return null).
    std::shared_ptr<const SettingsConstraintsAndProfileIDs> getSettingsConstraintsAndCurrentProfiles() const;

    const ExternalDictionariesLoader & getExternalDictionariesLoader() const;
    ExternalDictionariesLoader & getExternalDictionariesLoader();
    ExternalDictionariesLoader & getExternalDictionariesLoaderUnlocked();
    const EmbeddedDictionaries & getEmbeddedDictionaries() const;
    EmbeddedDictionaries & getEmbeddedDictionaries();
    void tryCreateEmbeddedDictionaries(const Poco::Util::AbstractConfiguration & config) const;
    void loadOrReloadDictionaries(const Poco::Util::AbstractConfiguration & config);

    const ExternalUserDefinedExecutableFunctionsLoader & getExternalUserDefinedExecutableFunctionsLoader() const;
    ExternalUserDefinedExecutableFunctionsLoader & getExternalUserDefinedExecutableFunctionsLoader();
    ExternalUserDefinedExecutableFunctionsLoader & getExternalUserDefinedExecutableFunctionsLoaderUnlocked();
    const IUserDefinedSQLObjectsLoader & getUserDefinedSQLObjectsLoader() const;
    IUserDefinedSQLObjectsLoader & getUserDefinedSQLObjectsLoader();
    void loadOrReloadUserDefinedExecutableFunctions(const Poco::Util::AbstractConfiguration & config);

#if USE_NLP
    SynonymsExtensions & getSynonymsExtensions() const;
    Lemmatizers & getLemmatizers() const;
#endif

    BackupsWorker & getBackupsWorker() const;

    /// I/O formats.
    InputFormatPtr getInputFormat(const String & name, ReadBuffer & buf, const Block & sample, UInt64 max_block_size, const std::optional<FormatSettings> & format_settings = std::nullopt) const;

    OutputFormatPtr getOutputFormat(const String & name, WriteBuffer & buf, const Block & sample) const;
    OutputFormatPtr getOutputFormatParallelIfPossible(const String & name, WriteBuffer & buf, const Block & sample) const;

    InterserverIOHandler & getInterserverIOHandler();
    const InterserverIOHandler & getInterserverIOHandler() const;

    /// How other servers can access this for downloading replicated data.
    void setInterserverIOAddress(const String & host, UInt16 port);
    std::pair<String, UInt16> getInterserverIOAddress() const;

    /// Credentials which server will use to communicate with others
    void updateInterserverCredentials(const Poco::Util::AbstractConfiguration & config);
    InterserverCredentialsPtr getInterserverCredentials() const;

    /// Interserver requests scheme (http or https)
    void setInterserverScheme(const String & scheme);
    String getInterserverScheme() const;

    /// Storage of allowed hosts from config.xml
    void setRemoteHostFilter(const Poco::Util::AbstractConfiguration & config);
    const RemoteHostFilter & getRemoteHostFilter() const;

    /// The port that the server listens for executing SQL queries.
    UInt16 getTCPPort() const;

    std::optional<UInt16> getTCPPortSecure() const;

    /// Register server ports during server starting up. No lock is held.
    void registerServerPort(String port_name, UInt16 port);

    UInt16 getServerPort(const String & port_name) const;

    /// For methods below you may need to acquire the context lock by yourself.

    ContextMutablePtr getQueryContext() const;
    bool hasQueryContext() const { return !query_context.expired(); }
    bool isInternalSubquery() const;

    ContextMutablePtr getSessionContext() const;
    bool hasSessionContext() const { return !session_context.expired(); }

    ContextMutablePtr getGlobalContext() const;

    static ContextPtr getGlobalContextInstance() { return global_context_instance; }

    bool hasGlobalContext() const { return !global_context.expired(); }
    bool isGlobalContext() const
    {
        auto ptr = global_context.lock();
        return ptr && ptr.get() == this;
    }

    ContextMutablePtr getBufferContext() const;

    void setQueryContext(ContextMutablePtr context_) { query_context = context_; }
    void setSessionContext(ContextMutablePtr context_) { session_context = context_; }

    void makeQueryContext() { query_context = shared_from_this(); }
    void makeSessionContext() { session_context = shared_from_this(); }
    void makeGlobalContext() { initGlobal(); global_context = shared_from_this(); }

    const Settings & getSettingsRef() const { return settings; }

    void setProgressCallback(ProgressCallback callback);
    /// Used in executeQuery() to pass it to the QueryPipeline.
    ProgressCallback getProgressCallback() const;

    void setFileProgressCallback(FileProgressCallback && callback) { file_progress_callback = callback; }
    FileProgressCallback getFileProgressCallback() const { return file_progress_callback; }

    /** Set in executeQuery and InterpreterSelectQuery. Then it is used in QueryPipeline,
      *  to update and monitor information about the total number of resources spent for the query.
      */
    void setProcessListElement(QueryStatusPtr elem);
    /// Can return nullptr if the query was not inserted into the ProcessList.
    QueryStatusPtr getProcessListElement() const;

    /// List all queries.
    ProcessList & getProcessList();
    const ProcessList & getProcessList() const;

    OvercommitTracker * getGlobalOvercommitTracker() const;

    MergeList & getMergeList();
    const MergeList & getMergeList() const;

    MovesList & getMovesList();
    const MovesList & getMovesList() const;

    ReplicatedFetchList & getReplicatedFetchList();
    const ReplicatedFetchList & getReplicatedFetchList() const;

    /// If the current session is expired at the time of the call, synchronously creates and returns a new session with the startNewSession() call.
    /// If no ZooKeeper configured, throws an exception.
    std::shared_ptr<zkutil::ZooKeeper> getZooKeeper() const;
    /// Same as above but return a zookeeper connection from auxiliary_zookeepers configuration entry.
    std::shared_ptr<zkutil::ZooKeeper> getAuxiliaryZooKeeper(const String & name) const;

    /// Try to connect to Keeper using get(Auxiliary)ZooKeeper. Useful for
    /// internal Keeper start (check connection to some other node). Return true
    /// if connected successfully (without exception) or our zookeeper client
    /// connection configured for some other cluster without our node.
    bool tryCheckClientConnectionToMyKeeperCluster() const;

    UInt32 getZooKeeperSessionUptime() const;
    UInt64 getClientProtocolVersion() const;
    void setClientProtocolVersion(UInt64 version);

#if USE_ROCKSDB
    MergeTreeMetadataCachePtr getMergeTreeMetadataCache() const;
    MergeTreeMetadataCachePtr tryGetMergeTreeMetadataCache() const;
#endif

#if USE_NURAFT
    std::shared_ptr<KeeperDispatcher> & getKeeperDispatcher() const;
    std::shared_ptr<KeeperDispatcher> & tryGetKeeperDispatcher() const;
#endif
    void initializeKeeperDispatcher(bool start_async) const;
    void shutdownKeeperDispatcher() const;
    void updateKeeperConfiguration(const Poco::Util::AbstractConfiguration & config);

    /// Set auxiliary zookeepers configuration at server starting or configuration reloading.
    void reloadAuxiliaryZooKeepersConfigIfChanged(const ConfigurationPtr & config);
    /// Has ready or expired ZooKeeper
    bool hasZooKeeper() const;
    /// Has ready or expired auxiliary ZooKeeper
    bool hasAuxiliaryZooKeeper(const String & name) const;
    /// Reset current zookeeper session. Do not create a new one.
    void resetZooKeeper() const;
    // Reload Zookeeper
    void reloadZooKeeperIfChanged(const ConfigurationPtr & config) const;

    void setSystemZooKeeperLogAfterInitializationIfNeeded();

    /// Create a cache of uncompressed blocks of specified size. This can be done only once.
    void setUncompressedCache(const String & uncompressed_cache_policy, size_t max_size_in_bytes);
    std::shared_ptr<UncompressedCache> getUncompressedCache() const;
    void dropUncompressedCache() const;

    /// Create a cache of marks of specified size. This can be done only once.
    void setMarkCache(const String & mark_cache_policy, size_t cache_size_in_bytes);
    std::shared_ptr<MarkCache> getMarkCache() const;
    void dropMarkCache() const;
    ThreadPool & getLoadMarksThreadpool() const;

    ThreadPool & getPrefetchThreadpool() const;

    /// Note: prefetchThreadpool is different from threadpoolReader
    /// in the way that its tasks are - wait for marks to be loaded
    /// and make a prefetch by putting a read task to threadpoolReader.
    size_t getPrefetchThreadpoolSize() const;

    /// Create a cache of index uncompressed blocks of specified size. This can be done only once.
    void setIndexUncompressedCache(size_t max_size_in_bytes);
    std::shared_ptr<UncompressedCache> getIndexUncompressedCache() const;
    void dropIndexUncompressedCache() const;

    /// Primary key cache size limit.
    void setPKCacheSize(size_t max_size_in_bytes);
    size_t getPKCacheSize() const;

    /// Create a cache of index marks of specified size. This can be done only once.
    void setIndexMarkCache(size_t cache_size_in_bytes);
    std::shared_ptr<MarkCache> getIndexMarkCache() const;
    void dropIndexMarkCache() const;

    /// Create a cache of mapped files to avoid frequent open/map/unmap/close and to reuse from several threads.
    void setMMappedFileCache(size_t cache_size_in_num_entries);
    std::shared_ptr<MMappedFileCache> getMMappedFileCache() const;
    void dropMMappedFileCache() const;

    /// Create a cache of query results for statements which run repeatedly.
    void setQueryCache(const Poco::Util::AbstractConfiguration & config);
    void updateQueryCacheConfiguration(const Poco::Util::AbstractConfiguration & config);
    std::shared_ptr<QueryCache> getQueryCache() const;
    void dropQueryCache() const;

    /** Clear the caches of the uncompressed blocks and marks.
      * This is usually done when renaming tables, changing the type of columns, deleting a table.
      *  - since caches are linked to file names, and become incorrect.
      *  (when deleting a table - it is necessary, since in its place another can appear)
      * const - because the change in the cache is not considered significant.
      */
    void dropCaches() const;

    /// Settings for MergeTree background tasks stored in config.xml
    BackgroundTaskSchedulingSettings getBackgroundProcessingTaskSchedulingSettings() const;
    BackgroundTaskSchedulingSettings getBackgroundMoveTaskSchedulingSettings() const;

    BackgroundSchedulePool & getBufferFlushSchedulePool() const;
    BackgroundSchedulePool & getSchedulePool() const;
    BackgroundSchedulePool & getMessageBrokerSchedulePool() const;
    BackgroundSchedulePool & getDistributedSchedulePool() const;

    ThrottlerPtr getReplicatedFetchesThrottler() const;
    ThrottlerPtr getReplicatedSendsThrottler() const;
    ThrottlerPtr getRemoteReadThrottler() const;
    ThrottlerPtr getRemoteWriteThrottler() const;

    /// Has distributed_ddl configuration or not.
    bool hasDistributedDDL() const;
    void setDDLWorker(std::unique_ptr<DDLWorker> ddl_worker);
    DDLWorker & getDDLWorker() const;

    std::shared_ptr<Clusters> getClusters() const;
    std::shared_ptr<Cluster> getCluster(const std::string & cluster_name) const;
    std::shared_ptr<Cluster> tryGetCluster(const std::string & cluster_name) const;
    void setClustersConfig(const ConfigurationPtr & config, bool enable_discovery = false, const String & config_name = "remote_servers");

    void startClusterDiscovery();

    /// Sets custom cluster, but doesn't update configuration
    void setCluster(const String & cluster_name, const std::shared_ptr<Cluster> & cluster);
    void reloadClusterConfig() const;

    Compiler & getCompiler();

    /// Call after initialization before using system logs. Call for global context.
    void initializeSystemLogs();

    /// Call after initialization before using trace collector.
    void initializeTraceCollector();

#if USE_ROCKSDB
    void initializeMergeTreeMetadataCache(const String & dir, size_t size);
#endif

    bool hasTraceCollector() const;

    /// Nullptr if the query log is not ready for this moment.
    std::shared_ptr<QueryLog> getQueryLog() const;
    std::shared_ptr<QueryThreadLog> getQueryThreadLog() const;
    std::shared_ptr<QueryViewsLog> getQueryViewsLog() const;
    std::shared_ptr<TraceLog> getTraceLog() const;
    std::shared_ptr<TextLog> getTextLog() const;
    std::shared_ptr<MetricLog> getMetricLog() const;
    std::shared_ptr<AsynchronousMetricLog> getAsynchronousMetricLog() const;
    std::shared_ptr<OpenTelemetrySpanLog> getOpenTelemetrySpanLog() const;
    std::shared_ptr<ZooKeeperLog> getZooKeeperLog() const;
    std::shared_ptr<SessionLog> getSessionLog() const;
    std::shared_ptr<TransactionsInfoLog> getTransactionsInfoLog() const;
    std::shared_ptr<ProcessorsProfileLog> getProcessorsProfileLog() const;
    std::shared_ptr<FilesystemCacheLog> getFilesystemCacheLog() const;
    std::shared_ptr<FilesystemReadPrefetchesLog> getFilesystemReadPrefetchesLog() const;
    std::shared_ptr<AsynchronousInsertLog> getAsynchronousInsertLog() const;
    std::shared_ptr<VIEventLog> getVectorIndexEventLog(const String & part_database = {}) const;

    /// Returns an object used to log operations with parts if it possible.
    /// Provide table name to make required checks.
    std::shared_ptr<PartLog> getPartLog(const String & part_database) const;

    const MergeTreeSettings & getMergeTreeSettings() const;
    const MergeTreeSettings & getReplicatedMergeTreeSettings() const;
    const StorageS3Settings & getStorageS3Settings() const;

    /// Prevents DROP TABLE if its size is greater than max_size (50GB by default, max_size=0 turn off this check)
    void setMaxTableSizeToDrop(size_t max_size);
    void checkTableCanBeDropped(const String & database, const String & table, const size_t & table_size) const;

    /// Prevents DROP PARTITION if its size is greater than max_size (50GB by default, max_size=0 turn off this check)
    void setMaxPartitionSizeToDrop(size_t max_size);
    void checkPartitionCanBeDropped(const String & database, const String & table, const size_t & partition_size) const;

    /// Lets you select the compression codec according to the conditions described in the configuration file.
    std::shared_ptr<ICompressionCodec> chooseCompressionCodec(size_t part_size, double part_size_ratio) const;


    /// Provides storage disks
    DiskPtr getDisk(const String & name) const;
    using DiskCreator = std::function<DiskPtr(const DisksMap & disks_map)>;
    DiskPtr getOrCreateDisk(const String & name, DiskCreator creator) const;

    StoragePoliciesMap getPoliciesMap() const;
    DisksMap getDisksMap() const;
    void updateStorageConfiguration(const Poco::Util::AbstractConfiguration & config);

    /// Provides storage politics schemes
    StoragePolicyPtr getStoragePolicy(const String & name) const;

    StoragePolicyPtr getStoragePolicyFromDisk(const String & disk_name) const;

    /// Get the server uptime in seconds.
    double getUptimeSeconds() const;

    using ConfigReloadCallback = std::function<void()>;
    void setConfigReloadCallback(ConfigReloadCallback && callback);
    void reloadConfig() const;

    void shutdown();
    bool isShutdown() const;

    bool isInternalQuery() const { return is_internal_query; }
    void setInternalQuery(bool internal) { is_internal_query = internal; }
    bool isDetachQuery() const { return  is_detach_query; }
    void setDetachQuery(bool detach) { is_detach_query = detach; }

    ActionLocksManagerPtr getActionLocksManager() const;

    enum class ApplicationType
    {
        SERVER,         /// The program is run as clickhouse-server daemon (default behavior)
        CLIENT,         /// clickhouse-client
        LOCAL,          /// clickhouse-local
        KEEPER,         /// clickhouse-keeper (also daemon)
        DISKS,          /// clickhouse-disks
    };

    ApplicationType getApplicationType() const;
    void setApplicationType(ApplicationType type);

    /// Sets default_profile and system_profile, must be called once during the initialization
    void setDefaultProfiles(const Poco::Util::AbstractConfiguration & config);
    String getDefaultProfileName() const;
    String getSystemProfileName() const;

    /// Base path for format schemas
    String getFormatSchemaPath() const;
    void setFormatSchemaPath(const String & path);

    SampleBlockCache & getSampleBlockCache() const;

    /// Query parameters for prepared statements.
    bool hasQueryParameters() const;
    const NameToNameMap & getQueryParameters() const;

    /// Throws if parameter with the given name already set.
    void setQueryParameter(const String & name, const String & value);
    void setQueryParameters(const NameToNameMap & parameters) { query_parameters = parameters; }

    /// Overrides values of existing parameters.
    void addQueryParameters(const NameToNameMap & parameters);

    /// Add started bridge command. It will be killed after context destruction
    void addBridgeCommand(std::unique_ptr<ShellCommand> cmd) const;

    IHostContextPtr & getHostContext();
    const IHostContextPtr & getHostContext() const;

    /// Initialize context of distributed DDL query with Replicated database.
    void initZooKeeperMetadataTransaction(ZooKeeperMetadataTransactionPtr txn, bool attach_existing = false);
    /// Returns context of current distributed DDL query or nullptr.
    ZooKeeperMetadataTransactionPtr getZooKeeperMetadataTransaction() const;
    /// Removes context of current distributed DDL.
    void resetZooKeeperMetadataTransaction();

    void checkTransactionsAreAllowed(bool explicit_tcl_query = false) const;
    void initCurrentTransaction(MergeTreeTransactionPtr txn);
    void setCurrentTransaction(MergeTreeTransactionPtr txn);
    MergeTreeTransactionPtr getCurrentTransaction() const;

    bool isServerCompletelyStarted() const;
    void setServerCompletelyStarted();

    PartUUIDsPtr getPartUUIDs() const;
    PartUUIDsPtr getIgnoredPartUUIDs() const;

    AsynchronousInsertQueue * getAsynchronousInsertQueue() const;
    void setAsynchronousInsertQueue(const std::shared_ptr<AsynchronousInsertQueue> & ptr);

    ReadTaskCallback getReadTaskCallback() const;
    void setReadTaskCallback(ReadTaskCallback && callback);

    MergeTreeReadTaskCallback getMergeTreeReadTaskCallback() const;
    void setMergeTreeReadTaskCallback(MergeTreeReadTaskCallback && callback);

    MergeTreeAllRangesCallback getMergeTreeAllRangesCallback() const;
    void setMergeTreeAllRangesCallback(MergeTreeAllRangesCallback && callback);

    UUID getParallelReplicasGroupUUID() const;
    void setParallelReplicasGroupUUID(UUID uuid);

    /// Background executors related methods
    void initializeBackgroundExecutorsIfNeeded();
    bool areBackgroundExecutorsInitialized();

    MergeMutateBackgroundExecutorPtr getMergeMutateExecutor() const;
    OrdinaryBackgroundExecutorPtr getMovesExecutor() const;
    OrdinaryBackgroundExecutorPtr getFetchesExecutor() const;
    OrdinaryBackgroundExecutorPtr getCommonExecutor() const;
    MergeMutateBackgroundExecutorPtr getVectorIndexExecutor() const;
    MergeMutateBackgroundExecutorPtr getSlowModeVectorIndexExecutor() const;

    enum class FilesystemReaderType
    {
        SYNCHRONOUS_LOCAL_FS_READER,
        ASYNCHRONOUS_LOCAL_FS_READER,
        ASYNCHRONOUS_REMOTE_FS_READER,
    };

    IAsynchronousReader & getThreadPoolReader(FilesystemReaderType type) const;

    size_t getThreadPoolReaderSize(FilesystemReaderType type) const;

    std::shared_ptr<AsyncReadCounters> getAsyncReadCounters() const;

    ThreadPool & getThreadPoolWriter() const;

    /** Get settings for reading from filesystem. */
    ReadSettings getReadSettings() const;

    /** Get settings for writing to filesystem. */
    WriteSettings getWriteSettings() const;

    /** There are multiple conditions that have to be met to be able to use parallel replicas */
    bool canUseParallelReplicasOnInitiator() const;
    bool canUseParallelReplicasOnFollower() const;

    enum class ParallelReplicasMode : uint8_t
    {
        SAMPLE_KEY,
        CUSTOM_KEY,
        READ_TASKS,
    };

    ParallelReplicasMode getParallelReplicasMode() const;

    /// Used for vector scan functions
    MutableVSDescriptionsPtr getVecScanDescriptions() const;
    void setVecScanDescriptions(MutableVSDescriptionsPtr vec_scan_descs) const;
    void resetVecScanDescriptions() const;

    /// Used for text search functions
    TextSearchInfoPtr getTextSearchInfo() const;
    void setTextSearchInfo(TextSearchInfoPtr text_search_info) const;
    void resetTextSearchInfo() const;

    /// Used for text search functions
    HybridSearchInfoPtr getHybridSearchInfo() const;
    void setHybridSearchInfo(HybridSearchInfoPtr hybrid_search_info) const;
    void resetHybridSearchInfo() const;

private:
    std::unique_lock<std::recursive_mutex> getLock() const;

    void initGlobal();

    /// Compute and set actual user settings, client_info.current_user should be set
    void calculateAccessRights();

    template <typename... Args>
    void checkAccessImpl(const Args &... args) const;

    EmbeddedDictionaries & getEmbeddedDictionariesImpl(bool throw_on_error) const;

    void checkCanBeDropped(const String & database, const String & table, const size_t & size, const size_t & max_size_to_drop) const;

    StoragePolicySelectorPtr getStoragePolicySelector(std::lock_guard<std::mutex> & lock) const;

    DiskSelectorPtr getDiskSelector(std::lock_guard<std::mutex> & lock) const;

    DisksMap getDisksMap(std::lock_guard<std::mutex> & lock) const;
};

struct HTTPContext : public IHTTPContext
{
    explicit HTTPContext(ContextPtr context_)
        : context(Context::createCopy(context_))
    {}

    uint64_t getMaxHstsAge() const override
    {
        return context->getSettingsRef().hsts_max_age;
    }

    uint64_t getMaxUriSize() const override
    {
        return context->getSettingsRef().http_max_uri_size;
    }

    uint64_t getMaxFields() const override
    {
        return context->getSettingsRef().http_max_fields;
    }

    uint64_t getMaxFieldNameSize() const override
    {
        return context->getSettingsRef().http_max_field_name_size;
    }

    uint64_t getMaxFieldValueSize() const override
    {
        return context->getSettingsRef().http_max_field_value_size;
    }

    uint64_t getMaxChunkSize() const override
    {
        return context->getSettingsRef().http_max_chunk_size;
    }

    Poco::Timespan getReceiveTimeout() const override
    {
        return context->getSettingsRef().http_receive_timeout;
    }

    Poco::Timespan getSendTimeout() const override
    {
        return context->getSettingsRef().http_send_timeout;
    }

    ContextPtr context;
};

}
