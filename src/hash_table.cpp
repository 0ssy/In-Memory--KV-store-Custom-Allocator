// hash_table.cpp - Hash table implementation (for non-template parts)
#include "kv_store/hash_table.hpp"

namespace kv_store {

// Explicit instantiation for common types used in the store
template class HashTable<std::string, std::string>;

} // namespace kv_store