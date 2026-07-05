#include "core/database.h"

#include <chrono>

namespace miniredis {

namespace {

constexpr size_t kDictEntryOverhead = 32;
constexpr size_t kExpireMetadataOverhead = sizeof(int64_t);
constexpr size_t kLruMetadataOverhead = sizeof(uint32_t);

}  // namespace

Database::Database() = default;

bool Database::ExpireIfNeeded(std::string_view key) {
  auto* entry = entries_.FindView(key);
  if (!entry || !entry->expire_at_ms.has_value()) return false;
  if (*entry->expire_at_ms > NowMs()) return false;
  return DeleteEntry(key);
}

bool Database::DeleteEntry(std::string_view key) {
  auto* entry = entries_.FindView(key);
  if (!entry) return false;
  if (entry->expire_at_ms.has_value()) {
    volatile_count_--;
  }
  return entries_.DeleteView(key);
}

void Database::SetEntryExpire(Entry& entry,
                              std::optional<int64_t> expire_at_ms) {
  bool had_expire = entry.expire_at_ms.has_value();
  bool has_expire = expire_at_ms.has_value();
  entry.expire_at_ms = expire_at_ms;

  if (!had_expire && has_expire) {
    volatile_count_++;
  } else if (had_expire && !has_expire) {
    volatile_count_--;
  }
}

size_t Database::PurgeExpiredKeys(int64_t now_ms) {
  size_t removed = 0;
  auto it = entries_.SafeBegin();
  auto end = entries_.SafeEnd();
  while (it != end) {
    std::string_view key = it->key;
    auto expire_at = it->value.expire_at_ms;
    ++it;
    if (!expire_at.has_value() || *expire_at > now_ms) continue;
    DeleteEntry(key);
    removed++;
  }
  return removed;
}

ExpireSampleResult Database::ExpireSome(int64_t now_ms, size_t count,
                                        uint64_t seed) {
  ExpireSampleResult result;
  if (count == 0 || volatile_count_ == 0) return result;

  auto samples = entries_.GetSomeKeys(entries_.Size(), seed);
  for (auto* entry : samples) {
    if (!entry->value.expire_at_ms.has_value()) continue;
    if (result.sampled >= count) break;

    std::string key = entry->key;
    result.sampled++;
    if (*entry->value.expire_at_ms > now_ms) continue;
    if (DeleteEntry(key)) {
      result.expired++;
    }
  }
  return result;
}

void Database::ForEachKey(KeyVisitor visitor) {
  PurgeExpiredKeys(NowMs());
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    KeyView kv{
        /*key=*/it->key,
        /*value=*/it->value.value,
        /*expire_at_ms=*/it->value.expire_at_ms,
        /*lru_clock=*/it->value.lru_clock,
    };
    visitor(kv);
  }
}

std::vector<std::string> Database::SampleKeys(size_t count, bool only_volatile,
                                              uint64_t seed) {
  std::vector<std::string> result;
  if (count == 0 || entries_.Size() == 0) return result;

  if (only_volatile && volatile_count_ == 0) return result;

  size_t sample_count = only_volatile ? entries_.Size() : count;
  auto samples = entries_.GetSomeKeys(sample_count, seed);
  for (auto* entry : samples) {
    if (only_volatile && !entry->value.expire_at_ms.has_value()) {
      continue;
    }
    result.push_back(entry->key);
    if (result.size() >= count) break;
  }
  return result;
}

std::optional<int64_t> Database::ExpireAt(std::string_view key) const {
  auto* entry = entries_.FindView(key);
  if (!entry) return std::nullopt;
  return entry->expire_at_ms;
}

void Database::RestoreValue(std::string key, Value value,
                            std::optional<int64_t> expire_at_ms) {
  if (auto* existing = entries_.FindView(key);
      existing && existing->expire_at_ms.has_value()) {
    volatile_count_--;
  }
  Entry entry{
      /*value=*/std::move(value),
      /*expire_at_ms=*/expire_at_ms,
      /*lru_clock=*/current_lru_clock_,
  };
  if (entry.expire_at_ms.has_value()) volatile_count_++;
  entries_.Set(std::move(key), std::move(entry));
}

size_t Database::ApproxMemoryUsage() const {
  size_t total = 0;
  // Iterate via const_cast for read-only estimation (SafeIterator prevents
  // rehash so const is fine).
  auto* self = const_cast<Database*>(this);
  for (auto it = self->entries_.SafeBegin(); it != self->entries_.SafeEnd();
       ++it) {
    total += it->key.size();
    total += miniredis::ApproxMemoryUsage(it->value.value);
    total += kDictEntryOverhead;
    total += kLruMetadataOverhead;
    if (it->value.expire_at_ms.has_value()) total += kExpireMetadataOverhead;
  }
  return total;
}

