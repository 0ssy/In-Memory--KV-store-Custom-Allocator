// benchmark_kv_store.cpp - KV Store benchmarks
#include "kv_store/kv_store.hpp"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <random>
#include <filesystem>

using namespace kv_store;
using namespace std::chrono;

struct BenchmarkResult {
    const char* name;
    int threads;
    int operations;
    double duration_ms;
    double ops_per_sec;
    size_t final_size;
};

void print_result(const BenchmarkResult& r) {
    std::cout << std::left << std::setw(30) << r.name
              << " threads=" << std::setw(3) << r.threads
              << " ops=" << std::setw(10) << r.operations
              << " time=" << std::setw(10) << std::fixed << std::setprecision(2) << r.duration_ms << "ms"
              << " throughput=" << std::setw(12) << std::fixed << std::setprecision(0) << r.ops_per_sec << " ops/s"
              << " size=" << r.final_size << "\n";
}

std::string random_string(size_t len, std::mt19937& rng) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        result += charset[dist(rng)];
    }
    return result;
}

BenchmarkResult run_insert_benchmark(kv_store::Config config, int num_ops, int key_size, int value_size, int num_threads) {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "kv_bench_insert";
    std::filesystem::remove_all(test_dir);
    config.data_dir = test_dir.string();
    
    std::cout << "  [DEBUG] Creating KVStore at: " << test_dir.string() << std::endl;
    
    size_t final_size = 0;
    double ms = 0;
    
    {
        KVStore store(config);
        std::cout << "  [DEBUG] KVStore created successfully" << std::endl;
        std::cout << "  [DEBUG] Store size: " << store.size() << std::endl;
        
        std::vector<std::string> keys(num_ops);
        std::vector<std::string> values(num_ops);
        std::cout << "  [DEBUG] Vectors allocated" << std::endl;
        
        std::mt19937 rng(42);
        std::cout << "  [DEBUG] RNG created" << std::endl;
        
        for (int i = 0; i < num_ops; ++i) {
            keys[i] = random_string(key_size, rng);
            values[i] = random_string(value_size, rng);
        }
        std::cout << "  [DEBUG] Keys/values generated" << std::endl;
        
        std::atomic<int> next_op{0};
        std::atomic<int> errors{0};
        std::cout << "  [DEBUG] Atomics created" << std::endl;
        
        auto worker = [&](int thread_id) {
            std::cout << "  [DEBUG] Worker " << thread_id << " started" << std::endl;
            while (true) {
                int op = next_op.fetch_add(1, std::memory_order_relaxed);
                if (op >= num_ops) break;
                
                if (!store.put(keys[op], values[op])) {
                    ++errors;
                }
            }
            std::cout << "  [DEBUG] Worker " << thread_id << " finished" << std::endl;
        };
        
        std::cout << "  [DEBUG] Worker lambda created" << std::endl;
        
        auto start = high_resolution_clock::now();
        std::cout << "  [DEBUG] Starting threads" << std::endl;
        
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker, i);
            std::cout << "  [DEBUG] Thread " << i << " created" << std::endl;
        }
        for (auto& t : threads) {
            t.join();
            std::cout << "  [DEBUG] Thread joined" << std::endl;
        }
        std::cout << "  [DEBUG] All threads joined" << std::endl;
        
        store.sync();
        std::cout << "  [DEBUG] Store synced" << std::endl;
        
        auto end = high_resolution_clock::now();
        std::cout << "  [DEBUG] End time recorded" << std::endl;
        
        ms = duration_cast<microseconds>(end - start).count() / 1000.0;
        std::cout << "  [DEBUG] Duration calculated: " << ms << "ms" << std::endl;
        
        final_size = store.size();
        std::cout << "  [DEBUG] Final size: " << final_size << std::endl;
    } // store destroyed here
    std::cout << "  [DEBUG] Nested scope exited, vectors destroyed" << std::endl;
    std::cout.flush();
    
    std::cout << "  [DEBUG] Store destroyed, removing test dir" << std::endl;
    std::cout << "  [DEBUG] About to remove test dir: " << test_dir.string() << std::endl;
    try {
        std::filesystem::remove_all(test_dir);
        std::cout << "  [DEBUG] Test dir removed" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  [DEBUG] Exception removing dir: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "  [DEBUG] Unknown exception removing dir" << std::endl;
    }
    std::cout << "  [DEBUG] Benchmark completed, size=" << final_size << std::endl;
    std::cout << "  [DEBUG] About to return result" << std::endl;
    std::cout.flush();
    
    BenchmarkResult result = {
        "Insert",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        final_size
    };
    std::cout << "  [DEBUG] Result created, returning" << std::endl;
    std::cout.flush();
    return result;
}

