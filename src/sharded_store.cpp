// sharded_store.cpp - Sharded store implementation
#include "kv_store/sharded_store.hpp"
#include "kv_store/wal.hpp"
#include <algorithm>

namespace kv_store {

ShardedStore::ShardedStore(const ShardConfig& config, WriteAheadLog* wal)
    : config_(config), wal_(wal), shards_(config.num_shards) {
    for (auto& shard : shards_) {
        shard.table = std::make_unique<HashTableType>(config.initial_table_size, config.max_load_factor);
    }
}

ShardedStore::~ShardedStore() = default;

size_t ShardedStore::hash_key(std::string_view key) const noexcept {
    // FNV-1a hash
    constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    
    uint64_t hash = FNV_OFFSET_BASIS;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= FNV_PRIME;
    }
    return static_cast<size_t>(hash);
}

size_t ShardedStore::shard_for_key(std::string_view key) const noexcept {
    return hash_key(key) % shards_.size();
}

bool ShardedStore::put(std::string_view key, std::string_view value) {
    size_t shard_idx = shard_for_key(key);
    Shard& shard = shards_[shard_idx];
    
    std::unique_lock lock(shard.mutex);
    
    uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
    
    // Write to WAL first (if enabled)
    if (wal_) {
        if (!wal_->append(seq, WalOpType::Put, key, value)) {
            healthy_.store(false, std::memory_order_relaxed);
            return false;
        }
    }
    
    // Update in-memory table
    auto [entry, inserted] = shard.table->insert_or_assign(std::string(key), std::string(value));
    (void)entry; (void)inserted;
    
    ++shard.puts;
    return true;
}

std::optional<std::string> ShardedStore::get(std::string_view key) const {
    size_t shard_idx = shard_for_key(key);
    const Shard& shard = shards_[shard_idx];
    
    std::shared_lock lock(shard.mutex);
    
    auto* entry = shard.table->find(std::string(key));
    if (entry) {
        return entry->value;
    }
    return std::nullopt;
}

bool ShardedStore::erase(std::string_view key) {
    size_t shard_idx = shard_for_key(key);
    Shard& shard = shards_[shard_idx];
    
    std::unique_lock lock(shard.mutex);
    
    uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
    
    // Write to WAL first (if enabled)
    if (wal_) {
        if (!wal_->append(seq, WalOpType::Delete, key)) {
            healthy_.store(false, std::memory_order_relaxed);
            return false;
        }
    }
    
    // Remove from in-memory table
    bool erased = shard.table->erase(std::string(key));
    
    if (erased) {
        ++shard.deletes;
    }
    
    return erased;
}

void ShardedStore::sync() {
    if (wal_) {
        wal_->sync();
    }
}

size_t ShardedStore::size() const noexcept {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard.mutex);
        total += shard.table->size();
    }
    return total;
}

ShardedStoreStats ShardedStore::stats() const {
    ShardedStoreStats result;
    result.shards.resize(shards_.size());
    
    for (size_t i = 0; i < shards_.size(); ++i) {
        const Shard& shard = shards_[i];
        std::shared_lock lock(shard.mutex);
        
        result.puts += shard.puts;
        result.gets += shard.gets;
        result.deletes += shard.deletes;
        result.resizes += shard.resizes;
        
        result.shards[i].size = shard.table->size();
        result.shards[i].capacity = shard.table->capacity();
        result.shards[i].puts = shard.puts;
        result.shards[i].gets = shard.gets;
        result.shards[i].deletes = shard.deletes;
    }
    
    if (wal_) {
        auto wal_stats = wal_->stats();
        result.wal_writes = wal_stats.records_written;
        result.wal_syncs = wal_stats.syncs;
    }
    
    return result;
}

bool ShardedStore::replay_wal() {
    if (!wal_) return true;
    
    bool success = wal_->replay([this](const WalRecord& record) -> bool {
        size_t shard_idx = shard_for_key(record.key());
        Shard& shard = shards_[shard_idx];
        
        std::unique_lock lock(shard.mutex);
        
        switch (record.header().op_type) {
            case WalOpType::Put:
                shard.table->insert_or_assign(std::string(record.key()), std::string(record.value()));
                ++shard.puts;
                break;
            case WalOpType::Delete:
                shard.table->erase(std::string(record.key()));
                ++shard.deletes;
                break;
            case WalOpType::Checkpoint:
                // No-op for checkpoint
                break;
        }
        
        // Update sequence
        sequence_.store(record.header().sequence + 1, std::memory_order_relaxed);
        return true;
    });
    
    if (!success) {
        healthy_.store(false, std::memory_order_relaxed);
    }
    
    return success;
}

bool ShardedStore::healthy() const noexcept {
    return healthy_.load(std::memory_order_relaxed);
}

} // namespace kv_store