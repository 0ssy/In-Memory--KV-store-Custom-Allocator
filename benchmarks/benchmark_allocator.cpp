// benchmark_allocator.cpp - Allocator benchmarks
#include "kv_store/allocator.hpp"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <new>

using namespace kv_store;
using namespace std::chrono;

struct AllocBenchResult {
    std::string name;
    int threads;
    int operations;
    double duration_ms;
    double ops_per_sec;
    size_t bytes_allocated;
};

void print_alloc_result(const AllocBenchResult& r) {
    std::cout << std::left << std::setw(35) << r.name
              << " threads=" << std::setw(3) << r.threads
              << " ops=" << std::setw(10) << r.operations
              << " time=" << std::setw(10) << std::fixed << std::setprecision(2) << r.duration_ms << "ms"
              << " throughput=" << std::setw(12) << std::fixed << std::setprecision(0) << r.ops_per_sec << " ops/s"
              << " bytes=" << r.bytes_allocated << "\n";
}

AllocBenchResult benchmark_malloc(int num_ops, int alloc_size, int num_threads) {
    std::atomic<int> next_op{0};
    std::vector<void*> pointers(num_ops);
    
    auto worker = [&](int thread_id) {
        while (true) {
            int op = next_op.fetch_add(1, std::memory_order_relaxed);
            if (op >= num_ops) break;
            pointers[op] = std::malloc(alloc_size);
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    
    // Free
    for (void* p : pointers) {
        std::free(p);
    }
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    
    return {
        "malloc/free",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        static_cast<size_t>(num_ops) * alloc_size
    };
}

AllocBenchResult benchmark_new_delete(int num_ops, int alloc_size, int num_threads) {
    std::atomic<int> next_op{0};
    std::vector<void*> pointers(num_ops);
    
    auto worker = [&](int thread_id) {
        while (true) {
            int op = next_op.fetch_add(1, std::memory_order_relaxed);
            if (op >= num_ops) break;
            pointers[op] = ::operator new(alloc_size, std::align_val_t(alignof(std::max_align_t)));
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    
    // Delete
    for (void* p : pointers) {
        ::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
    }
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    
    return {
        "operator new/delete",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        static_cast<size_t>(num_ops) * alloc_size
    };
}

AllocBenchResult benchmark_arena_allocator(int num_ops, int alloc_size, int num_threads) {
    ArenaAllocator arena(1024 * 1024);
    std::atomic<int> next_op{0};
    std::vector<void*> pointers(num_ops);
    
    auto worker = [&](int thread_id) {
        while (true) {
            int op = next_op.fetch_add(1, std::memory_order_relaxed);
            if (op >= num_ops) break;
            pointers[op] = arena.allocate(alloc_size);
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    
    // Arena doesn't free individual allocations
    arena.reset();
    
    return {
        "ArenaAllocator (bump)",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        static_cast<size_t>(num_ops) * alloc_size
    };
}

AllocBenchResult benchmark_free_list_allocator(int num_ops, int alloc_size, int num_threads) {
    FreeListAllocator fl(alloc_size, num_ops / num_threads + 100);
    std::atomic<int> next_op{0};
    std::vector<void*> pointers(num_ops);
    
    auto worker = [&](int thread_id) {
        while (true) {
            int op = next_op.fetch_add(1, std::memory_order_relaxed);
            if (op >= num_ops) break;
            pointers[op] = fl.allocate(alloc_size);
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    
    // Deallocate
    for (void* p : pointers) {
        fl.deallocate(p, alloc_size);
    }
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    
    return {
        "FreeListAllocator",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        static_cast<size_t>(num_ops) * alloc_size
    };
}

AllocBenchResult benchmark_free_list_reuse(int num_ops, int alloc_size, int num_threads) {
    FreeListAllocator fl(alloc_size, 1000);
    std::atomic<int> next_op{0};
    
    auto worker = [&](int thread_id) {
        std::vector<void*> local_ptrs;
        local_ptrs.reserve(100);
        
        while (true) {
            int op = next_op.fetch_add(1, std::memory_order_relaxed);
            if (op >= num_ops) break;
            
            void* ptr = fl.allocate(alloc_size);
            local_ptrs.push_back(ptr);
            
            // Deallocate every 10 allocations
            if (local_ptrs.size() >= 10) {
                for (void* p : local_ptrs) {
                    fl.deallocate(p, alloc_size);
                }
                local_ptrs.clear();
            }
        }
        
        // Cleanup remaining
        for (void* p : local_ptrs) {
            fl.deallocate(p, alloc_size);
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    
    return {
        "FreeListAllocator (reuse)",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        static_cast<size_t>(num_ops) * alloc_size
    };
}

int main() {
    std::cout << "=== Allocator Benchmarks ===\n\n";
    
    std::vector<std::tuple<int, int, std::vector<int>>> test_configs = {
        {100000, 32, {1, 2, 4, 8}},
        {100000, 64, {1, 2, 4, 8}},
        {100000, 256, {1, 2, 4}},
        {50000, 1024, {1, 2, 4}},
    };
    
    for (auto [num_ops, alloc_size, thread_counts] : test_configs) {
        std::cout << "--- Alloc size: " << alloc_size << " bytes, Operations: " << num_ops << " ---\n";
        
        for (int threads : thread_counts) {
            // malloc/free
            auto r1 = benchmark_malloc(num_ops, alloc_size, threads);
            print_alloc_result(r1);
            
            // operator new/delete
            auto r2 = benchmark_new_delete(num_ops, alloc_size, threads);
            print_alloc_result(r2);
            
            // Arena allocator
            auto r3 = benchmark_arena_allocator(num_ops, alloc_size, threads);
            print_alloc_result(r3);
            
            // Free list allocator
            auto r4 = benchmark_free_list_allocator(num_ops, alloc_size, threads);
            print_alloc_result(r4);
            
            // Free list with reuse
            auto r5 = benchmark_free_list_reuse(num_ops, alloc_size, threads);
            print_alloc_result(r5);
            
            std::cout << "\n";
        }
    }
    
    std::cout << "=== Allocator Benchmarks Complete ===\n";
    return 0;
}