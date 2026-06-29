#include "persistence/rdb_deserialize.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/database.h"
#include "core/server.h"
#include "ds/ds_common.h"
#include "persistence/crc64.h"
#include "persistence/rdb_format.h"

namespace miniredis {

uint8_t RdbDeserializer::ReadUint8() {
  if (pos_ >= size_) return 0;
  return data_[pos_++];
}

uint16_t RdbDeserializer::ReadUint16LE() {
  if (pos_ + 2 > size_) return 0;
  uint16_t v = ds::LoadLE16(data_ + pos_);
  pos_ += 2;
  return v;
}

uint32_t RdbDeserializer::ReadUint32LE() {
  if (pos_ + 4 > size_) return 0;
  uint32_t v = ds::LoadLE32(data_ + pos_);
  pos_ += 4;
  return v;
}

uint64_t RdbDeserializer::ReadUint64LE() {
  if (pos_ + 8 > size_) return 0;
  uint64_t v = ds::LoadLE64(data_ + pos_);
  pos_ += 8;
  return v;
}

size_t RdbDeserializer::ReadLen() {
  uint8_t first = ReadUint8();
  uint8_t enc = (first >> 6) & 0x03;
  if (enc == rdb::kLen6Bit) return first & 0x3F;
  if (enc == rdb::kLen14Bit) {
    uint8_t low = ReadUint8();
    return ((static_cast<size_t>(first) & 0x3F) << 8) | low;
  }
  if (first == rdb::kLen32Bit) return ReadUint32LE();
  if (first == rdb::kLen64Bit) return static_cast<size_t>(ReadUint64LE());
  return first;
}

std::string RdbDeserializer::ReadString() {
  // Check for int encoding
  uint8_t first = (pos_ < size_) ? data_[pos_] : 0;
  if (first == rdb::kEncInt8) {
    pos_++;
    int8_t v = static_cast<int8_t>(ReadUint8());
    return std::to_string(v);
  }
  if (first == rdb::kEncInt16) {
    pos_++;
    int16_t v = static_cast<int16_t>(ReadUint16LE());
    return std::to_string(v);
  }
  if (first == rdb::kEncInt32) {
    pos_++;
    int32_t v = static_cast<int32_t>(ReadUint32LE());
    return std::to_string(v);
  }

  size_t len = ReadLen();
  if (pos_ + len > size_) return {};
  std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
  pos_ += len;
  return s;
}

double RdbDeserializer::ReadDouble() {
  uint8_t marker = ReadUint8();
  if (marker == 253) {
    union {
      uint64_t u;
      double d;
    } u;
    u.u = ReadUint64LE();
    return u.d;
  }
  // Could parse string representation, but keep it simple for now.
  return 0.0;
}

StringValue RdbDeserializer::LoadStringValue() {
  return StringValue(ReadString());
}

ListValue RdbDeserializer::LoadListValue() {
  ListValue lv;
  size_t count = ReadLen();
  for (size_t i = 0; i < count; i++) {
    lv.PushTail(ReadString());
  }
  return lv;
}

SetValue RdbDeserializer::LoadSetValue() {
  SetValue sv;
  size_t count = ReadLen();
  for (size_t i = 0; i < count; i++) {
    sv.Add(ReadString());
  }
  return sv;
}

HashValue RdbDeserializer::LoadHashValue() {
  HashValue hv;
  size_t count = ReadLen();
  for (size_t i = 0; i < count; i++) {
    std::string field = ReadString();
    std::string value = ReadString();
    hv.Set(field, value);
  }
  return hv;
}

ZSetValue RdbDeserializer::LoadZSetValue() {
  ZSetValue zv;
  size_t count = ReadLen();
  for (size_t i = 0; i < count; i++) {
    std::string element = ReadString();
    double score = ReadDouble();
    zv.Add(element, score);
  }
  return zv;
}

Value RdbDeserializer::LoadValue(uint8_t type_byte) {
  switch (type_byte) {
    case rdb::kTypeString:
      return LoadStringValue();
    case rdb::kTypeList:
      return LoadListValue();
    case rdb::kTypeSet:
      return LoadSetValue();
    case rdb::kTypeHash:
      return LoadHashValue();
    case rdb::kTypeZSet2:
      return LoadZSetValue();
    default:
      return StringValue("");
  }
}

bool RdbDeserializer::Load(const std::string& filepath, Server& server) {
  // Read entire file into memory.
  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;
  std::streamsize fsize = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), fsize)) return false;
  file.close();

  data_ = buffer.data();
  size_ = buffer.size();
  pos_ = 0;

  if (size_ < 9) return false;

  // Verify magic
  if (std::memcmp(data_, rdb::kRdbMagic, 5) != 0) return false;
  // Version string "0011"
  if (std::memcmp(data_ + 5, "0011", 4) != 0) return false;
  pos_ = 9;

  // Read CRC64 from the end
  if (size_ < 8) return false;
  uint64_t expected_crc = ds::LoadLE64(data_ + size_ - 8);

  // Verify CRC64 (everything after header, before crc)
  uint64_t actual_crc = Crc64(data_ + 9, size_ - 9 - 8, 0);
  if (actual_crc != expected_crc) return false;

  int current_db = 0;

  while (pos_ < size_ - 8) {
    uint8_t op = ReadUint8();

    if (op == rdb::kOpEof) break;

    if (op == rdb::kOpSelectDb) {
      current_db = static_cast<int>(ReadLen());
      continue;
    }

    if (op == rdb::kOpResizeDb) {
      ReadLen();  // db size hint (ignored)
      ReadLen();  // expires size hint (ignored)
      continue;
    }

    if (op == rdb::kOpAux) {
      ReadString();  // aux key
      ReadString();  // aux value
      continue;
    }

    std::optional<int64_t> expire_ms;
    if (op == rdb::kOpExpireTimeMs) {
      expire_ms = static_cast<int64_t>(ReadUint64LE());
      op = ReadUint8();  // actual type byte
    } else if (op == rdb::kOpExpireTime) {
      expire_ms = static_cast<int64_t>(ReadUint32LE()) * 1000;
      op = ReadUint8();  // actual type byte
    }

    std::string key = ReadString();
    Value value = LoadValue(op);

    // Skip expired keys during load.
    if (expire_ms.has_value()) {
      auto now = std::chrono::system_clock::now().time_since_epoch();
      int64_t now_ms = static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
      if (*expire_ms <= now_ms) {
        continue;
      }
    }

    Database* db = server.GetDb(current_db);
    if (!db) continue;
    db->RestoreValue(std::move(key), std::move(value), expire_ms);
  }

  return true;
}

}  // namespace miniredis
