// wal.hpp - Write-Ahead Log for durability
#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace kv_store {

// WAL Record types
enum class WalOpType : uint8_t {
    Put = 1,
    Delete = 2,
    Checkpoint = 3
};

// WAL Record header (fixed size)
struct alignas(8) WalRecordHeader {
    uint32_t magic;        // Magic number for validation
    uint32_t version;      // Format version
    uint64_t sequence;     // Monotonic sequence number
    uint64_t timestamp;    // Unix timestamp (microseconds)
    WalOpType op_type;     // Operation type
    uint8_t flags;         // Flags (checksum type, etc.)
    uint16_t key_len;      // Key length
    uint32_t value_len;    // Value length (0 for DELETE)
    uint32_t checksum;     // CRC32 of key + value
    uint32_t header_crc;   // CRC32 of header (excluding this field)
    
    static constexpr uint32_t MAGIC = 0x4B565354; // "KVST"
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t SIZE = 40; // bytes
    
    bool valid() const noexcept {
        return magic == MAGIC && version == VERSION;
    }
};

// WAL Record (header + key + value)
class WalRecord {
public:
    WalRecord() = default;
    
    WalRecord(uint64_t seq, WalOpType type, std::string_view key, std::string_view value = "")
        : header_{}, key_(key), value_(value) {
        header_.magic = WalRecordHeader::MAGIC;
        header_.version = WalRecordHeader::VERSION;
        header_.sequence = seq;
        header_.op_type = type;
        header_.flags = 0;
        header_.key_len = static_cast<uint16_t>(key.size());
        header_.value_len = static_cast<uint32_t>(value.size());
        header_.timestamp = current_time_micros();
        compute_checksums();
    }
    
    const WalRecordHeader& header() const noexcept { return header_; }
    std::string_view key() const noexcept { return key_; }
    std::string_view value() const noexcept { return value_; }
    size_t total_size() const noexcept { return WalRecordHeader::SIZE + key_.size() + value_.size(); }
    
    // Serialize to buffer
    void serialize(std::vector<char>& buffer) const {
        buffer.resize(total_size());
        char* dst = buffer.data();
        std::memcpy(dst, &header_, WalRecordHeader::SIZE);
        dst += WalRecordHeader::SIZE;
        std::memcpy(dst, key_.data(), key_.size());
        dst += key_.size();
        std::memcpy(dst, value_.data(), value_.size());
    }
    
    // Deserialize from buffer
    static std::optional<WalRecord> deserialize(std::string_view data) {
        if (data.size() < WalRecordHeader::SIZE) return std::nullopt;
        
        WalRecord record;
        const char* src = data.data();
        std::memcpy(&record.header_, src, WalRecordHeader::SIZE);
        src += WalRecordHeader::SIZE;
        
        if (!record.header_.valid()) return std::nullopt;
        if (data.size() < WalRecordHeader::SIZE + record.header_.key_len + record.header_.value_len) {
            return std::nullopt;
        }
        
        record.key_ = std::string_view(src, record.header_.key_len);
        src += record.header_.key_len;
        record.value_ = std::string_view(src, record.header_.value_len);
        
        // Verify checksums
        if (!record.verify_checksums()) return std::nullopt;
        
        return record;
    }
    
    // Verify record integrity
    bool verify_checksums() const {
        // Verify header CRC
        uint32_t header_crc = crc32(reinterpret_cast<const uint8_t*>(&header_), 
                                     offsetof(WalRecordHeader, header_crc));
        if (header_crc != header_.header_crc) return false;
        
        // Verify data CRC
        uint32_t data_crc = crc32(reinterpret_cast<const uint8_t*>(key_.data()), key_.size());
        data_crc = crc32_continue(data_crc, reinterpret_cast<const uint8_t*>(value_.data()), value_.size());
        if (data_crc != header_.checksum) return false;
        
        return true;
    }
    
    // Compute checksums
    void compute_checksums() {
        header_.checksum = crc32(reinterpret_cast<const uint8_t*>(key_.data()), key_.size());
        header_.checksum = crc32_continue(header_.checksum, 
                                           reinterpret_cast<const uint8_t*>(value_.data()), value_.size());
        header_.header_crc = crc32(reinterpret_cast<const uint8_t*>(&header_), 
                                    offsetof(WalRecordHeader, header_crc));
    }
    
private:
    static uint64_t current_time_micros() {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    }
    
    // CRC32 implementation
    static uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFF) {
        static const uint32_t table[256] = {
            0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
            0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
            0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
            0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
            0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
            0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
            0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
            0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
            0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
            0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BFC24,
            0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
            0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
            0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
            0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
            0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
            0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
            0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
            0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
            0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
            0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
            0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
            0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
            0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
            0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
            0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
            0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
            0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
            0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
            0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
            0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
            0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
            0x2BB45A92, 0x5CB36A04, 0xC2D8FFA7, 0xB5DFCF31, 0x2CD99E8B, 0x5BDEAE1D,
            0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
            0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
            0x92D28E90, 0xE5D5BE08, 0x7CDCEFB7, 0x0BDCFE0F, 0x93D9E92B, 0xE4DAD9BD,
            0x7FD027F4, 0x08D71762, 0x9FD046F8, 0xE8D7766E
        };
        
        for (size_t i = 0; i < len; ++i) {
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }
    
    static uint32_t crc32_continue(uint32_t crc, const uint8_t* data, size_t len) {
        return crc32(data, len, crc);
    }
    
    WalRecordHeader header_;
    std::string_view key_;
    std::string_view value_;
};

