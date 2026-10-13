// sharded_store.hpp - Fix: include missing headers
#pragma once

#include "kv_store/hash_table.hpp"
#include "kv_store/allocator.hpp"
#include "kv_store/wal.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <thread>

namespace kv_store {

// Shard configuration
struct ShardConfig {
    size_t num_shards = 16;
    size_t initial_table_size = 1024;
    double max_load_factor = 0.7;
};

// Statistics for the sharded store
struct ShardedStoreStats {
    uint64_t puts = 0;
    uint64_t gets = 0;
    uint64_t deletes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t resizes = 0;
    uint64_t wal_writes = 0;
    uint64_t wal_syncs = 0;
    uint64_t lock_contentions = 0;
    
    // Per-shard stats
    struct ShardStat {
        size_t size = 0;
        size_t capacity = 0;
        uint64_t puts = 0;
        uint64_t gets = 0;
        uint64_t deletes = 0;
    };
    std::vector<ShardStat> shards;
};

// Forward declaration
class WriteAheadLog;

// Thread-safe sharded key-value store
class ShardedStore {
public:
    using HashTableType = HashTable<std::string, std::string>;
    
    explicit ShardedStore(const ShardConfig& config = ShardConfig{}, 
                          WriteAheadLog* wal = nullptr);
    ~ShardedStore();
    
    ShardedStore(const ShardedStore&) = delete;
    ShardedStore& operator=(const ShardedStore&) = delete;
    
    // Core operations
    bool put(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key) const;
    bool erase(std::string_view key);
    
    // Sync WAL
    void sync();
    
    // Get total size
    size_t size() const noexcept;
    
    // Get stats
    ShardedStoreStats stats() const;
    
    // Replay WAL to rebuild state
    bool replay_wal();
    
    // Check health
    bool healthy() const noexcept;
    
    // Get shard for a key
    size_t shard_for_key(std::string_view key) const noexcept;
    
private:
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unique_ptr<HashTableType> table;
        uint64_t puts = 0;
        uint64_t gets = 0;
        uint64_t deletes = 0;
        uint64_t resizes = 0;
    };
    
    const ShardConfig config_;
    WriteAheadLog* wal_;
    std::vector<Shard> shards_;
    std::atomic<uint64_t> sequence_{1};
    std::atomic<bool> healthy_{true};
    
    // Hash function for sharding
    size_t hash_key(std::string_view key) const noexcept;
};

} // namespace kv_store