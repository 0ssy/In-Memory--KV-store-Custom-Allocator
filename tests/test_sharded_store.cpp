// test_sharded_store.cpp - Sharded store tests
#include "kv_store/sharded_store.hpp"
#include "kv_store/wal.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <filesystem>

using namespace kv_store;

class ShardedStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "kv_store_sharded_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        
        wal_config_.directory = test_dir_.string();
        wal_config_.sync_on_write = true;
        wal_ = std::make_unique<WriteAheadLog>(wal_config_);
        
        shard_config_.num_shards = 4;
        shard_config_.initial_table_size = 16;
        store_ = std::make_unique<ShardedStore>(shard_config_, wal_.get());
    }
    
    void TearDown() override {
        store_.reset();
        wal_.reset();
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
    WriteAheadLog::Config wal_config_;
    ShardConfig shard_config_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<ShardedStore> store_;
};

TEST_F(ShardedStoreTest, BasicPutGet) {
    EXPECT_TRUE(store_->put("key1", "value1"));
    
    auto val = store_->get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value1");
    
    EXPECT_FALSE(store_->get("nonexistent").has_value());
}

TEST_F(ShardedStoreTest, UpdateExistingKey) {
    store_->put("key", "value1");
    store_->put("key", "value2");
    
    auto val = store_->get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value2");
    
    EXPECT_EQ(store_->size(), 1);
}

TEST_F(ShardedStoreTest, Delete) {
    store_->put("key1", "value1");
    store_->put("key2", "value2");
    
    EXPECT_TRUE(store_->erase("key1"));
    EXPECT_FALSE(store_->get("key1").has_value());
    EXPECT_TRUE(store_->get("key2").has_value());
    
    EXPECT_FALSE(store_->erase("key1")); // Already deleted
    EXPECT_FALSE(store_->erase("nonexistent"));
    
    EXPECT_EQ(store_->size(), 1);
}

TEST_F(ShardedStoreTest, PersistenceAndRecovery) {
    store_->put("key1", "value1");
    store_->put("key2", "value2");
    store_->erase("key1");
    store_->sync();
    
    // Create new store with same WAL
    auto store2 = std::make_unique<ShardedStore>(shard_config_, wal_.get());
    
    EXPECT_EQ(store2->size(), 1);
    EXPECT_FALSE(store2->get("key1").has_value());
    EXPECT_EQ(*store2->get("key2"), "value2");
}

TEST_F(ShardedStoreTest, ShardDistribution) {
    // Insert many keys
    for (int i = 0; i < 100; ++i) {
        store_->put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    // Check all shards have some data
    auto stats = store_->stats();
    int non_empty_shards = 0;
    for (const auto& shard : stats.shards) {
        if (shard.size > 0) ++non_empty_shards;
    }
    
    EXPECT_GT(non_empty_shards, 1); // Should be distributed
    EXPECT_EQ(store_->size(), 100);
}

TEST_F(ShardedStoreTest, ConcurrentReadWrite) {
    // Pre-populate
    for (int i = 0; i < 100; ++i) {
        store_->put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    std::atomic<int> errors{0};
    std::atomic<int> reads{0};
    std::atomic<int> writes{0};
    
    auto reader = [&]() {
        for (int i = 0; i < 1000; ++i) {
            auto val = store_->get("key" + std::to_string(i % 100));
            if (!val.has_value()) ++errors;
            ++reads;
        }
    };
    
    auto writer = [&]() {
        for (int i = 0; i < 1000; ++i) {
            std::string key = "new_key" + std::to_string(i);
            if (!store_->put(key, "value")) ++errors;
            ++writes;
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(reader);
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(writer);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(reads.load(), 4000);
    EXPECT_EQ(writes.load(), 2000);
}

TEST_F(ShardedStoreTest, ConcurrentErase) {
    // Pre-populate
    for (int i = 0; i < 200; ++i) {
        store_->put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    std::atomic<int> errors{0};
    std::atomic<int> erases{0};
    
    auto eraser = [&](int start, int end) {
        for (int i = start; i < end; ++i) {
            if (!store_->erase("key" + std::to_string(i))) {
                // Key might have been deleted by another thread
            }
            ++erases;
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(eraser, i * 50, (i + 1) * 50);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(erases.load(), 200);
    EXPECT_EQ(store_->size(), 0);
}

TEST_F(ShardedStoreTest, LargeKeysAndValues) {
    std::string large_key(1000, 'k');
    std::string large_value(10000, 'v');
    
    EXPECT_TRUE(store_->put(large_key, large_value));
    
    auto val = store_->get(large_key);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->size(), 10000);
}

TEST_F(ShardedStoreTest, Sync) {
    store_->put("key1", "value1");
    store_->sync(); // Should not crash
    
    auto stats = store_->stats();
    EXPECT_GT(stats.wal_syncs, 0);
}

TEST_F(ShardedStoreTest, HealthyFlag) {
    EXPECT_TRUE(store_->healthy());
    
    // Cause a WAL error by corrupting the file
    wal_->sync();
    std::filesystem::path segment = WriteAheadLog::segment_path(test_dir_.string(), 0);
    std::filesystem::resize_file(segment, 0); // Truncate
    
    // Next write should fail
    EXPECT_FALSE(store_->put("key", "value"));
    EXPECT_FALSE(store_->healthy());
}

TEST_F(ShardedStoreTest, Stats) {
    store_->put("key1", "value1");
    store_->put("key2", "value2");
    store_->get("key1");
    store_->get("nonexistent");
    store_->erase("key1");
    
    auto stats = store_->stats();
    EXPECT_EQ(stats.puts, 2);
    EXPECT_EQ(stats.gets, 2);
    EXPECT_EQ(stats.deletes, 1);
    EXPECT_GT(stats.wal_writes, 0);
}

TEST_F(ShardedStoreTest, ManyShards) {
    shard_config_.num_shards = 64;
    shard_config_.initial_table_size = 8;
    auto store = std::make_unique<ShardedStore>(shard_config_, wal_.get());
    
    for (int i = 0; i < 1000; ++i) {
        store->put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    EXPECT_EQ(store->size(), 1000);
    
    auto stats = store->stats();
    EXPECT_EQ(stats.shards.size(), 64);
}

TEST_F(ShardedStoreTest, SingleShard) {
    shard_config_.num_shards = 1;
    auto store = std::make_unique<ShardedStore>(shard_config_, wal_.get());
    
    store->put("key1", "value1");
    store->put("key2", "value2");
    
    EXPECT_EQ(store->size(), 2);
    EXPECT_EQ(*store->get("key1"), "value1");
}