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
