// kv_store.cpp - Main KV Store implementation
#include "kv_store/kv_store.hpp"
#include "kv_store/sharded_store.hpp"
#include "kv_store/wal.hpp"
#include <memory>

namespace kv_store {

class KVStore::Impl {
public:
    explicit Impl(const Config& config) : config_(config) {
        // Initialize WAL
        WriteAheadLog::Config wal_config;
        wal_config.directory = config.data_dir;
        wal_config.segment_size = config.wal_segment_size;
        wal_config.enable_checksums = config.enable_checksums;
        wal_config.sync_on_write = (config.sync_mode == SyncMode::Immediate);
        
        wal_ = std::make_unique<WriteAheadLog>(wal_config);
        
        // Initialize sharded store
        ShardConfig shard_config;
        shard_config.num_shards = config.num_shards;
        shard_config.initial_table_size = config.initial_table_size;
        shard_config.max_load_factor = config.max_load_factor;
        
        store_ = std::make_unique<ShardedStore>(shard_config, wal_.get());
        
        // Replay WAL to rebuild state
        store_->replay_wal();
        
        // Start periodic sync thread if needed
        if (config.sync_mode == SyncMode::Periodic) {
            stop_sync_thread_ = false;
            sync_thread_ = std::thread([this] { sync_loop(); });
        }
    }
    
    ~Impl() {
        if (config_.sync_mode == SyncMode::Periodic) {
            stop_sync_thread_ = true;
            sync_cv_.notify_one();
            if (sync_thread_.joinable()) {
                sync_thread_.join();
            }
        }
        // Final sync
        if (wal_) {
            wal_->sync();
        }
    }
    
    bool put(std::string_view key, std::string_view value) {
        return store_->put(key, value);
    }
    
    std::optional<std::string> get(std::string_view key) const {
        return store_->get(key);
    }
    
    bool erase(std::string_view key) {
        return store_->erase(key);
    }
    
    void sync() {
        store_->sync();
    }
    
    size_t size() const noexcept {
        return store_->size();
    }
    
    bool healthy() const noexcept {
        return store_->healthy();
    }
    
    KVStore::Stats stats() const noexcept {
        auto s = store_->stats();
        KVStore::Stats result;
        result.puts = s.puts;
        result.gets = s.gets;
        result.deletes = s.deletes;
        result.hits = s.hits;
        result.misses = s.misses;
        result.wal_writes = s.wal_writes;
        result.wal_syncs = s.wal_syncs;
        result.resize_count = s.resizes;
        return result;
    }
    
private:
    void sync_loop() {
        std::unique_lock<std::mutex> lock(sync_mutex_);
        while (!stop_sync_thread_) {
            if (sync_cv_.wait_for(lock, config_.sync_interval, [this] { return stop_sync_thread_; })) {
                break; // Stop requested
            }
            if (!stop_sync_thread_ && wal_) {
                wal_->sync();
            }
        }
    }
    
    Config config_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<ShardedStore> store_;
    std::thread sync_thread_;
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;
    bool stop_sync_thread_{false};
};

KVStore::KVStore(const Config& config) : pimpl_(std::make_unique<Impl>(config)) {}

KVStore::~KVStore() = default;

KVStore::KVStore(KVStore&&) noexcept = default;
KVStore& KVStore::operator=(KVStore&&) noexcept = default;

bool KVStore::put(std::string_view key, std::string_view value) {
    return pimpl_->put(key, value);
}

std::optional<std::string> KVStore::get(std::string_view key) const {
    return pimpl_->get(key);
}

bool KVStore::erase(std::string_view key) {
    return pimpl_->erase(key);
}

void KVStore::sync() {
    pimpl_->sync();
}

size_t KVStore::size() const noexcept {
    return pimpl_->size();
}

bool KVStore::healthy() const noexcept {
    return pimpl_->healthy();
}

KVStore::Stats KVStore::stats() const noexcept {
    return pimpl_->stats();
}

} // namespace kv_store