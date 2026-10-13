// benchmark_main.cpp - Benchmark entry point
// This file allows selecting which benchmark to run
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: kv_store_benchmarks <benchmark_name>\n";
        std::cout << "Available benchmarks:\n";
        std::cout << "  kv_store    - KV Store benchmarks\n";
        std::cout << "  allocator   - Allocator benchmarks\n";
        return 1;
    }
    
    std::string benchmark = argv[1];
    
    if (benchmark == "kv_store") {
        // Run KV store benchmarks
        // This would normally call the benchmark function from benchmark_kv_store.cpp
        std::cout << "Run KV Store benchmarks by linking with benchmark_kv_store.cpp\n";
        return 0;
    } else if (benchmark == "allocator") {
        std::cout << "Run Allocator benchmarks by linking with benchmark_allocator.cpp\n";
        return 0;
    } else {
        std::cout << "Unknown benchmark: " << benchmark << "\n";
        return 1;
    }
}