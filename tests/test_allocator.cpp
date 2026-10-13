// test_allocator.cpp - Allocator tests
#include "kv_store/allocator.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <chrono>

using namespace kv_store;

TEST(ArenaAllocatorTest, BasicAllocation) {
    ArenaAllocator arena(1024);
    
    void* ptr1 = arena.allocate(100);
    ASSERT_NE(ptr1, nullptr);
    
    void* ptr2 = arena.allocate(200);
    ASSERT_NE(ptr2, nullptr);
    
    EXPECT_NE(ptr1, ptr2);
    EXPECT_EQ(arena.allocation_count(), 2);
    EXPECT_EQ(arena.allocated_bytes(), 300);
}

TEST(ArenaAllocatorTest, Alignment) {
    ArenaAllocator arena(1024);
    
    // Test various alignments
    for (size_t align : {1, 2, 4, 8, 16, 32, 64, 128}) {
        void* ptr = arena.allocate(1, align);
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        EXPECT_EQ(addr % align, 0) << "Alignment " << align << " failed";
    }
}

TEST(ArenaAllocatorTest, ChunkGrowth) {
    ArenaAllocator arena(64); // Very small initial chunk
    
    // Allocate more than initial chunk
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        ptrs.push_back(arena.allocate(100));
    }
    
    EXPECT_GT(arena.chunk_count(), 1);
    EXPECT_EQ(arena.allocation_count(), 100);
    
    // Verify all pointers are valid and unique
    for (size_t i = 0; i < ptrs.size(); ++i) {
        for (size_t j = i + 1; j < ptrs.size(); ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]);
        }
    }
}

TEST(ArenaAllocatorTest, Reset) {
    ArenaAllocator arena(1024);
    
    arena.allocate(100);
    arena.allocate(200);
    
    EXPECT_EQ(arena.allocation_count(), 2);
    
    arena.reset();
    
    EXPECT_EQ(arena.allocation_count(), 0);
    EXPECT_EQ(arena.allocated_bytes(), 0);
    
    // Can allocate again after reset
    void* ptr = arena.allocate(50);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(arena.allocation_count(), 1);
}

TEST(ArenaAllocatorTest, MoveSemantics) {
    ArenaAllocator arena1(1024);
    arena1.allocate(100);
    
    ArenaAllocator arena2 = std::move(arena1);
    
    EXPECT_EQ(arena2.allocation_count(), 1);
    EXPECT_EQ(arena1.allocation_count(), 0);
    
    // arena2 should still work
    void* ptr = arena2.allocate(50);
    ASSERT_NE(ptr, nullptr);
}

TEST(FreeListAllocatorTest, BasicAllocation) {
    FreeListAllocator fl(64, 10);
    
    void* ptr1 = fl.allocate(32);
    ASSERT_NE(ptr1, nullptr);
    
    void* ptr2 = fl.allocate(32);
    ASSERT_NE(ptr2, nullptr);
    
    EXPECT_NE(ptr1, ptr2);
    EXPECT_EQ(fl.allocated_count(), 2);
    EXPECT_EQ(fl.free_count(), 8);
}

TEST(FreeListAllocatorTest, DeallocateAndReuse) {
    FreeListAllocator fl(64, 10);
    
    void* ptr1 = fl.allocate(32);
    void* ptr2 = fl.allocate(32);
    
    fl.deallocate(ptr1, 32);
    EXPECT_EQ(fl.allocated_count(), 1);
    EXPECT_EQ(fl.free_count(), 9);
    
    // Should reuse the freed block
    void* ptr3 = fl.allocate(32);
    EXPECT_EQ(ptr3, ptr1);
    EXPECT_EQ(fl.allocated_count(), 2);
    EXPECT_EQ(fl.free_count(), 8);
}

TEST(FreeListAllocatorTest, Alignment) {
    FreeListAllocator fl(128, 10);
    
    void* ptr = fl.allocate(16, 32);
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    EXPECT_EQ(addr % 32, 0);
}

TEST(FreeListAllocatorTest, OversizedAllocation) {
    FreeListAllocator fl(64, 10);
    
    // Allocate larger than block size - should fall back to system allocator
    void* ptr = fl.allocate(128);
    ASSERT_NE(ptr, nullptr);
    
    // Deallocate should work
    fl.deallocate(ptr, 128);
    EXPECT_EQ(fl.total_allocations(), 1);
    EXPECT_EQ(fl.total_deallocations(), 1);
}

TEST(FreeListAllocatorTest, MoveSemantics) {
    FreeListAllocator fl1(64, 10);
    fl1.allocate(32);
    fl1.allocate(32);
    
    FreeListAllocator fl2 = std::move(fl1);
    
    EXPECT_EQ(fl2.allocated_count(), 2);
    EXPECT_EQ(fl1.allocated_count(), 0);
}

TEST(FreeListAllocatorTest, ConcurrentAccess) {
    FreeListAllocator fl(64, 1000);
    const int num_threads = 4;
    const int ops_per_thread = 1000;
    
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&fl, &errors, ops_per_thread]() {
            std::vector<void*> ptrs;
            ptrs.reserve(ops_per_thread);
            
            for (int i = 0; i < ops_per_thread; ++i) {
                void* ptr = fl.allocate(32);
                if (!ptr) {
                    ++errors;
                    continue;
                }
                ptrs.push_back(ptr);
            }
            
            // Deallocate half
            for (size_t i = 0; i < ptrs.size() / 2; ++i) {
                fl.deallocate(ptrs[i], 32);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(errors.load(), 0);
    EXPECT_GT(fl.total_allocations(), 0);
    EXPECT_GT(fl.total_deallocations(), 0);
}

TEST(StlArenaAllocatorTest, StringAllocation) {
    ArenaAllocator arena(1024);
    StlArenaAllocator<std::string> alloc(arena);
    
    std::string* str = alloc.allocate(1);
    alloc.construct(str, "hello world");
    
    EXPECT_EQ(*str, "hello world");
    
    alloc.destroy(str);
    alloc.deallocate(str, 1);
}

TEST(StlFreeListAllocatorTest, StringAllocation) {
    FreeListAllocator fl(sizeof(std::string), 100);
    StlFreeListAllocator<std::string> alloc(fl);
    
    std::string* str = alloc.allocate(1);
    alloc.construct(str, "test string");
    
    EXPECT_EQ(*str, "test string");
    
    alloc.destroy(str);
    alloc.deallocate(str, 1);
}

TEST(AllocatorTest, StressTest) {
    ArenaAllocator arena(1024 * 1024);
    FreeListAllocator fl(256, 10000);
    
    // Mix of allocations
    std::vector<void*> arena_ptrs;
    std::vector<void*> fl_ptrs;
    
    for (int i = 0; i < 5000; ++i) {
        if (i % 2 == 0) {
            arena_ptrs.push_back(arena.allocate(64));
        } else {
            fl_ptrs.push_back(fl.allocate(128));
        }
    }
    
    // Deallocate free list pointers
    for (void* ptr : fl_ptrs) {
        fl.deallocate(ptr, 128);
    }
    
    // Reallocate
    for (int i = 0; i < 1000; ++i) {
        fl_ptrs.push_back(fl.allocate(128));
    }
    
    EXPECT_GT(arena.allocation_count(), 0);
    EXPECT_GT(fl.total_allocations(), 0);
}