// ============================================================================
// Write-Ahead Log
// ============================================================================
class WriteAheadLog {
public:
    struct Config {
        std::string directory = "./data";
        size_t segment_size = 64 * 1024 * 1024; // 64MB
        bool enable_checksums = true;
        bool sync_on_write = false;  // If true, fsync every write
    };
    
    struct Stats {
        uint64_t records_written = 0;
        uint64_t bytes_written = 0;
        uint64_t syncs = 0;
        uint64_t segments = 0;
        uint64_t corrupt_records = 0;
    };
    
    explicit WriteAheadLog(const Config& config = Config{});
    ~WriteAheadLog();
    
    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;
    
    // Append a record (thread-safe)
    bool append(uint64_t sequence, WalOpType type, std::string_view key, std::string_view value = "");
    
    // Force sync to disk
    bool sync();
    
    // Replay all records from beginning
    // Callback returns false to stop replay
    template<typename Callback>
    bool replay(Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        return replay_unlocked(std::forward<Callback>(callback));
    }
    
    // Get current sequence number
    uint64_t current_sequence() const noexcept { return sequence_; }
    
    // Get stats
    Stats stats() const noexcept;
    
    // Close current segment
    void rotate();
    
private:
    Config config_;
    mutable std::mutex mutex_;
    std::ofstream current_file_;
    std::filesystem::path current_path_;
    uint64_t sequence_ = 0;
    size_t current_segment_bytes_ = 0;
    uint64_t segment_id_ = 0;
    Stats stats_;
    
    // Internal methods (require lock)
    bool ensure_open();
    bool rotate_unlocked();
    bool sync_unlocked();
    template<typename Callback>
    bool replay_unlocked(Callback&& callback) {
        // Find all segment files
        std::vector<std::filesystem::path> segments;
        for (const auto& entry : std::filesystem::directory_iterator(config_.directory)) {
            if (entry.is_regular_file() && entry.path().filename().string().starts_with("wal_") 
                && entry.path().filename().string().ends_with(".log")) {
                segments.push_back(entry.path());
            }
        }
        
        // Sort by segment ID
        std::sort(segments.begin(), segments.end(), 
            [](const auto& a, const auto& b) {
                return extract_sequence(a) < extract_sequence(b);
            });
        
        uint64_t expected_sequence = 1;
        
        for (const auto& segment_path : segments) {
            std::ifstream file(segment_path, std::ios::binary);
            if (!file.is_open()) continue;
            
            std::vector<char> buffer(64 * 1024);
            
            while (file.good()) {
                // Read header first
                WalRecordHeader header;
                file.read(reinterpret_cast<char*>(&header), WalRecordHeader::SIZE);
                
                if (file.gcount() == 0) break; // EOF
                if (file.gcount() != static_cast<std::streamsize>(WalRecordHeader::SIZE)) {
                    ++stats_.corrupt_records;
                    break; // Truncated header
                }
                
                if (!header.valid()) {
                    ++stats_.corrupt_records;
                    break; // Invalid magic/version
                }
                
                // Read key + value
                size_t data_size = header.key_len + header.value_len;
                if (data_size > buffer.size()) {
                    buffer.resize(data_size);
                }
                
                file.read(buffer.data(), data_size);
                if (file.gcount() != static_cast<std::streamsize>(data_size)) {
                    ++stats_.corrupt_records;
                    break; // Truncated record
                }
                
                // Deserialize and verify - combine header + data into contiguous buffer for verification
                std::vector<char> full_record;
                full_record.resize(WalRecordHeader::SIZE + data_size);
                std::memcpy(full_record.data(), &header, WalRecordHeader::SIZE);
                std::memcpy(full_record.data() + WalRecordHeader::SIZE, buffer.data(), data_size);
                
                auto record_opt = WalRecord::deserialize(std::string_view(full_record.data(), full_record.size()));
                
                if (!record_opt) {
                    ++stats_.corrupt_records;
                    break; // Checksum mismatch
                }
                
                const WalRecord& record = *record_opt;
                
                // Check sequence continuity
                if (record.header().sequence != expected_sequence) {
                    // Gap in sequence - stop replay
                    break;
                }
                
                // Call callback
                bool continue_replay = callback(record);
                if (!continue_replay) return true;
                
                expected_sequence = record.header().sequence + 1;
            }
            
            file.close();
        }
        
        sequence_ = expected_sequence - 1;
        return true;
    }
    static std::filesystem::path segment_path(const std::string& dir, uint64_t segment_id);
    static uint64_t extract_sequence(const std::filesystem::path& path);
};

} // namespace kv_store