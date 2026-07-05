#include "persistence/rdb_serialize.h"

#include <chrono>
#include <cstring>
#include <fstream>

#include "core/database.h"
#include "core/server.h"
#include "ds/ds_common.h"
#include "persistence/crc64.h"
#include "persistence/rdb_format.h"

namespace miniredis {

void RdbSerializer::WriteBytes(const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  buf_.append(reinterpret_cast<const char*>(p), len);
}

void RdbSerializer::WriteUint8(uint8_t v) { WriteBytes(&v, 1); }

void RdbSerializer::WriteUint16LE(uint16_t v) {
  uint8_t b[2];
  ds::StoreLE16(b, v);
  WriteBytes(b, 2);
}

void RdbSerializer::WriteUint32LE(uint32_t v) {
  uint8_t b[4];
  ds::StoreLE32(b, v);
  WriteBytes(b, 4);
}

void RdbSerializer::WriteUint64LE(uint64_t v) {
  uint8_t b[8];
  ds::StoreLE64(b, v);
  WriteBytes(b, 8);
}

void RdbSerializer::WriteLen(size_t len) {
  if (len < 64) {
    WriteUint8(static_cast<uint8_t>(len));
  } else if (len < 16384) {
    WriteUint8(static_cast<uint8_t>(rdb::kLen14BitPrefix | (len >> 8)));
    WriteUint8(static_cast<uint8_t>(len & 0xFF));
  } else {
    WriteUint8(rdb::kLen32Bit);
    WriteUint32LE(static_cast<uint32_t>(len));
  }
}

void RdbSerializer::WriteString(std::string_view s) {
  // Try int encoding for small integers.
  if (!s.empty() && (s[0] == '-' || (s[0] >= '0' && s[0] <= '9'))) {
    // Simple heuristic: only encode if the string is short and purely numeric.
    bool all_digits = true;
    for (size_t i = (s[0] == '-' ? 1 : 0); i < s.size(); i++) {
      if (s[i] < '0' || s[i] > '9') {
        all_digits = false;
        break;
      }
    }
    // Skip int encoding for now to keep things simple — write as raw string.
    (void)all_digits;
  }
  WriteLen(s.size());
  WriteBytes(s.data(), s.size());
}

void RdbSerializer::WriteDouble(double d) {
  // Write binary double (8 bytes, little-endian)
  WriteUint8(253);
  union {
    double d;
    uint64_t u;
  } u;
  u.d = d;
  WriteUint64LE(u.u);
}

void RdbSerializer::SaveHeader() {
  // Magic + version
  buf_.append(rdb::kRdbMagic, 5);
  buf_.append("0011", 4);
}

void RdbSerializer::SaveFooter() {
  WriteUint8(rdb::kOpEof);
  // CRC64 of everything after the header
  uint64_t crc = Crc64(buf_.data() + 9, buf_.size() - 9, 0);
  WriteUint64LE(crc);
}

void RdbSerializer::SaveType(uint8_t type_byte) { WriteUint8(type_byte); }

void RdbSerializer::SaveStringValue(const StringValue& sv) {
  StringValue::StringViewScratch scratch;
  WriteString(sv.ToStringView(scratch));
}

void RdbSerializer::SaveListValue(const ListValue& lv) {
  auto elements = lv.Range(0, -1);
  WriteLen(elements.size());
  for (const auto& elem : elements) {
    WriteString(elem);
  }
}

void RdbSerializer::SaveSetValue(const SetValue& sv) {
  auto members = sv.Members();
  WriteLen(members.size());
  for (const auto& m : members) {
    WriteString(m);
  }
}

void RdbSerializer::SaveHashValue(const HashValue& hv) {
  auto pairs = hv.GetAll();
  WriteLen(pairs.size());
  for (const auto& [field, val] : pairs) {
    WriteString(field);
    WriteString(val);
  }
}

void RdbSerializer::SaveZSetValue(const ZSetValue& zv) {
  auto range = zv.Range(0, -1);
  WriteLen(range.size());
  for (const auto& entry : range) {
    WriteString(entry.element);
    WriteDouble(entry.score);
  }
}

void RdbSerializer::SaveValue(const Value& v) {
  std::visit(
      [this](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, StringValue>) {
          SaveStringValue(val);
        } else if constexpr (std::is_same_v<T, ListValue>) {
          SaveListValue(val);
        } else if constexpr (std::is_same_v<T, SetValue>) {
          SaveSetValue(val);
        } else if constexpr (std::is_same_v<T, HashValue>) {
          SaveHashValue(val);
        } else if constexpr (std::is_same_v<T, ZSetValue>) {
          SaveZSetValue(val);
        }
      },
      v);
}

void RdbSerializer::SaveDatabase(int db_index, Server& server) {
  Database* db = server.GetDb(db_index);
  if (!db) return;

  // Purge expired keys before saving.
  db->PurgeExpiredKeys(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());

  if (db->Size() == 0) return;

  // SELECTDB
  WriteUint8(rdb::kOpSelectDb);
  WriteLen(static_cast<size_t>(db_index));

  // RESIZEDB
  WriteUint8(rdb::kOpResizeDb);
  WriteLen(db->Size());
  WriteLen(db->ExpiresSize());

  // Iterate all keys
  db->ForEachKey([this](const KeyView& kv) {
    // Expiry
    if (kv.expire_at_ms.has_value()) {
      WriteUint8(rdb::kOpExpireTimeMs);
      WriteUint64LE(static_cast<uint64_t>(*kv.expire_at_ms));
    }

    // Type byte + key
    ValueType type = GetType(kv.value);
    switch (type) {
      case ValueType::kString:
        SaveType(rdb::kTypeString);
        break;
      case ValueType::kList:
        SaveType(rdb::kTypeList);
        break;
      case ValueType::kSet:
        SaveType(rdb::kTypeSet);
        break;
      case ValueType::kHash:
        SaveType(rdb::kTypeHash);
        break;
      case ValueType::kZSet:
        SaveType(rdb::kTypeZSet2);
        break;
    }
    WriteString(kv.key);
    SaveValue(kv.value);
  });
}

bool RdbSerializer::SaveToString(std::string& out, Server& server) {
  buf_.clear();
  SaveHeader();

  int db_count = server.DbCount();
  for (int i = 0; i < db_count; i++) {
    SaveDatabase(i, server);
  }

  SaveFooter();
  out.swap(buf_);
  return true;
}

bool RdbSerializer::Save(const std::string& filepath, Server& server) {
  std::string data;
  if (!SaveToString(data, server)) return false;

  // Write to temp file, then rename for atomicity.
  std::string tmp = filepath + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file.good()) {
      file.close();
      return false;
    }
  }
  if (::rename(tmp.c_str(), filepath.c_str()) != 0) return false;
  return true;
}

}  // namespace miniredis
