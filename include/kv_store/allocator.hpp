// allocator.hpp - Custom memory allocators
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <mutex>
#include <vector>
#include <cassert>

namespace kv_store {

// ============================================================================
// Arena/Bump Allocator - Fast sequential allocation, no deallocation
// ============================================================================
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t initial_size = 64 * 1024) 
        : chunk_size_(initial_size) {
        allocate_chunk(initial_size);
    }
    
    ~ArenaAllocator() {
        for (void* chunk : chunks_) {
            ::operator delete(chunk, std::align_val_t(alignof(std::max_align_t)));
        }
    }
    
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    
    ArenaAllocator(ArenaAllocator&& other) noexcept
        : chunks_(std::move(other.chunks_)),
          current_(other.current_),
          end_(other.end_),
          chunk_size_(other.chunk_size_) {
        other.current_ = nullptr;
        other.end_ = nullptr;
    }
    
    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept {
        if (this != &other) {
            for (void* chunk : chunks_) {
                ::operator delete(chunk, std::align_val_t(alignof(std::max_align_t)));
            }
            chunks_ = std::move(other.chunks_);
            current_ = other.current_;
            end_ = other.end_;
            chunk_size_ = other.chunk_size_;
            other.current_ = nullptr;
            other.end_ = nullptr;
        }
        return *this;
    }
    
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        // Align current pointer
        uintptr_t addr = reinterpret_cast<uintptr_t>(current_);
        uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
        void* aligned_ptr = reinterpret_cast<void*>(aligned);
        size_t padding = aligned - addr;
        
        if (static_cast<char*>(aligned_ptr) + size > static_cast<char*>(end_)) {
            // Need new chunk
            size_t new_chunk_size = std::max(chunk_size_, size + padding + alignment);
            allocate_chunk(new_chunk_size);
            
            // Recalculate with new chunk
            addr = reinterpret_cast<uintptr_t>(current_);
            aligned = (addr + alignment - 1) & ~(alignment - 1);
            aligned_ptr = reinterpret_cast<void*>(aligned);
        }
        
        current_ = static_cast<char*>(aligned_ptr) + size;
        ++allocation_count_;
        allocated_bytes_ += size;
        return aligned_ptr;
    }
    
    void deallocate(void*, size_t) noexcept {
        // Arena allocator doesn't support individual deallocation
        // Use reset() to free all at once
    }
    
    void reset() noexcept {
        if (!chunks_.empty()) {
            current_ = chunks_.front();
            end_ = static_cast<char*>(chunks_.front()) + chunk_size_;
            // Keep chunks for reuse
        }
        allocation_count_ = 0;
        allocated_bytes_ = 0;
    }
    
    size_t allocated_bytes() const noexcept { return allocated_bytes_; }
    size_t allocation_count() const noexcept { return allocation_count_; }
    size_t chunk_count() const noexcept { return chunks_.size(); }
    
private:
    void allocate_chunk(size_t size) {
        void* chunk = ::operator new(size, std::align_val_t(alignof(std::max_align_t)));
        chunks_.push_back(chunk);
        current_ = chunk;
        end_ = static_cast<char*>(chunk) + size;
    }
    
    std::vector<void*> chunks_;
    void* current_ = nullptr;
    void* end_ = nullptr;
    size_t chunk_size_;
    size_t allocation_count_ = 0;
    size_t allocated_bytes_ = 0;
};

// ============================================================================
// Free-List Allocator - Reuses freed blocks of fixed size
// ============================================================================
class FreeListAllocator {
public:
    explicit FreeListAllocator(size_t block_size, size_t initial_blocks = 1024)
        : block_size_(align_up(block_size, alignof(std::max_align_t))),
          block_align_(alignof(std::max_align_t)) {
        grow(initial_blocks);
    }
    
    ~FreeListAllocator() {
        for (void* block : blocks_) {
            ::operator delete(block, std::align_val_t(block_align_));
        }
    }
    
    FreeListAllocator(const FreeListAllocator&) = delete;
    FreeListAllocator& operator=(const FreeListAllocator&) = delete;
    
    FreeListAllocator(FreeListAllocator&& other) noexcept
        : free_list_(std::move(other.free_list_)),
          blocks_(std::move(other.blocks_)),
          block_size_(other.block_size_),
          block_align_(other.block_align_),
          allocated_count_(other.allocated_count_),
          total_allocations_(other.total_allocations_),
          total_deallocations_(other.total_deallocations_) {
        other.allocated_count_ = 0;
    }
    
    FreeListAllocator& operator=(FreeListAllocator&& other) noexcept {
        if (this != &other) {
            for (void* block : blocks_) {
                ::operator delete(block, std::align_val_t(block_align_));
            }
            free_list_ = std::move(other.free_list_);
            blocks_ = std::move(other.blocks_);
            block_size_ = other.block_size_;
            block_align_ = other.block_align_;
            allocated_count_ = other.allocated_count_;
            total_allocations_ = other.total_allocations_;
            total_deallocations_ = other.total_deallocations_;
            other.allocated_count_ = 0;
        }
        return *this;
    }
    
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (size > block_size_ || alignment > block_align_) {
            // Fall back to system allocator for oversized requests
            return ::operator new(size, std::align_val_t(alignment));
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (free_list_.empty()) {
            grow(1024);
        }
        
        void* ptr = free_list_.back();
        free_list_.pop_back();
        ++allocated_count_;
        ++total_allocations_;
        return ptr;
    }
    
