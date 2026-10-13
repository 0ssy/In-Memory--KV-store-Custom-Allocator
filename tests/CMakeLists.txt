# Tests CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# Test executable
add_executable(kv_store_tests
    test_hash_table.cpp
    test_allocator.cpp
    test_wal.cpp
    test_sharded_store.cpp
    test_kv_store.cpp
    test_main.cpp
)

target_link_libraries(kv_store_tests PRIVATE kv_store GTest::GTest GTest::Main)

add_test(NAME kv_store_tests COMMAND kv_store_tests)

# Run tests automatically on build
add_custom_target(run_tests ALL COMMAND kv_store_tests DEPENDS kv_store_tests)