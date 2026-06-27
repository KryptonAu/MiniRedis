#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace miniredis {
namespace ds {

// ===== 小端读写 helper（用于 intset/listpack 的 Redis 兼容二进制格式） =====

inline uint16_t LoadLE16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t LoadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t LoadLE64(const uint8_t* p) {
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

inline void StoreLE16(uint8_t* p, uint16_t val) {
  p[0] = static_cast<uint8_t>(val & 0xFF);
  p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void StoreLE32(uint8_t* p, uint32_t val) {
  p[0] = static_cast<uint8_t>(val & 0xFF);
  p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

inline void StoreLE64(uint8_t* p, uint64_t val) {
  p[0] = static_cast<uint8_t>(val & 0xFF);
  p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
  p[4] = static_cast<uint8_t>((val >> 32) & 0xFF);
  p[5] = static_cast<uint8_t>((val >> 40) & 0xFF);
  p[6] = static_cast<uint8_t>((val >> 48) & 0xFF);
  p[7] = static_cast<uint8_t>((val >> 56) & 0xFF);
}

}  // namespace ds
}  // namespace miniredis
