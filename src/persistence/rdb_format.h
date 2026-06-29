#pragma once

#include <cstdint>

namespace miniredis {
namespace rdb {

// Opcodes
constexpr uint8_t kOpAux = 0xFA;
constexpr uint8_t kOpResizeDb = 0xFB;
constexpr uint8_t kOpExpireTimeMs = 0xFC;
constexpr uint8_t kOpExpireTime = 0xFD;
constexpr uint8_t kOpSelectDb = 0xFE;
constexpr uint8_t kOpEof = 0xFF;

// Value type encodings (Redis generic types, not compact types)
constexpr uint8_t kTypeString = 0;
constexpr uint8_t kTypeList = 1;
constexpr uint8_t kTypeSet = 2;
constexpr uint8_t kTypeZSet2 = 5;
constexpr uint8_t kTypeHash = 4;

// String encoding
constexpr uint8_t kEncInt8 = 0xC0;
constexpr uint8_t kEncInt16 = 0xC1;
constexpr uint8_t kEncInt32 = 0xC2;

// Length encoding
constexpr uint8_t kLen6Bit = 0;
constexpr uint8_t kLen14Bit = 1;
constexpr uint8_t kLen14BitPrefix = 0x40;
constexpr uint8_t kLen32Bit = 0x80;
constexpr uint8_t kLen64Bit = 0x81;

// RDB version
constexpr int kRdbVersion = 11;
constexpr const char kRdbMagic[] = "REDIS";

}  // namespace rdb
}  // namespace miniredis
