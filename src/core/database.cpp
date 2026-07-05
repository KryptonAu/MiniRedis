#include "core/database.h"

#include <chrono>
#include <cstring>

namespace miniredis {

static constexpr uint32_t kLruClockMax = 0xFFFFFF;  // 24-bit

Database::Database() = default;

bool Database::ExpireIfNeeded(std::string_view key) {
  auto* expire_ms = expires_.FindView(key);
  if (!expire_ms) return false;            // no expiry set
  if (*expire_ms > NowMs()) return false;  // not yet expired
  // Expired — delete
  keyspace_.DeleteView(key);
  expires_.DeleteView(key);
  lru_.DeleteView(key);
  return true;
}

size_t Database::PurgeExpiredKeys(int64_t now_ms) {
  size_t removed = 0;
  auto it = expires_.SafeBegin();
  auto end = expires_.SafeEnd();
  while (it != end) {
    std::string_view key = it->key;
    int64_t expire_at = it->value;
    ++it;
    if (expire_at > now_ms) continue;  // not yet expired
    keyspace_.DeleteView(key);
    lru_.DeleteView(key);
    expires_.DeleteView(key);
    removed++;
  }
  return removed;
}

ExpireSampleResult Database::ExpireSome(int64_t now_ms, size_t count,
                                        uint64_t seed) {
  ExpireSampleResult result;
  if (count == 0 || expires_.Size() == 0) return result;

  auto samples = expires_.GetSomeKeys(count, seed);
  result.sampled = samples.size();
  for (auto* entry : samples) {
    std::string key = entry->key;
    if (entry->value > now_ms) continue;
    expires_.DeleteView(key);
    lru_.DeleteView(key);
    if (keyspace_.DeleteView(key)) {
      result.expired++;
    }
  }
  return result;
}

void Database::ForEachKey(KeyVisitor visitor) {
  PurgeExpiredKeys(NowMs());
  for (auto it = keyspace_.begin(); it != keyspace_.end(); ++it) {
    auto* exp = expires_.FindView(it->key);
    auto* lr = lru_.FindView(it->key);
    KeyView kv{
        /*key=*/it->key,
        /*value=*/it->value,
        /*expire_at_ms=*/exp ? std::optional<int64_t>(*exp) : std::nullopt,
        /*lru_clock=*/lr ? *lr : 0,
    };
    visitor(kv);
  }
}

std::vector<std::string> Database::SampleKeys(size_t count, bool only_volatile,
                                              uint64_t seed) {
  std::vector<std::string> result;
  if (count == 0 || keyspace_.Size() == 0) return result;

  if (only_volatile && expires_.Size() == 0) return result;

  // Sample from expires_ (only volatile) or keyspace_ (all keys).
  if (only_volatile) {
    auto samples = expires_.GetSomeKeys(count, seed);
    for (auto* entry : samples) {
      result.push_back(entry->key);
    }
  } else {
    auto samples = keyspace_.GetSomeKeys(count, seed);
    for (auto* entry : samples) {
      result.push_back(entry->key);
    }
  }
  return result;
}

std::optional<int64_t> Database::ExpireAt(std::string_view key) const {
  auto* v = expires_.FindView(key);
  if (!v) return std::nullopt;
  return *v;
}

void Database::RestoreValue(std::string key, Value value,
                            std::optional<int64_t> expire_at_ms) {
  // Always update LRU clock on restore.
  keyspace_.SetView(key, std::move(value));
  lru_.SetView(key, current_lru_clock_);

  if (expire_at_ms.has_value()) {
    expires_.SetView(key, *expire_at_ms);
  } else {
    expires_.DeleteView(key);
  }
}

size_t Database::ApproxMemoryUsage() const {
  size_t total = 0;
  // Iterate via const_cast for read-only estimation (SafeIterator prevents
  // rehash so const is fine).
  auto* self = const_cast<Database*>(this);
  for (auto it = self->keyspace_.SafeBegin(); it != self->keyspace_.SafeEnd();
       ++it) {
    total += it->key.size();                           // key
    total += miniredis::ApproxMemoryUsage(it->value);  // value
  }
  // Estimate dict overhead: ~32 bytes per entry for bucket pointers + metadata
  total += keyspace_.Size() * 32;
  total += expires_.Size() * 24;
  total += lru_.Size() * 12;
  return total;
}

std::optional<size_t> Database::ApproxMemoryUsageOf(
    std::string_view key) const {
  auto* value = keyspace_.FindView(key);
  if (!value) return std::nullopt;

  size_t total = key.size();
  total += miniredis::ApproxMemoryUsage(*value);
  total += 32;
  if (expires_.FindView(key)) total += 24;
  if (lru_.FindView(key)) total += 12;
  return total;
}

void Database::SetCurrentLruClock(uint32_t clock) {
  current_lru_clock_ = clock;
}

void Database::Touch(std::string_view key) {
  lru_.SetView(key, current_lru_clock_);
}

std::optional<uint32_t> Database::LruOf(std::string_view key) const {
  auto* v = lru_.FindView(key);
  if (!v) return std::nullopt;
  return *v;
}

int64_t Database::NowMs() const {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool Database::Exists(std::string_view key) {
  ExpireIfNeeded(key);
  return keyspace_.FindView(key) != nullptr;
}

std::optional<ValueType> Database::Type(std::string_view key) {
  ExpireIfNeeded(key);
  auto* val = keyspace_.FindView(key);
  if (!val) return std::nullopt;
  return GetType(*val);
}

bool Database::Delete(std::string_view key) {
  expires_.DeleteView(key);
  lru_.DeleteView(key);
  return keyspace_.DeleteView(key);
}

Value* Database::Find(std::string_view key) {
  ExpireIfNeeded(key);
  auto* val = keyspace_.FindView(key);
  if (val != nullptr) {
    Touch(key);
  }
  return val;
}

bool Database::Set(std::string_view key, Value value) {
  expires_.DeleteView(key);  // SET clears TTL
  bool result = keyspace_.SetView(key, std::move(value));
  if (result) {
    Touch(key);
  }
  return result;
}

bool Database::Rename(std::string_view old_key, std::string_view new_key) {
  std::string old_k(old_key);
  std::string new_k(new_key);
  ExpireIfNeeded(old_key);
  auto* val_ptr = keyspace_.FindView(old_key);
  if (!val_ptr) return false;
  Value& val = *val_ptr;

  // Move value
  Value v = std::move(val);

  // Migrate TTL
  int64_t ttl = -1;
  if (auto* expire_ms_ptr = expires_.FindView(old_key)) {
    int64_t& expire_ms = *expire_ms_ptr;
    ttl = expire_ms;
  }

  // Migrate LRU
  uint32_t lru_val = current_lru_clock_;
  if (auto* lr_ptr = lru_.FindView(old_key)) {
    uint32_t& lr = *lr_ptr;
    lru_val = lr;
  }

  keyspace_.DeleteView(old_k);
  expires_.DeleteView(old_k);
  lru_.DeleteView(old_k);

  keyspace_.SetView(new_k, std::move(v));
  lru_.SetView(new_k, lru_val);
  if (ttl >= 0) {
    expires_.SetView(new_k, ttl);
  } else {
    expires_.DeleteView(new_k);
  }
  return true;
}

bool Database::RenameNX(std::string_view old_key, std::string_view new_key) {
  ExpireIfNeeded(new_key);
  if (keyspace_.FindView(new_key)) return false;
  return Rename(old_key, new_key);
}

std::vector<std::string> Database::Keys(std::string_view pattern) {
  PurgeExpiredKeys(NowMs());
  // Phase 3: only support '*' pattern
  std::vector<std::string> result;
  for (auto it = keyspace_.begin(); it != keyspace_.end(); ++it) {
    result.push_back(it->key);
  }
  return result;
}

std::optional<std::string> Database::RandomKey() {
  PurgeExpiredKeys(NowMs());
  return keyspace_.RandomKey();
}

size_t Database::Size() {
  PurgeExpiredKeys(NowMs());
  return keyspace_.Size();
}

size_t Database::ExpiresSize() const { return expires_.Size(); }

bool Database::SetExpire(std::string_view key, int64_t expire_at_ms) {
  ExpireIfNeeded(key);
  if (!keyspace_.FindView(key)) return false;
  expires_.SetView(key, expire_at_ms);
  return true;
}

bool Database::Persist(std::string_view key) {
  ExpireIfNeeded(key);
  if (!keyspace_.FindView(key)) return false;
  return expires_.DeleteView(key);
}

int64_t Database::TTL(std::string_view key) {
  if (!keyspace_.FindView(key)) return -2;
  auto* expire_ms = expires_.FindView(key);
  if (!expire_ms) return -1;
  int64_t remaining = *expire_ms - NowMs();
  if (remaining < 0) {
    keyspace_.DeleteView(key);
    expires_.DeleteView(key);
    lru_.DeleteView(key);
    return -2;
  }
  return remaining;
}

bool Database::IsExpired(std::string_view key) const {
  auto* expire_ms = expires_.FindView(key);
  if (!expire_ms) return false;
  return *expire_ms <= NowMs();
}

void Database::Clear() {
  keyspace_.Clear();
  expires_.Clear();
  lru_.Clear();
}

}  // namespace miniredis
