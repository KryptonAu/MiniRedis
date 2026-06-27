#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/dict.h"
#include "types/value.h"

namespace miniredis {

class Database {
 public:
  Database();

  // Key CRUD (non-const — may trigger lazy expiration)
  bool Exists(std::string_view key);
  std::optional<ValueType> Type(std::string_view key);
  bool Delete(std::string_view key);

  Value* Find(std::string_view key);
  bool Set(std::string_view key, Value value);
  bool Rename(std::string_view old_key, std::string_view new_key);
  bool RenameNX(std::string_view old_key, std::string_view new_key);

  std::vector<std::string> Keys(std::string_view pattern = "*");
  std::optional<std::string> RandomKey();

  size_t Size();
  size_t ExpiresSize() const;

  // Expire
  bool SetExpire(std::string_view key, int64_t expire_at_ms);
  bool Persist(std::string_view key);
  int64_t TTL(std::string_view key);
  bool IsExpired(std::string_view key) const;

  void Clear();

 private:
  ds::Dict<std::string, Value> keyspace_;
  ds::Dict<std::string, int64_t> expires_;

  bool ExpireIfNeeded(std::string_view key);
  void PurgeExpiredKeys();
  int64_t NowMs() const;
};

}  // namespace miniredis
