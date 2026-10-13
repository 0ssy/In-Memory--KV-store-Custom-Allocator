# benchmarks/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# KV Store Benchmark executable
add_executable(kv_store_benchmark
    benchmark_kv_store.cpp
)

target_link_libraries(kv_store_benchmark PRIVATE kv_store)

# Allocator Benchmark executable
add_executable(allocator_benchmark
    benchmark_allocator.cpp
)

target_link_libraries(allocator_benchmark PRIVATE kv_store)

# Add benchmark targets
add_custom_target(run_kv_store_benchmarks ALL COMMAND kv_store_benchmark DEPENDS kv_store_benchmark)
add_custom_target(run_allocator_benchmarks ALL COMMAND allocator_benchmark DEPENDS allocator_benchmark)