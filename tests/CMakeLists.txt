add_executable(miniredis_ds_test
    ds/dict_test.cpp
    ds/listpack_test.cpp
    ds/intset_test.cpp
    ds/quicklist_test.cpp
    ds/skiplist_test.cpp
)
target_link_libraries(miniredis_ds_test PRIVATE miniredis_ds GTest::gtest_main)
target_compile_features(miniredis_ds_test PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(miniredis_ds_test)

add_executable(miniredis_types_test
    types/numeric_parse_test.cpp
    types/string_value_test.cpp
    types/list_value_test.cpp
    types/set_value_test.cpp
    types/hash_value_test.cpp
    types/zset_value_test.cpp
    types/encoding_upgrade_test.cpp
)
target_link_libraries(miniredis_types_test PRIVATE miniredis_types GTest::gtest_main)
target_compile_features(miniredis_types_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_types_test)

add_executable(miniredis_core_test
    core/resp_protocol_test.cpp
    core/config_test.cpp
    core/database_test.cpp
    core/client_test.cpp
    core/server_test.cpp
)
target_link_libraries(miniredis_core_test PRIVATE miniredis_core GTest::gtest_main)
target_compile_features(miniredis_core_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_core_test)

add_executable(miniredis_commands_test
    commands/registry_test.cpp
    commands/dispatcher_test.cpp
    commands/command_helpers_test.cpp
    commands/string_commands_test.cpp
    commands/list_commands_test.cpp
    commands/set_commands_test.cpp
    commands/hash_commands_test.cpp
    commands/zset_commands_test.cpp
    commands/key_commands_test.cpp
    commands/server_commands_test.cpp
)
target_link_libraries(miniredis_commands_test PRIVATE miniredis_commands GTest::gtest_main)
target_compile_features(miniredis_commands_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_commands_test)

add_executable(miniredis_io_test
    io/epoll_context_test.cpp
    io/async_io_test.cpp
    io/cmd_context_test.cpp
    io/client_session_test.cpp
    io/miniredis_tcp_integration_test.cpp
)
target_link_libraries(miniredis_io_test PRIVATE miniredis_io GTest::gtest_main)
target_compile_features(miniredis_io_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_io_test PROPERTIES TIMEOUT 60)

add_executable(miniredis_persistence_test
    persistence/crc64_test.cpp
    persistence/rdb_test.cpp
    persistence/aof_test.cpp
)
target_link_libraries(miniredis_persistence_test PRIVATE miniredis_persistence GTest::gtest_main)
target_compile_features(miniredis_persistence_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_persistence_test)

add_executable(miniredis_eviction_test
    eviction/expire_test.cpp
)
target_link_libraries(miniredis_eviction_test PRIVATE miniredis_eviction miniredis_core GTest::gtest_main)
target_compile_features(miniredis_eviction_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_eviction_test)

add_executable(miniredis_eviction_bench
    eviction/eviction_bench.cpp
)
target_link_libraries(miniredis_eviction_bench PRIVATE miniredis_eviction miniredis_core)
target_compile_features(miniredis_eviction_bench PRIVATE cxx_std_20)
