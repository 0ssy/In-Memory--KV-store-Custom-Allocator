// wal.cpp - Write-Ahead Log implementation
#include "kv_store/wal.hpp"
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

namespace kv_store {

WriteAheadLog::WriteAheadLog(const Config& config) : config_(config) {
    std::filesystem::create_directories(config_.directory);
    ensure_open();
}

WriteAheadLog::~WriteAheadLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_file_.is_open()) {
        current_file_.flush();
        current_file_.close();
    }
}

bool WriteAheadLog::ensure_open() {
    if (current_file_.is_open()) return true;
    
    current_path_ = segment_path(config_.directory, segment_id_);
    current_file_.open(current_path_, std::ios::binary | std::ios::app);
    
    if (!current_file_.is_open()) {
        return false;
    }
    
    // Get current file size
    current_file_.seekp(0, std::ios::end);
    current_segment_bytes_ = static_cast<size_t>(current_file_.tellp());
    
    return true;
}

bool WriteAheadLog::rotate_unlocked() {
    if (current_file_.is_open()) {
        current_file_.flush();
        current_file_.close();
    }
    
    ++segment_id_;
    current_segment_bytes_ = 0;
    ++stats_.segments;
    
    return ensure_open();
}

std::filesystem::path WriteAheadLog::segment_path(const std::string& dir, uint64_t segment_id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "wal_%016llx.log", static_cast<unsigned long long>(segment_id));
    return std::filesystem::path(dir) / buf;
}

uint64_t WriteAheadLog::extract_sequence(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    // wal_XXXXXXXXXXXXXXXX.log
    if (filename.size() >= 21 && filename.starts_with("wal_") && filename.ends_with(".log")) {
        std::string hex = filename.substr(4, 16);
        try {
            return std::stoull(hex, nullptr, 16);
        } catch (...) {}
    }
    return 0;
}

bool WriteAheadLog::append(uint64_t sequence, WalOpType type, std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!ensure_open()) return false;
    
    WalRecord record(sequence, type, key, value);
    std::vector<char> buffer;
    record.serialize(buffer);
    
    // Check if we need to rotate
    if (current_segment_bytes_ + buffer.size() > config_.segment_size) {
        if (!rotate_unlocked()) return false;
    }
    
    current_file_.write(buffer.data(), buffer.size());
    if (!current_file_.good()) return false;
    
    current_segment_bytes_ += buffer.size();
    sequence_ = sequence;
    ++stats_.records_written;
    stats_.bytes_written += buffer.size();
    
    if (config_.sync_on_write) {
        sync_unlocked();
    }
    
    return true;
}

bool WriteAheadLog::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sync_unlocked();
}

bool WriteAheadLog::sync_unlocked() {
    if (!current_file_.is_open()) return true;
    
    current_file_.flush();
    ++stats_.syncs;
    return true;
}

WriteAheadLog::Stats WriteAheadLog::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void WriteAheadLog::rotate() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotate_unlocked();
}

} // namespace kv_store