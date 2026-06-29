#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "types/value.h"

namespace miniredis {

class Server;

class RdbSerializer {
 public:
  bool Save(const std::string& filepath, Server& server);
  bool SaveToString(std::string& out, Server& server);

 private:
  std::string buf_;
  uint64_t crc_ = 0;

  void WriteBytes(const void* data, size_t len);
  void WriteUint8(uint8_t v);
  void WriteUint16LE(uint16_t v);
  void WriteUint32LE(uint32_t v);
  void WriteUint64LE(uint64_t v);
  void WriteLen(size_t len);
  void WriteString(std::string_view s);
  void WriteDouble(double d);

  void SaveHeader();
  void SaveFooter();
  void SaveType(uint8_t type_byte);
  void SaveDatabase(int db_index, Server& server);
  void SaveStringValue(const StringValue& sv);
  void SaveListValue(const ListValue& lv);
  void SaveSetValue(const SetValue& sv);
  void SaveHashValue(const HashValue& hv);
  void SaveZSetValue(const ZSetValue& zv);
  void SaveValue(const Value& v);
};

}  // namespace miniredis
