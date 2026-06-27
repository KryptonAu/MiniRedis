#pragma once

#include <cstdint>

namespace miniredis {

enum class ValueType : uint8_t {
  kString = 0,
  kList = 1,
  kSet = 2,
  kHash = 3,
  kZSet = 4,
};

enum class ValueEncoding : uint8_t {
  // String
  kIntEmbed = 0,
  kRaw = 1,

  // List
  kQuicklist = 2,

  // Set
  kIntset = 3,
  kHashtable = 4,

  // Hash
  kListpack = 5,
  kHashHT = 6,

  // ZSet
  kZSetListpack = 7,
  kSkiplist = 8,
};

}  // namespace miniredis
