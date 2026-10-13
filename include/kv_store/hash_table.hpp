// hash_table.hpp - Open addressing hash table with linear probing
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <utility>

namespace kv_store {

// Forward declaration for allocator
template<typename T> class Allocator;

// Hash function - FNV-1a 64-bit
inline uint64_t fnv1a_hash(std::string_view key) noexcept {
    constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    
    uint64_t hash = FNV_OFFSET_BASIS;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= FNV_PRIME;
    }
    return hash;
}

// Entry states for open addressing
enum class EntryState : uint8_t {
    Empty = 0,
    Occupied = 1,
    Deleted = 2  // Tombstone
};

// Hash table entry
template<typename Key, typename Value>
struct HashEntry {
    EntryState state = EntryState::Empty;
    uint64_t hash = 0;
    Key key;
    Value value;
    
    HashEntry() = default;
    
    HashEntry(uint64_t h, Key&& k, Value&& v)
        : state(EntryState::Occupied), hash(h), key(std::move(k)), value(std::move(v)) {}
    
    HashEntry(const HashEntry&) = default;
    HashEntry& operator=(const HashEntry&) = default;
    HashEntry(HashEntry&&) noexcept = default;
    HashEntry& operator=(HashEntry&&) noexcept = default;
    
    bool is_empty() const noexcept { return state == EntryState::Empty; }
    bool is_occupied() const noexcept { return state == EntryState::Occupied; }
    bool is_deleted() const noexcept { return state == EntryState::Deleted; }
    
    void mark_deleted() noexcept { state = EntryState::Deleted; }
    void mark_empty() noexcept { state = EntryState::Empty; }
};

// Open addressing hash table with linear probing
template<typename Key, typename Value, typename Hash = std::hash<Key>, 
         typename KeyEqual = std::equal_to<Key>, typename Alloc = std::allocator<HashEntry<Key, Value>>>
class HashTable {
public:
    using entry_type = HashEntry<Key, Value>;
    using allocator_type = Alloc;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    // Default configuration
    static constexpr double DEFAULT_MAX_LOAD_FACTOR = 0.7;
    static constexpr size_type MIN_CAPACITY = 8;
    
    explicit HashTable(size_type capacity = 1024, 
                       double max_load_factor = DEFAULT_MAX_LOAD_FACTOR,
                       const Alloc& alloc = Alloc())
        : alloc_(alloc), max_load_factor_(max_load_factor), size_(0), capacity_(0), entries_(nullptr) {
        reserve(capacity);
    }
    
    ~HashTable() {
        clear();
        deallocate_entries();
    }
    
    // Non-copyable, movable
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;
    
    HashTable(HashTable&& other) noexcept
        : alloc_(std::move(other.alloc_)),
          max_load_factor_(other.max_load_factor_),
          size_(other.size_),
          capacity_(other.capacity_),
          entries_(other.entries_) {
        other.entries_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    
    HashTable& operator=(HashTable&& other) noexcept {
        if (this != &other) {
            clear();
            deallocate_entries();
            
            alloc_ = std::move(other.alloc_);
            max_load_factor_ = other.max_load_factor_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            entries_ = other.entries_;
            
            other.entries_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }
    
    // Reserve capacity
    void reserve(size_type new_cap) {
        if (new_cap <= capacity_) return;
        new_cap = next_power_of_two(new_cap);
        rehash(new_cap);
    }
    
    // Insert or update
    // Returns pair<iterator, bool> where bool is true if inserted (new key)
    template<typename K, typename V>
    std::pair<entry_type*, bool> insert_or_assign(K&& key, V&& value) {
        if (should_resize()) {
            resize();
        }
        
        const uint64_t hash = hash_fn_(key);
        size_type idx = probe_index(hash);
        
        // Linear probing
        while (true) {
            entry_type* entry = &entries_[idx];
            
            if (entry->is_empty()) {
                // New insertion
                ::new (entry) entry_type(hash, Key(std::forward<K>(key)), Value(std::forward<V>(value)));
                ++size_;
                return {entry, true};
            }
            
            if (entry->is_occupied() && key_equal_(entry->key, key)) {
                // Update existing
                entry->value = std::forward<V>(value);
                return {entry, false};
            }
            
            if (entry->is_deleted()) {
                // Reuse tombstone
                ::new (entry) entry_type(hash, Key(std::forward<K>(key)), Value(std::forward<V>(value)));
                ++size_;
                return {entry, true};
            }
            
            idx = (idx + 1) & (capacity_ - 1);
        }
    }
    
    // Find key
    template<typename K>
    entry_type* find(const K& key) noexcept {
        const uint64_t hash = hash_fn_(key);
        size_type idx = probe_index(hash);
        
        while (true) {
            entry_type* entry = &entries_[idx];
            
            if (entry->is_empty()) {
                return nullptr; // Not found
            }
            
            if (entry->is_occupied() && key_equal_(entry->key, key)) {
                return entry; // Found
            }
            
            idx = (idx + 1) & (capacity_ - 1);
        }
    }
    
    template<typename K>
    const entry_type* find(const K& key) const noexcept {
        return const_cast<HashTable*>(this)->find(key);
    }
    
    // Erase key
    template<typename K>
    bool erase(const K& key) noexcept {
        entry_type* entry = find(key);
        if (!entry) return false;
        
        entry->~entry_type();
        entry->mark_deleted();
        --size_;
        return true;
    }
    
    // Get value (const)
    std::optional<Value> get(const Key& key) const {
        const entry_type* entry = find(key);
        if (!entry) return std::nullopt;
        return entry->value;
    }
    
    // Get value (mutable)
    Value* get_mutable(const Key& key) noexcept {
        entry_type* entry = find(key);
        if (!entry) return nullptr;
        return &entry->value;
    }
    
    // Size and capacity
    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }
    double load_factor() const noexcept { return capacity_ > 0 ? double(size_) / capacity_ : 0.0; }
    double max_load_factor() const noexcept { return max_load_factor_; }
    void max_load_factor(double mlf) noexcept { max_load_factor_ = mlf; }
    
    // Clear all entries
    void clear() noexcept {
        if (entries_) {
            for (size_type i = 0; i < capacity_; ++i) {
                if (entries_[i].is_occupied()) {
                    entries_[i].~entry_type();
                }
            }
        }
        size_ = 0;
    }
    
    // Iterator support for range-based for
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = entry_type;
        using difference_type = ptrdiff_t;
        using pointer = entry_type*;
        using reference = entry_type&;
        
        iterator(entry_type* ptr, entry_type* end) : current_(ptr), end_(end) {
            advance_to_valid();
        }
        
        reference operator*() const noexcept { return *current_; }
        pointer operator->() const noexcept { return current_; }
        
        iterator& operator++() noexcept {
            ++current_;
            advance_to_valid();
            return *this;
        }
        
        iterator operator++(int) noexcept {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }
        
        bool operator==(const iterator& other) const noexcept { return current_ == other.current_; }
        bool operator!=(const iterator& other) const noexcept { return current_ != other.current_; }
        
    private:
        void advance_to_valid() noexcept {
            while (current_ != end_ && !current_->is_occupied()) {
                ++current_;
            }
        }
        
        entry_type* current_;
        entry_type* end_;
    };
    
