#include "eviction/evict.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "core/database.h"
#include "core/server.h"

namespace miniredis {

namespace {

constexpr int kEvpoolSize = 16;
constexpr uint32_t kLruClockMax = 0xFFFFFF;

// Compute idle time from LRU clock values, handling wraparound.
unsigned long long EstimateIdleTime(uint32_t lru_clock, uint32_t stored_clock) {
  if (lru_clock >= stored_clock) {
    return static_cast<unsigned long long>(lru_clock - stored_clock);
  }
  return static_cast<unsigned long long>(lru_clock +
                                         (kLruClockMax - stored_clock));
}

struct EvictionPoolEntry {
  unsigned long long idle = 0;
  std::string key;
  int dbid = 0;

  bool Empty() const { return key.empty(); }
  void Clear() {
    idle = 0;
    key.clear();
    dbid = 0;
  }
};

// Fill the eviction pool with candidate keys sorted by idle time (ascending).
// The pool has the least-idle entry at position 0 and most-idle at the end.
bool EvictionPoolPopulate(int dbid, Database& db,
                          const std::vector<std::string>& samples,
                          uint32_t lru_clock,
                          std::array<EvictionPoolEntry, kEvpoolSize>& pool) {
  bool inserted = false;
  for (const auto& key : samples) {
    auto lr = db.LruOf(key);
    if (!lr.has_value()) continue;
    auto idle = EstimateIdleTime(lru_clock, *lr);

    if (!pool[0].Empty() && !pool.back().Empty() && idle <= pool[0].idle) {
      continue;
    }

    std::vector<EvictionPoolEntry> entries;
    entries.reserve(kEvpoolSize + 1);
    for (const auto& entry : pool) {
      if (!entry.Empty()) entries.push_back(entry);
    }
    entries.push_back({idle, key, dbid});
    std::sort(
        entries.begin(), entries.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.idle < rhs.idle; });
    if (entries.size() > kEvpoolSize) {
      entries.erase(entries.begin());
    }

    for (auto& entry : pool) entry.Clear();
    for (size_t i = 0; i < entries.size(); ++i) {
      pool[i] = std::move(entries[i]);
    }
    inserted = true;
  }
  return inserted;
}

// Get maxmemory policy as enum from config.
enum class Policy { kNoEviction, kAllKeysLru, kVolatileLru };

Policy ParsePolicy(const std::string& s) {
  if (s == "allkeys_lru") return Policy::kAllKeysLru;
  if (s == "volatile_lru") return Policy::kVolatileLru;
  return Policy::kNoEviction;
}

int BestCandidateIndex(const std::array<EvictionPoolEntry, kEvpoolSize>& pool) {
  for (int i = kEvpoolSize - 1; i >= 0; i--) {
    if (!pool[static_cast<size_t>(i)].Empty()) return i;
  }
  return -1;
}

}  // namespace

EvictionResult PerformEvictions(Server& server) {
  size_t maxmemory = server.GetConfig().maxmemory;
  if (maxmemory == 0) return EvictionResult::kOk;

  Policy policy = ParsePolicy(server.GetConfig().maxmemory_policy);
  if (policy == Policy::kNoEviction) return EvictionResult::kOk;

  size_t mem_used = server.ApproxMemoryUsage();
  if (mem_used <= maxmemory) return EvictionResult::kOk;

  uint32_t lru_clock = server.LruClock();
  int samples_per_iter = server.GetConfig().maxmemory_samples;
  int db_count = server.DbCount();

  std::array<EvictionPoolEntry, kEvpoolSize> pool;
  uint64_t sample_seed = 0;

  // Evict until memory is below limit, or we can't evict anymore.
  while (mem_used > maxmemory) {
    int best_idx = BestCandidateIndex(pool);
    if (best_idx < 0) {
      bool inserted = false;
      bool only_volatile = (policy == Policy::kVolatileLru);
      for (int dbid = 0; dbid < db_count; dbid++) {
        Database* db = server.GetDb(dbid);
        if (!db) continue;
        if (only_volatile && db->ExpiresSize() == 0) continue;

        auto samples = db->SampleKeys(static_cast<size_t>(samples_per_iter),
                                      only_volatile, sample_seed++);
        inserted |= EvictionPoolPopulate(dbid, *db, samples, lru_clock, pool);
      }

      best_idx = BestCandidateIndex(pool);
      if (!inserted || best_idx < 0) return EvictionResult::kNoMemory;
    }

    // Delete the best candidate.
    auto& candidate = pool[static_cast<size_t>(best_idx)];
    int target_db = candidate.dbid;
    std::string key_to_delete = std::move(candidate.key);
    candidate.Clear();

    Database* db = server.GetDb(target_db);
    if (db) {
      std::optional<size_t> freed = db->ApproxMemoryUsageOf(key_to_delete);
      if (db->Delete(key_to_delete)) {
        if (freed.has_value()) {
          mem_used = *freed >= mem_used ? 0 : mem_used - *freed;
        } else {
          mem_used = server.ApproxMemoryUsage();
        }
        server.IncrementEvicted(1);
      }
    }
  }

  return EvictionResult::kOk;
}

bool ShouldRejectWriteForOom(const Server& server, bool is_write_command) {
  if (!is_write_command) return false;
  if (server.GetConfig().maxmemory == 0) return false;
  if (ParsePolicy(server.GetConfig().maxmemory_policy) != Policy::kNoEviction)
    return false;
  return server.ApproxMemoryUsage() > server.GetConfig().maxmemory;
}

}  // namespace miniredis
