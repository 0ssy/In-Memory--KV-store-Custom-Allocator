// test_hash_table.cpp - Hash table tests
#include "kv_store/hash_table.hpp"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>
#include <random>

using namespace kv_store;

TEST(HashTableTest, BasicInsertAndLookup) {
    HashTable<std::string, std::string> ht(16);
    
    EXPECT_TRUE(ht.insert_or_assign("key1", "value1").second);
    EXPECT_FALSE(ht.insert_or_assign("key1", "new_value1").second);
    
    auto val = ht.get("key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "new_value1");
    
    EXPECT_FALSE(ht.get("nonexistent").has_value());
}

TEST(HashTableTest, CollisionHandling) {
    HashTable<std::string, std::string> ht(8); // Small table to force collisions
    
    // Insert multiple keys that might collide
    for (int i = 0; i < 20; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string val = "value" + std::to_string(i);
        EXPECT_TRUE(ht.insert_or_assign(key, val).second);
    }
    
    // Verify all keys
    for (int i = 0; i < 20; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string expected = "value" + std::to_string(i);
        auto val = ht.get(key);
        ASSERT_TRUE(val.has_value()) << "Missing key: " << key;
        EXPECT_EQ(*val, expected);
    }
}

TEST(HashTableTest, DeleteAndTombstone) {
    HashTable<std::string, std::string> ht(16);
    
    ht.insert_or_assign("key1", "value1");
    ht.insert_or_assign("key2", "value2");
    ht.insert_or_assign("key3", "value3");
    
    EXPECT_TRUE(ht.erase("key2"));
    EXPECT_FALSE(ht.erase("key2")); // Already deleted
    EXPECT_FALSE(ht.erase("nonexistent"));
    
    EXPECT_TRUE(ht.get("key1").has_value());
    EXPECT_FALSE(ht.get("key2").has_value());
    EXPECT_TRUE(ht.get("key3").has_value());
    
    // Re-insert deleted key
    EXPECT_TRUE(ht.insert_or_assign("key2", "new_value2").second);
    EXPECT_EQ(*ht.get("key2"), "new_value2");
}

TEST(HashTableTest, Resize) {
    HashTable<std::string, std::string> ht(4, 0.75);
    
    // Insert enough to trigger resize
    for (int i = 0; i < 10; ++i) {
        ht.insert_or_assign("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    EXPECT_GT(ht.capacity(), 4);
    
    // Verify all data still accessible
    for (int i = 0; i < 10; ++i) {
        auto val = ht.get("key" + std::to_string(i));
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, "value" + std::to_string(i));
    }
}

TEST(HashTableTest, UpdateExistingKey) {
    HashTable<std::string, std::string> ht(16);
    
    ht.insert_or_assign("key", "value1");
    EXPECT_EQ(ht.size(), 1);
    
    ht.insert_or_assign("key", "value2");
    EXPECT_EQ(ht.size(), 1);
    
    auto val = ht.get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value2");
}

TEST(HashTableTest, Iterator) {
    HashTable<std::string, std::string> ht(16);
    
    ht.insert_or_assign("a", "1");
    ht.insert_or_assign("b", "2");
    ht.insert_or_assign("c", "3");
    
    int count = 0;
    for (auto& entry : ht) {
        EXPECT_TRUE(entry.is_occupied());
        ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST(HashTableTest, Clear) {
    HashTable<std::string, std::string> ht(16);
    
    ht.insert_or_assign("key1", "value1");
    ht.insert_or_assign("key2", "value2");
    
    ht.clear();
    
    EXPECT_EQ(ht.size(), 0);
    EXPECT_FALSE(ht.get("key1").has_value());
    EXPECT_FALSE(ht.get("key2").has_value());
}

TEST(HashTableTest, MoveSemantics) {
    HashTable<std::string, std::string> ht1(16);
    ht1.insert_or_assign("key1", "value1");
    ht1.insert_or_assign("key2", "value2");
    
    HashTable<std::string, std::string> ht2 = std::move(ht1);
    
    EXPECT_EQ(ht2.size(), 2);
    EXPECT_TRUE(ht2.get("key1").has_value());
    EXPECT_TRUE(ht2.get("key2").has_value());
    EXPECT_EQ(ht1.size(), 0);
}

TEST(HashTableTest, Stats) {
    HashTable<std::string, std::string> ht(16);
    
    ht.insert_or_assign("key1", "value1");
    ht.get("key1");
    ht.get("nonexistent");
    
    auto stats = ht.get_stats();
    EXPECT_EQ(stats.size, 1);
    EXPECT_GT(stats.probes, 0);
}

TEST(HashTableTest, EmptyKey) {
    HashTable<std::string, std::string> ht(16);
    
    EXPECT_TRUE(ht.insert_or_assign("", "empty_key").second);
    auto val = ht.get("");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "empty_key");
}

TEST(HashTableTest, LargeValues) {
    HashTable<std::string, std::string> ht(16);
    
    std::string large_value(10000, 'x');
    ht.insert_or_assign("large", large_value);
    
    auto val = ht.get("large");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->size(), 10000);
}

TEST(HashTableTest, ManyResizes) {
    HashTable<std::string, std::string> ht(8, 0.5);
    
    for (int i = 0; i < 1000; ++i) {
        ht.insert_or_assign("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    auto stats = ht.get_stats();
    EXPECT_GT(stats.resizes, 0);
    EXPECT_EQ(stats.size, 1000);
}

TEST(HashTableTest, FNV1AHash) {
    // Test hash function directly
    EXPECT_EQ(fnv1a_hash(""), 0xcbf29ce484222325ULL);
    EXPECT_EQ(fnv1a_hash("a"), 0xaf63dc4c8601ec8cULL);
    EXPECT_EQ(fnv1a_hash("hello"), 0x5e2a7b1a7b8c4f3eULL);
}

TEST(HashTableTest, ConcurrencyReadOnly) {
    HashTable<std::string, std::string> ht(1024);
    
    // Populate
    for (int i = 0; i < 100; ++i) {
        ht.insert_or_assign("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    // Concurrent reads
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&ht]() {
            for (int i = 0; i < 1000; ++i) {
                ht.get("key" + std::to_string(i % 100));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
}