BenchmarkResult run_lookup_benchmark(kv_store::Config config, int num_ops, int key_size, int value_size, int num_threads) {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "kv_bench_lookup";
    std::filesystem::remove_all(test_dir);
    config.data_dir = test_dir.string();
    
    size_t final_size = 0;
    double ms = 0;
    
    {
        KVStore store(config);
        
        // Pre-populate
        std::mt19937 rng(42);
        for (int i = 0; i < num_ops; ++i) {
            store.put(random_string(key_size, rng), random_string(value_size, rng));
        }
        store.sync();
        
        // Generate lookup keys (mix of existing and non-existing)
        std::vector<std::string> lookup_keys(num_ops);
        std::uniform_int_distribution<> dist(0, num_ops - 1);
        for (int i = 0; i < num_ops; ++i) {
            if (i % 3 == 0) {
                lookup_keys[i] = "nonexistent_" + std::to_string(i);
            } else {
                int idx = dist(rng);
                // Re-generate the same key
                std::mt19937 rng2(42);
                for (int j = 0; j <= idx; ++j) {
                    lookup_keys[i] = random_string(key_size, rng2);
                }
            }
        }
        
        std::atomic<int> next_op{0};
        std::atomic<int> hits{0};
        std::atomic<int> misses{0};
        
        auto worker = [&](int thread_id) {
            while (true) {
                int op = next_op.fetch_add(1, std::memory_order_relaxed);
                if (op >= num_ops) break;
                
                auto val = store.get(lookup_keys[op]);
                if (val.has_value()) ++hits;
                else ++misses;
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
        ms = duration_cast<microseconds>(end - start).count() / 1000.0;
        
        final_size = store.size();
    } // store destroyed here
    
    try {
        std::filesystem::remove_all(test_dir);
    } catch (...) {}
    
    return {
        "Lookup",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        final_size
    };
}

BenchmarkResult run_mixed_benchmark(kv_store::Config config, int num_ops, int key_size, int value_size, int num_threads) {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "kv_bench_mixed";
    std::filesystem::remove_all(test_dir);
    config.data_dir = test_dir.string();
    
    size_t final_size = 0;
    double ms = 0;
    
    {
        KVStore store(config);
        
        // Pre-populate with some data
        std::mt19937 rng(42);
        for (int i = 0; i < num_ops / 10; ++i) {
            store.put(random_string(key_size, rng), random_string(value_size, rng));
        }
        store.sync();
        
        std::atomic<int> next_op{0};
        std::atomic<int> puts{0};
        std::atomic<int> gets{0};
        std::atomic<int> deletes{0};
        std::atomic<int> errors{0};
        
        auto worker = [&](int thread_id) {
            std::mt19937 local_rng(42 + thread_id);
            while (true) {
                int op = next_op.fetch_add(1, std::memory_order_relaxed);
                if (op >= num_ops) break;
                
                std::string key = random_string(key_size, local_rng);
                std::string value = random_string(value_size, local_rng);
                
                int op_type = op % 100;
                if (op_type < 60) { // 60% reads
                    store.get(key);
                    ++gets;
                } else if (op_type < 90) { // 30% writes
                    if (store.put(key, value)) ++puts;
                    else ++errors;
                } else { // 10% deletes
                    if (store.erase(key)) ++deletes;
                }
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
        
        store.sync();
        
        auto end = high_resolution_clock::now();
        ms = duration_cast<microseconds>(end - start).count() / 1000.0;
        
        final_size = store.size();
    } // store destroyed here
    
    try {
        std::filesystem::remove_all(test_dir);
    } catch (...) {}
    
    return {
        "Mixed (60/30/10)",
        num_threads,
        num_ops,
        ms,
        num_ops / (ms / 1000.0),
        final_size
    };
}

int main() {
    std::cout << "=== KV Store Benchmarks ===\n\n";
    
    // Test configurations
    struct BenchConfig {
        std::string name;
        int num_ops;
        int key_size;
        int value_size;
        std::vector<int> thread_counts;
        kv_store::SyncMode sync_mode;
    };
    
    std::vector<BenchConfig> configs = {
        {"Small (16B/16B)", 1000, 16, 16, {1}, kv_store::SyncMode::None},
    };
    
    for (const auto& cfg : configs) {
        std::cout << "--- " << cfg.name << " ---\n";
        
        for (int threads : cfg.thread_counts) {
            kv_store::Config config;
            config.sync_mode = cfg.sync_mode;
            config.num_shards = std::max(4, threads * 2);
            
            std::cout << "[MAIN] Running insert benchmark with " << threads << " threads\n" << std::flush;
            // Insert benchmark
            auto r1 = run_insert_benchmark(config, cfg.num_ops, cfg.key_size, cfg.value_size, threads);
            std::cout << "[MAIN] Insert benchmark returned\n" << std::flush;
            print_result(r1);
            std::cout << "[MAIN] Insert result printed\n" << std::flush;
            
            std::cout << "[MAIN] Running lookup benchmark\n" << std::flush;
            // Lookup benchmark
            auto r2 = run_lookup_benchmark(config, cfg.num_ops, cfg.key_size, cfg.value_size, threads);
            std::cout << "[MAIN] Lookup benchmark returned\n" << std::flush;
            print_result(r2);
            std::cout << "[MAIN] Lookup result printed\n" << std::flush;
            
            std::cout << "[MAIN] Running mixed benchmark\n" << std::flush;
            // Mixed benchmark
            auto r3 = run_mixed_benchmark(config, cfg.num_ops, cfg.key_size, cfg.value_size, threads);
            std::cout << "[MAIN] Mixed benchmark returned\n" << std::flush;
            print_result(r3);
            std::cout << "[MAIN] Mixed result printed\n" << std::flush;
        }
        std::cout << "\n";
    }
    
    // Test with sync enabled
    std::cout << "--- With Immediate Sync (16B/16B) ---\n";
    for (int threads : {1, 2, 4}) {
        kv_store::Config config;
        config.sync_mode = kv_store::SyncMode::Immediate;
        config.num_shards = std::max(4, threads * 2);
        
        auto r = run_insert_benchmark(config, 50000, 16, 16, threads);
        print_result(r);
    }
    
    std::cout << "\n=== Benchmarks Complete ===\n";
    return 0;
}