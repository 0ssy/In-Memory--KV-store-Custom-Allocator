// test_kv_store.cpp - Main KV Store API tests
#include "kv_store/kv_store.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace kv_store;

class KVStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "kv_store_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        
        config_.data_dir = test_dir_.string();
        config_.sync_mode = SyncMode::Immediate;
        config_.num_shards = 4;
        store_ = std::make_unique<KVStore>(config_);
    }
    
    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
    Config config_;
    std::unique_ptr<KVStore> store_;
};

TEST_F(KVStoreTest, BasicAPI) {
    // Put
    bool inserted = store_->put("key1", "value1");
    EXPECT_TRUE(inserted);
    
    // Get
    auto val = store_->get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value1");
    
    // Update
    inserted = store_->put("key1", "value2");
    EXPECT_FALSE(inserted); // Not inserted, updated
    
    val = store_->get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value2");
    
    // Erase
    bool erased = store_->erase("key1");
    EXPECT_TRUE(erased);
    
    val = store_->get("key1");
    EXPECT_FALSE(val.has_value());
    
    // Erase non-existent
    erased = store_->erase("key1");
    EXPECT_FALSE(erased);
}

TEST_F(KVStoreTest, Persistence) {
    store_->put("key1", "value1");
    store_->put("key2", "value2");
    store_->erase("key1");
    store_->sync();
    
    // Reopen
    auto store2 = std::make_unique<KVStore>(config_);
    
    EXPECT_FALSE(store2->get("key1").has_value());
    EXPECT_EQ(*store2->get("key2"), "value2");
    EXPECT_EQ(store2->size(), 1);
}

TEST_F(KVStoreTest, SyncModes) {
    // Test Immediate sync mode
    {
        Config cfg = config_;
        cfg.sync_mode = SyncMode::Immediate;
        KVStore store(cfg);
        store.put("key", "value");
        // Should be synced immediately
    }
    
    // Test Periodic sync mode
    {
        Config cfg = config_;
        cfg.sync_mode = SyncMode::Periodic;
        cfg.sync_interval = std::chrono::milliseconds(10);
        KVStore store(cfg);
        store.put("key", "value");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Should be synced by background thread
    }
    
    // Test None sync mode
    {
        Config cfg = config_;
        cfg.sync_mode = SyncMode::None;
        KVStore store(cfg);
        store.put("key", "value");
        // Not synced until explicit sync()
        store.sync();
    }
}

TEST_F(KVStoreTest, ConcurrentAccess) {
    const int num_threads = 8;
    const int ops_per_thread = 500;
    
    std::atomic<int> errors{0};
    std::atomic<int> successful_puts{0};
    std::atomic<int> successful_gets{0};
    
    auto writer = [&](int thread_id) {
        for (int i = 0; i < ops_per_thread; ++i) {
            std::string key = "key" + std::to_string(thread_id * ops_per_thread + i);
            std::string value = "value" + std::to_string(i);
            if (store_->put(key, value)) {
                ++successful_puts;
            } else {
                ++errors;
            }
        }
    };
    
    auto reader = [&]() {
        for (int i = 0; i < ops_per_thread * num_threads; ++i) {
            std::string key = "key" + std::to_string(i);
            auto val = store_->get(key);
            if (val.has_value()) {
                ++successful_gets;
            }
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(writer, i);
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(reader);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(successful_puts.load(), num_threads * ops_per_thread);
    EXPECT_EQ(store_->size(), num_threads * ops_per_thread);
}

TEST_F(KVStoreTest, Stats) {
    store_->put("key1", "value1");
    store_->put("key2", "value2");
    store_->get("key1");
    store_->get("nonexistent");
    store_->erase("key1");
    store_->sync();
    
    auto stats = store_->stats();
    EXPECT_EQ(stats.puts, 2);
    EXPECT_EQ(stats.gets, 2);
    EXPECT_EQ(stats.deletes, 1);
    EXPECT_GT(stats.wal_writes, 0);
    EXPECT_GT(stats.wal_syncs, 0);
}

TEST_F(KVStoreTest, MoveSemantics) {
    store_->put("key1", "value1");
    
    KVStore store2 = std::move(*store_);
    store_ = std::make_unique<KVStore>(std::move(store2));
    
    auto val = store_->get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value1");
}

TEST_F(KVStoreTest, LargeData) {
    std::string large_key(5000, 'k');
    std::string large_value(50000, 'v');
    
    EXPECT_TRUE(store_->put(large_key, large_value));
    
    auto val = store_->get(large_key);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->size(), 50000);
}

TEST_F(KVStoreTest, EmptyStrings) {
    EXPECT_TRUE(store_->put("", "empty_key"));
    EXPECT_TRUE(store_->put("empty_value", ""));
    EXPECT_TRUE(store_->put("", ""));
    
    EXPECT_EQ(*store_->get(""), "");
    EXPECT_EQ(*store_->get("empty_value"), "");
    
    EXPECT_TRUE(store_->erase(""));
    EXPECT_FALSE(store_->get("").has_value());
}

TEST_F(KVStoreTest, StressTest) {
    // Mixed workload
    const int num_operations = 10000;
    
    for (int i = 0; i < num_operations; ++i) {
        std::string key = "key" + std::to_string(i % 1000);
        std::string value = "value" + std::to_string(i);
        
        if (i % 3 == 0) {
            store_->put(key, value);
        } else if (i % 3 == 1) {
            store_->get(key);
        } else {
            store_->erase(key);
        }
    }
    
    store_->sync();
    EXPECT_GT(store_->size(), 0);
}

TEST_F(KVStoreTest, ConfigDefaults) {
    KVStore store(Config{}); // Default config
    
    EXPECT_TRUE(store.put("key", "value"));
    EXPECT_EQ(*store.get("key"), "value");
}

TEST_F(KVStoreTest, HealthCheck) {
    EXPECT_TRUE(store_->healthy());
    store_->put("key", "value");
    EXPECT_TRUE(store_->healthy());
}

TEST_F(KVStoreTest, CustomShardCount) {
    Config cfg = config_;
    cfg.num_shards = 1;
    KVStore store(cfg);
    
    for (int i = 0; i < 100; ++i) {
        store.put("key" + std::to_string(i), "value");
    }
    
    EXPECT_EQ(store.size(), 100);
}