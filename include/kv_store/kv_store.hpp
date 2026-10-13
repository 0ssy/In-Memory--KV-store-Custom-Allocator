// kv_store.hpp - Main public API
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <chrono>

namespace kv_store {

enum class SyncMode {
    None,       // No sync - data in OS buffers
    Periodic,   // Sync periodically (configurable interval)
    Immediate   // fsync on every write
};

struct Config {
    std::string data_dir = "./data";
    size_t initial_table_size = 1024;
    double max_load_factor = 0.7;
    size_t num_shards = 16;
    SyncMode sync_mode = SyncMode::Periodic;
    std::chrono::milliseconds sync_interval{100};
    size_t wal_segment_size = 64 * 1024 * 1024; // 64MB
    bool enable_checksums = true;
};

class KVStore {
public:
    explicit KVStore(const Config& config = Config{});
    ~KVStore();

    // Non-copyable, movable
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;
    KVStore(KVStore&&) noexcept;
    KVStore& operator=(KVStore&&) noexcept;

    // Core operations
    // Returns true if inserted (new key), false if updated (existing key)
    // Durability depends on SyncMode: Immediate = durably recorded, others = in memory/OS buffer
    bool put(std::string_view key, std::string_view value);

    // Returns value if found, nullopt otherwise
    std::optional<std::string> get(std::string_view key) const;

    // Returns true if key existed and was deleted
    bool erase(std::string_view key);

    // Force sync to durable storage
    void sync();

    // Get current approximate size
    size_t size() const noexcept;

    // Check if store is healthy
    bool healthy() const noexcept;

    // Statistics
    struct Stats {
        uint64_t puts = 0;
        uint64_t gets = 0;
        uint64_t deletes = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t wal_writes = 0;
        uint64_t wal_syncs = 0;
        uint64_t resize_count = 0;
    };
    Stats stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace kv_store