#include "core/database.h"

#include <chrono>

namespace miniredis {

Database::Database() = default;

bool Database::ExpireIfNeeded(std::string_view key) {
  std::string k(key);
  auto* expire_ms = expires_.Find(k);
  if (!expire_ms) return false;            // no expiry set
  if (*expire_ms > NowMs()) return false;  // not yet expired
  // Expired — delete
  keyspace_.Delete(k);
  expires_.Delete(k);
  return true;
}

void Database::PurgeExpiredKeys() {
  auto it = expires_.SafeBegin();
  auto end = expires_.SafeEnd();
  while (it != end) {
    std::string key = it->key;
    ++it;
    ExpireIfNeeded(key);
  }
}

int64_t Database::NowMs() const {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool Database::Exists(std::string_view key) {
  std::string k(key);
  ExpireIfNeeded(k);
  return keyspace_.Find(k) != nullptr;
}

std::optional<ValueType> Database::Type(std::string_view key) {
  std::string k(key);
  ExpireIfNeeded(k);
  auto* val = keyspace_.Find(k);
  if (!val) return std::nullopt;
  return GetType(*val);
}

bool Database::Delete(std::string_view key) {
  std::string k(key);
  expires_.Delete(k);
  return keyspace_.Delete(k);
}

Value* Database::Find(std::string_view key) {
  std::string k(key);
  ExpireIfNeeded(k);
  return keyspace_.Find(k);
}

bool Database::Set(std::string_view key, Value value) {
  std::string k(key);
  expires_.Delete(k);  // SET clears TTL
  return keyspace_.Set(k, std::move(value));
}

bool Database::Rename(std::string_view old_key, std::string_view new_key) {
  std::string old_k(old_key);
  std::string new_k(new_key);
  ExpireIfNeeded(old_k);
  auto* val = keyspace_.Find(old_k);
  if (!val) return false;  // old doesn't exist

  // Move value
  Value v = std::move(*val);
  keyspace_.Delete(old_k);

  // Migrate TTL
  auto* expire_ms = expires_.Find(old_k);
  int64_t ttl = -1;
  if (expire_ms) {
    ttl = *expire_ms;
    expires_.Delete(old_k);
  }

  keyspace_.Set(new_k, std::move(v));  // overwrites new if exists
  if (ttl >= 0) {
    expires_.Set(new_k, ttl);
  } else {
    expires_.Delete(new_k);
  }
  return true;
}

bool Database::RenameNX(std::string_view old_key, std::string_view new_key) {
  std::string new_k(new_key);
  ExpireIfNeeded(new_k);
  if (keyspace_.Find(new_k)) return false;  // new key exists — fail
  return Rename(old_key, new_key);
}

std::vector<std::string> Database::Keys(std::string_view pattern) {
  PurgeExpiredKeys();
  // Phase 3: only support '*' pattern
  std::vector<std::string> result;
  for (auto it = keyspace_.begin(); it != keyspace_.end(); ++it) {
    result.push_back(it->key);
  }
  return result;
}

std::optional<std::string> Database::RandomKey() {
  PurgeExpiredKeys();
  return keyspace_.RandomKey();
}

size_t Database::Size() {
  PurgeExpiredKeys();
  return keyspace_.Size();
}

size_t Database::ExpiresSize() const { return expires_.Size(); }

bool Database::SetExpire(std::string_view key, int64_t expire_at_ms) {
  std::string k(key);
  ExpireIfNeeded(k);
  if (!keyspace_.Find(k)) return false;
  expires_.Set(k, expire_at_ms);
  return true;
}

bool Database::Persist(std::string_view key) {
  std::string k(key);
  ExpireIfNeeded(k);
  if (!keyspace_.Find(k)) return false;
  return expires_.Delete(k);
}

int64_t Database::TTL(std::string_view key) {
  std::string k(key);
  if (!keyspace_.Find(k)) return -2;  // not found
  auto* expire_ms = expires_.Find(k);
  if (!expire_ms) return -1;  // persistent
  int64_t remaining = *expire_ms - NowMs();
  if (remaining < 0) {
    // Expired
    keyspace_.Delete(k);
    expires_.Delete(k);
    return -2;
  }
  return remaining;
}

bool Database::IsExpired(std::string_view key) const {
  std::string k(key);
  auto* expire_ms = expires_.Find(k);
  if (!expire_ms) return false;
  return *expire_ms <= NowMs();
}

void Database::Clear() {
  keyspace_.Clear();
  expires_.Clear();
}

}  // namespace miniredis
