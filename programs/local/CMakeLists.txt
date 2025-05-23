set (CLICKHOUSE_LOCAL_SOURCES LocalServer.cpp)

set (CLICKHOUSE_LOCAL_LINK
    PRIVATE
        boost::program_options
        clickhouse_aggregate_functions
        clickhouse_common_config
        clickhouse_common_io
        clickhouse_functions
        clickhouse_parsers
        clickhouse_storages_system
        clickhouse_table_functions
)

clickhouse_program_add(local)

target_link_libraries(clickhouse-local-lib PRIVATE clickhouse-server-lib)

# Need skim library in supercrate.
if (TARGET ch_rust::supercrate)
    target_link_libraries(clickhouse-local-lib PRIVATE ch_rust::supercrate)
endif()

if (TARGET ch_contrib::azure_sdk)
    target_link_libraries(clickhouse-local-lib PRIVATE ch_contrib::azure_sdk)
endif()

# Always use internal readpassphrase
target_link_libraries(clickhouse-local-lib PRIVATE readpassphrase)

if (ENABLE_FUZZING)
    add_compile_definitions(FUZZING_MODE=1)
    set (WITH_COVERAGE ON)
    target_link_libraries(clickhouse-local-lib PRIVATE ${LIB_FUZZING_ENGINE})
endif ()
