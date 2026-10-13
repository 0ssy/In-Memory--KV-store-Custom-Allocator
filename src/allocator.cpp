// allocator.cpp - Allocator implementation (for non-template parts)
#include "kv_store/allocator.hpp"

namespace kv_store {

// Explicit instantiations
template class StlArenaAllocator<std::string>;
template class StlArenaAllocator<std::pair<const std::string, std::string>>;
template class StlFreeListAllocator<std::string>;
template class StlFreeListAllocator<std::pair<const std::string, std::string>>;

} // namespace kv_store