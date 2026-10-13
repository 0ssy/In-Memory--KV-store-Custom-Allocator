// test_wal.cpp - Write-Ahead Log tests
#include "kv_store/wal.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>
#include <random>

using namespace kv_store;

class WalTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "kv_store_wal_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
};

TEST_F(WalTest, BasicAppendAndReplay) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    
    // Append some records
    EXPECT_TRUE(wal.append(1, WalOpType::Put, "key1", "value1"));
    EXPECT_TRUE(wal.append(2, WalOpType::Put, "key2", "value2"));
    EXPECT_TRUE(wal.append(3, WalOpType::Delete, "key1"));
    
    wal.sync();
    
    // Replay
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records[0].header().op_type, WalOpType::Put);
    EXPECT_EQ(records[0].key(), "key1");
    EXPECT_EQ(records[0].value(), "value1");
    
    EXPECT_EQ(records[1].header().op_type, WalOpType::Put);
    EXPECT_EQ(records[1].key(), "key2");
    EXPECT_EQ(records[1].value(), "value2");
    
    EXPECT_EQ(records[2].header().op_type, WalOpType::Delete);
    EXPECT_EQ(records[2].key(), "key1");
    EXPECT_EQ(records[2].value(), "");
}

TEST_F(WalTest, ReplayAfterRestart) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    {
        WriteAheadLog wal(config);
        wal.append(1, WalOpType::Put, "key1", "value1");
        wal.append(2, WalOpType::Put, "key2", "value2");
        wal.sync();
    } // wal destroyed, file closed
    
    // New WAL instance should replay
    WriteAheadLog wal2(config);
    
    std::vector<WalRecord> records;
    wal2.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records[0].key(), "key1");
    EXPECT_EQ(records[1].key(), "key2");
}

TEST_F(WalTest, CorruptRecordDetection) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    wal.append(1, WalOpType::Put, "key1", "value1");
    wal.sync();
    
    // Corrupt the file by truncating
    std::filesystem::path segment = WriteAheadLog::segment_path(test_dir_.string(), 0);
    std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-5, std::ios::end); // Truncate last 5 bytes
    char c = 0;
    file.write(&c, 1);
    file.close();
    
    // Replay should detect corruption and stop
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    // Should have read the valid record before corruption
    EXPECT_EQ(records.size(), 1);
    EXPECT_EQ(records[0].key(), "key1");
}

TEST_F(WalTest, TruncatedRecordDetection) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    wal.append(1, WalOpType::Put, "key1", "value1");
    wal.sync();
    
    // Truncate the file in the middle of a record
    std::filesystem::path segment = WriteAheadLog::segment_path(test_dir_.string(), 0);
    auto file_size = std::filesystem::file_size(segment);
    std::filesystem::resize_file(segment, file_size / 2);
    
    // Replay should detect truncation
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    // Should have read nothing or partial
    EXPECT_EQ(records.size(), 0);
}

TEST_F(WalTest, ChecksumVerification) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    config.enable_checksums = true;
    
    WriteAheadLog wal(config);
    wal.append(1, WalOpType::Put, "key1", "value1");
    wal.sync();
    
    // Corrupt the data by flipping a bit in the value
    std::filesystem::path segment = WriteAheadLog::segment_path(test_dir_.string(), 0);
    std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
    
    // Find the value and corrupt it
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(-10, std::ios::end); // Near the end where value is
    
    char buf[10];
    file.read(buf, 10);
    // Corrupt first byte of value
    file.seekp(-10, std::ios::end);
    buf[0] ^= 0xFF;
    file.write(buf, 10);
    file.close();
    
    // Replay should detect checksum mismatch
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    EXPECT_EQ(records.size(), 0); // Corrupt record should be rejected
}

TEST_F(WalTest, SegmentRotation) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.segment_size = 1024; // Very small segments
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    
    // Write enough to trigger rotation
    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        // Make value large enough to trigger rotation
        value.append(100, 'x');
        EXPECT_TRUE(wal.append(i + 1, WalOpType::Put, key, value));
    }
    
    wal.sync();
    
    auto stats = wal.stats();
    EXPECT_GT(stats.segments, 1);
    
    // Replay all segments
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    EXPECT_EQ(records.size(), 100);
}

TEST_F(WalTest, SequenceNumberContinuity) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    
    wal.append(1, WalOpType::Put, "key1", "value1");
    wal.append(2, WalOpType::Put, "key2", "value2");
    wal.append(3, WalOpType::Put, "key3", "value3");
    wal.sync();
    
    // Manually create a segment with a gap
    std::filesystem::path segment = WriteAheadLog::segment_path(test_dir_.string(), 1);
    std::ofstream file(segment, std::ios::binary | std::ios::app);
    
    WalRecord record(5, WalOpType::Put, "key5", "value5"); // Gap: missing 4
    std::vector<char> buffer;
    record.serialize(buffer);
    file.write(buffer.data(), buffer.size());
    file.close();
    
    // Replay should stop at the gap
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    // Should only replay up to sequence 3
    EXPECT_EQ(records.size(), 3);
}

TEST_F(WalTest, ConcurrentAppend) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = false; // Don't sync for performance
    
    WriteAheadLog wal(config);
    const int num_threads = 4;
    const int ops_per_thread = 1000;
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&wal, &success_count, t, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                uint64_t seq = static_cast<uint64_t>(t * ops_per_thread + i + 1);
                std::string key = "key" + std::to_string(seq);
                std::string value = "value" + std::to_string(seq);
                if (wal.append(seq, WalOpType::Put, key, value)) {
                    ++success_count;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);
    
    wal.sync();
    
    // Verify all records
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    EXPECT_EQ(records.size(), num_threads * ops_per_thread);
}

TEST_F(WalTest, Sync) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = false;
    
    WriteAheadLog wal(config);
    
    wal.append(1, WalOpType::Put, "key1", "value1");
    wal.append(2, WalOpType::Put, "key2", "value2");
    
    EXPECT_TRUE(wal.sync());
    
    auto stats = wal.stats();
    EXPECT_EQ(stats.syncs, 1);
}

TEST_F(WalTest, EmptyKeyAndValue) {
    WriteAheadLog::Config config;
    config.directory = test_dir_.string();
    config.sync_on_write = true;
    
    WriteAheadLog wal(config);
    
    EXPECT_TRUE(wal.append(1, WalOpType::Put, "", "value"));
    EXPECT_TRUE(wal.append(2, WalOpType::Put, "key", ""));
    EXPECT_TRUE(wal.append(3, WalOpType::Put, "", ""));
    EXPECT_TRUE(wal.append(4, WalOpType::Delete, ""));
    
    wal.sync();
    
    std::vector<WalRecord> records;
    wal.replay([&records](const WalRecord& record) {
        records.push_back(record);
        return true;
    });
    
    ASSERT_EQ(records.size(), 4);
    EXPECT_EQ(records[0].key(), "");
    EXPECT_EQ(records[1].value(), "");
    EXPECT_EQ(records[2].key(), "");
    EXPECT_EQ(records[2].value(), "");
    EXPECT_EQ(records[3].key(), "");
}