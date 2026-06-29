#pragma once

#include <cstdint>
#include <string>

#include "types/value.h"

namespace miniredis {

class Server;

class RdbDeserializer {
 public:
  bool Load(const std::string& filepath, Server& server);

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;

  uint8_t ReadUint8();
  uint16_t ReadUint16LE();
  uint32_t ReadUint32LE();
  uint64_t ReadUint64LE();
  size_t ReadLen();
  std::string ReadString();
  double ReadDouble();

  StringValue LoadStringValue();
  ListValue LoadListValue();
  SetValue LoadSetValue();
  HashValue LoadHashValue();
  ZSetValue LoadZSetValue();
  Value LoadValue(uint8_t type_byte);
};

}  // namespace miniredis
