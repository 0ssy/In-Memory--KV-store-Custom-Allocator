cmake_minimum_required(VERSION 3.20)
project(kv_store VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(ENABLE_SANITIZERS "Enable address/undefined behavior sanitizers" OFF)

# Main library
add_library(kv_store STATIC
    src/hash_table.cpp
    src/allocator.cpp
    src/wal.cpp
    src/sharded_store.cpp
    src/kv_store.cpp
)

target_include_directories(kv_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Compiler warnings - must be after add_library
if(MSVC)
    target_compile_options(kv_store PRIVATE /W4 /WX)
else()
    target_compile_options(kv_store PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
endif()

if(ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(kv_store PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kv_store PRIVATE -fsanitize=address,undefined)
endif()

# Tests - optional, only if GTest is available
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_subdirectory(tests)
    else()
        message(WARNING "GTest not found, skipping tests. Install GTest or set BUILD_TESTS=OFF")
    endif()
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Install
install(TARGETS kv_store
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include FILES_MATCHING PATTERN "*.hpp")