    void deallocate(void* ptr, size_t size) noexcept {
        if (!ptr) return;
        
        if (size > block_size_) {
            // Was allocated via system allocator
            ::operator delete(ptr, std::align_val_t(alignof(std::max_align_t)));
            return;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push_back(ptr);
        --allocated_count_;
        ++total_deallocations_;
    }
    
    size_t allocated_count() const noexcept { 
        std::lock_guard<std::mutex> lock(mutex_);
        return allocated_count_; 
    }
    
    size_t free_count() const noexcept { 
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_.size(); 
    }
    
    size_t total_allocations() const noexcept { return total_allocations_; }
    size_t total_deallocations() const noexcept { return total_deallocations_; }
    size_t block_size() const noexcept { return block_size_; }
    
private:
    static size_t align_up(size_t value, size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    
    void grow(size_t num_blocks) {
        for (size_t i = 0; i < num_blocks; ++i) {
            void* block = ::operator new(block_size_, std::align_val_t(block_align_));
            blocks_.push_back(block);
            free_list_.push_back(block);
        }
    }
    
    std::vector<void*> free_list_;
    std::vector<void*> blocks_;
    size_t block_size_;
    size_t block_align_;
    size_t allocated_count_ = 0;
    size_t total_allocations_ = 0;
    size_t total_deallocations_ = 0;
    mutable std::mutex mutex_;
};

// ============================================================================
// Thread-safe allocator wrapper
// ============================================================================
template<typename BaseAllocator>
class ThreadSafeAllocator {
public:
    using value_type = typename BaseAllocator::value_type;
    
    explicit ThreadSafeAllocator(BaseAllocator& alloc) : alloc_(alloc) {}
    
    template<typename T>
    ThreadSafeAllocator(const ThreadSafeAllocator<T>& other) : alloc_(other.alloc_) {}
    
    void* allocate(size_t n, size_t alignment = alignof(std::max_align_t)) {
        std::lock_guard<std::mutex> lock(mutex_);
        return alloc_.allocate(n, alignment);
    }
    
    void deallocate(void* p, size_t n) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        alloc_.deallocate(p, n);
    }
    
    BaseAllocator& base() noexcept { return alloc_; }
    const BaseAllocator& base() const noexcept { return alloc_; }
    
private:
    BaseAllocator& alloc_;
    mutable std::mutex mutex_;
};

// ============================================================================
// STL-compatible allocator adapter for ArenaAllocator
// ============================================================================
template<typename T>
class StlArenaAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    template<typename U> struct rebind { using other = StlArenaAllocator<U>; };
    
    explicit StlArenaAllocator(ArenaAllocator& arena) : arena_(&arena) {}
    
    template<typename U>
    StlArenaAllocator(const StlArenaAllocator<U>& other) : arena_(other.arena_) {}
    
    T* allocate(size_t n) {
        void* ptr = arena_->allocate(n * sizeof(T), alignof(T));
        return static_cast<T*>(ptr);
    }
    
    void deallocate(T* p, size_t n) noexcept {
        // Arena doesn't support individual deallocation
        (void)p; (void)n;
    }
    
    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
    
    template<typename U>
    void destroy(U* p) {
        p->~U();
    }
    
    bool operator==(const StlArenaAllocator& other) const noexcept {
        return arena_ == other.arena_;
    }
    
    bool operator!=(const StlArenaAllocator& other) const noexcept {
        return !(*this == other);
    }
    
    ArenaAllocator* arena() const noexcept { return arena_; }
    
private:
    ArenaAllocator* arena_;
};

// ============================================================================
// STL-compatible allocator adapter for FreeListAllocator
// ============================================================================
template<typename T>
class StlFreeListAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    template<typename U> struct rebind { using other = StlFreeListAllocator<U>; };
    
    explicit StlFreeListAllocator(FreeListAllocator& fl) : fl_(&fl) {}
    
    template<typename U>
    StlFreeListAllocator(const StlFreeListAllocator<U>& other) : fl_(other.fl_) {}
    
    T* allocate(size_t n) {
        if (n != 1) {
            // Only support single-object allocation for free list
            return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(alignof(T))));
        }
        void* ptr = fl_->allocate(sizeof(T), alignof(T));
        return static_cast<T*>(ptr);
    }
    
    void deallocate(T* p, size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p, std::align_val_t(alignof(T)));
            return;
        }
        fl_->deallocate(p, sizeof(T));
    }
    
    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
    
    template<typename U>
    void destroy(U* p) {
        p->~U();
    }
    
    bool operator==(const StlFreeListAllocator& other) const noexcept {
        return fl_ == other.fl_;
    }
    
    bool operator!=(const StlFreeListAllocator& other) const noexcept {
        return !(*this == other);
    }
    
    FreeListAllocator* base() const noexcept { return fl_; }
    
private:
    FreeListAllocator* fl_;
};

} // namespace kv_store