    iterator begin() noexcept { return iterator(entries_, entries_ + capacity_); }
    iterator end() noexcept { return iterator(entries_ + capacity_, entries_ + capacity_); }
    
    // Stats
    struct Stats {
        size_type size = 0;
        size_type capacity = 0;
        double load_factor = 0.0;
        uint64_t probes = 0;
        uint64_t resizes = 0;
    };
    
    Stats get_stats() const noexcept {
        return {size_, capacity_, max_load_factor_, probes_, resize_count_};
    }
    
private:
    Alloc alloc_;
    Hash hash_fn_;
    KeyEqual key_equal_;
    double max_load_factor_;
    size_type size_;
    size_type capacity_;
    entry_type* entries_;
    
    mutable uint64_t probes_ = 0;
    uint64_t resize_count_ = 0;
    
    // Allocate entry array
    entry_type* allocate_entries(size_type n) {
        if (n == 0) return nullptr;
        return std::allocator_traits<Alloc>::allocate(alloc_, n);
    }
    
    void deallocate_entries() noexcept {
        if (entries_) {
            std::allocator_traits<Alloc>::deallocate(alloc_, entries_, capacity_);
            entries_ = nullptr;
        }
    }
    
    // Probe index from hash
    size_type probe_index(uint64_t hash) const noexcept {
        return static_cast<size_type>(hash & (capacity_ - 1));
    }
    
    // Check if resize needed
    bool should_resize() const noexcept {
        return size_ >= capacity_ * max_load_factor_;
    }
    
    // Resize and rehash
    void resize() {
        size_type new_cap = capacity_ == 0 ? MIN_CAPACITY : capacity_ * 2;
        rehash(new_cap);
    }
    
    // Rehash to new capacity
    void rehash(size_type new_cap) {
        entry_type* old_entries = entries_;
        size_type old_cap = capacity_;
        
        capacity_ = next_power_of_two(new_cap);
        entries_ = allocate_entries(capacity_);
        
        // Initialize new entries to empty
        for (size_type i = 0; i < capacity_; ++i) {
            ::new (&entries_[i]) entry_type();
        }
        
        size_ = 0;
        ++resize_count_;
        
        // Reinsert all occupied entries
        if (old_entries) {
            for (size_type i = 0; i < old_cap; ++i) {
                if (old_entries[i].is_occupied()) {
                    insert_or_assign(std::move(old_entries[i].key), std::move(old_entries[i].value));
                    old_entries[i].~entry_type();
                } else if (old_entries[i].is_deleted()) {
                    old_entries[i].~entry_type();
                }
            }
            std::allocator_traits<Alloc>::deallocate(alloc_, old_entries, old_cap);
        }
    }
    
    // Next power of two
    static size_type next_power_of_two(size_type n) noexcept {
        if (n <= MIN_CAPACITY) return MIN_CAPACITY;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }
};

} // namespace kv_store