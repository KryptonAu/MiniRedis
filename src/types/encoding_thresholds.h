#pragma once

#include <cstddef>

namespace miniredis {

struct EncodingThresholds {
  // Set
  size_t set_max_intset_entries = 512;

  // Hash
  size_t hash_max_listpack_entries = 512;
  size_t hash_max_listpack_value = 64;

  // ZSet
  size_t zset_max_listpack_entries = 128;
  size_t zset_max_listpack_value = 64;
};

}  // namespace miniredis
