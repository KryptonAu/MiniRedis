#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/dict.h"
#include "types/value.h"

namespace miniredis {

// Lightweight read-only view of a key and its associated metadata.
struct KeyView {
  std::string_view key;
  const Value& value;
  std::optional<int64_t> expire_at_ms;
  uint32_t lru_clock;
};

using KeyVisitor = std::function<void(const KeyView&)>;

struct ExpireSampleResult {
  size_t sampled = 0;
  size_t expired = 0;
};

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

  // Purge all expired keys; returns the number of keys removed.
  size_t PurgeExpiredKeys(int64_t now_ms);
  ExpireSampleResult ExpireSome(int64_t now_ms, size_t count, uint64_t seed);

  // Traversal / sampling (public, for persistence/eviction modules).
  void ForEachKey(KeyVisitor visitor);
  std::vector<std::string> SampleKeys(size_t count, bool only_volatile,
                                      uint64_t seed = 0);
  std::optional<int64_t> ExpireAt(std::string_view key) const;

  // Restore a value (used by RDB/AOF loading). Preserves move-only Value.
  void RestoreValue(std::string key, Value value,
                    std::optional<int64_t> expire_at_ms);

  // Approximate memory usage of this database (keys + values + metadata).
  size_t ApproxMemoryUsage() const;
  std::optional<size_t> ApproxMemoryUsageOf(std::string_view key) const;

  // LRU tracking.
  void SetCurrentLruClock(uint32_t clock);
  void Touch(std::string_view key);
  std::optional<uint32_t> LruOf(std::string_view key) const;

  void Clear();

 private:
  struct Entry {
    Value value;
    std::optional<int64_t> expire_at_ms;
    uint32_t lru_clock = 0;
  };

  ds::Dict<std::string, Entry> entries_;
  size_t volatile_count_ = 0;
  uint32_t current_lru_clock_ = 0;

  bool ExpireIfNeeded(std::string_view key);
  bool DeleteEntry(std::string_view key);
  void SetEntryExpire(Entry& entry, std::optional<int64_t> expire_at_ms);
  int64_t NowMs() const;
};

}  // namespace miniredis