std::optional<size_t> Database::ApproxMemoryUsageOf(
    std::string_view key) const {
  auto* entry = entries_.FindView(key);
  if (!entry) return std::nullopt;

  size_t total = key.size();
  total += miniredis::ApproxMemoryUsage(entry->value);
  total += kDictEntryOverhead;
  total += kLruMetadataOverhead;
  if (entry->expire_at_ms.has_value()) total += kExpireMetadataOverhead;
  return total;
}

void Database::SetCurrentLruClock(uint32_t clock) {
  current_lru_clock_ = clock;
}

void Database::Touch(std::string_view key) {
  auto* entry = entries_.FindView(key);
  if (entry) entry->lru_clock = current_lru_clock_;
}

std::optional<uint32_t> Database::LruOf(std::string_view key) const {
  auto* entry = entries_.FindView(key);
  if (!entry) return std::nullopt;
  return entry->lru_clock;
}

int64_t Database::NowMs() const {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool Database::Exists(std::string_view key) {
  ExpireIfNeeded(key);
  return entries_.FindView(key) != nullptr;
}

std::optional<ValueType> Database::Type(std::string_view key) {
  ExpireIfNeeded(key);
  auto* entry = entries_.FindView(key);
  if (!entry) return std::nullopt;
  return GetType(entry->value);
}

bool Database::Delete(std::string_view key) { return DeleteEntry(key); }

Value* Database::Find(std::string_view key) {
  ExpireIfNeeded(key);
  auto* entry = entries_.FindView(key);
  if (!entry) return nullptr;
  entry->lru_clock = current_lru_clock_;
  return &entry->value;
}

bool Database::Set(std::string_view key, Value value) {
  if (auto* existing = entries_.FindView(key);
      existing && existing->expire_at_ms.has_value()) {
    volatile_count_--;
  }
  Entry entry{
      /*value=*/std::move(value),
      /*expire_at_ms=*/std::nullopt,
      /*lru_clock=*/current_lru_clock_,
  };
  return entries_.SetView(key, std::move(entry));
}

bool Database::Rename(std::string_view old_key, std::string_view new_key) {
  std::string old_k(old_key);
  std::string new_k(new_key);
  ExpireIfNeeded(old_key);
  auto* old_entry = entries_.FindView(old_key);
  if (!old_entry) return false;
  if (old_k == new_k) return true;

  ExpireIfNeeded(new_key);
  old_entry = entries_.FindView(old_key);
  if (!old_entry) return false;

  Entry moved = std::move(*old_entry);
  DeleteEntry(old_k);
  DeleteEntry(new_k);
  if (moved.expire_at_ms.has_value()) volatile_count_++;
  entries_.Set(std::move(new_k), std::move(moved));
  return true;
}

bool Database::RenameNX(std::string_view old_key, std::string_view new_key) {
  ExpireIfNeeded(new_key);
  if (entries_.FindView(new_key)) return false;
  return Rename(old_key, new_key);
}

std::vector<std::string> Database::Keys(std::string_view pattern) {
  (void)pattern;
  PurgeExpiredKeys(NowMs());
  // Phase 3: only support '*' pattern
  std::vector<std::string> result;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    result.push_back(it->key);
  }
  return result;
}

std::optional<std::string> Database::RandomKey() {
  PurgeExpiredKeys(NowMs());
  return entries_.RandomKey();
}

size_t Database::Size() {
  PurgeExpiredKeys(NowMs());
  return entries_.Size();
}

size_t Database::ExpiresSize() const { return volatile_count_; }

bool Database::SetExpire(std::string_view key, int64_t expire_at_ms) {
  ExpireIfNeeded(key);
  auto* entry = entries_.FindView(key);
  if (!entry) return false;
  SetEntryExpire(*entry, expire_at_ms);
  return true;
}

bool Database::Persist(std::string_view key) {
  ExpireIfNeeded(key);
  auto* entry = entries_.FindView(key);
  if (!entry) return false;
  bool had_expire = entry->expire_at_ms.has_value();
  SetEntryExpire(*entry, std::nullopt);
  return had_expire;
}

int64_t Database::TTL(std::string_view key) {
  auto* entry = entries_.FindView(key);
  if (!entry) return -2;
  if (!entry->expire_at_ms.has_value()) return -1;
  int64_t remaining = *entry->expire_at_ms - NowMs();
  if (remaining < 0) {
    DeleteEntry(key);
    return -2;
  }
  return remaining;
}

bool Database::IsExpired(std::string_view key) const {
  auto* entry = entries_.FindView(key);
  if (!entry || !entry->expire_at_ms.has_value()) return false;
  return *entry->expire_at_ms <= NowMs();
}

void Database::Clear() {
  entries_.Clear();
  volatile_count_ = 0;
}

}  // namespace miniredis
