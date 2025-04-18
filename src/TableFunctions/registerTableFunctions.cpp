#include "registerTableFunctions.h"
#include <TableFunctions/TableFunctionFactory.h>


namespace DB
{
void registerTableFunctions()
{
    auto & factory = TableFunctionFactory::instance();

    registerTableFunctionMerge(factory);
    registerTableFunctionRemote(factory);
    registerTableFunctionNumbers(factory);
    registerTableFunctionNull(factory);
    registerTableFunctionZeros(factory);
    registerTableFunctionExecutable(factory);
    registerTableFunctionFile(factory);
    registerTableFunctionURL(factory);
    registerTableFunctionValues(factory);
    registerTableFunctionInput(factory);
    registerTableFunctionGenerate(factory);
    registerTableFunctionMongoDB(factory);

    registerTableFunctionMeiliSearch(factory);

#if USE_AWS_S3
    registerTableFunctionS3(factory);
    registerTableFunctionS3Cluster(factory);
    registerTableFunctionCOS(factory);
    registerTableFunctionOSS(factory);
    registerTableFunctionHudi(factory);
    registerTableFunctionDeltaLake(factory);
#if USE_AVRO
    registerTableFunctionIceberg(factory);
#endif

#endif

#if USE_HDFS
    registerTableFunctionHDFS(factory);
    registerTableFunctionHDFSCluster(factory);
#endif

#if USE_HIVE
    registerTableFunctionHive(factory);
#endif

    registerTableFunctionODBC(factory);
    registerTableFunctionJDBC(factory);

    registerTableFunctionView(factory);
    registerTableFunctionViewIfPermitted(factory);

#if USE_MYSQL
    registerTableFunctionMySQL(factory);
#endif

#if USE_LIBPQXX
    registerTableFunctionPostgreSQL(factory);
#endif

#if USE_SQLITE
    registerTableFunctionSQLite(factory);
#endif

    registerTableFunctionDictionary(factory);

    registerTableFunctionFormat(factory);
    registerTableFunctionExplain(factory);

#if USE_TANTIVY_SEARCH
    registerTableFunctionFtsIndex(factory);
#endif

    registerTableFunctionFullTextSearch(factory);
}

}
