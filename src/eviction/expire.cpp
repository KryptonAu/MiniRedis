#include "eviction/expire.h"

#include <chrono>

#include "core/database.h"
#include "core/server.h"

namespace miniredis {

namespace {

int64_t NowUs() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

size_t ActiveExpireCycle(Server& server, int64_t now_ms,
                         const ActiveExpireConfig& config) {
  size_t total_expired = 0;
  int db_count = server.DbCount();
  if (db_count == 0) return 0;

  // Per-call DB rotation state (function-static to survive across calls,
  // mirrors Redis's static current_db).
  static int current_db = 0;

  int64_t start_us = NowUs();
  int64_t timelimit_us = static_cast<int64_t>(
      server.GetConfig().hz > 0
          ? (1000 / server.GetConfig().hz) * 1000 * config.slow_time_perc / 100
          : config.fast_duration_us);
  if (timelimit_us <= 0) timelimit_us = 1000;

  int dbs_per_call = std::min(db_count, 16);

  for (int j = 0; j < dbs_per_call; j++) {
    // Check time budget.
    if (NowUs() - start_us > timelimit_us) break;

    int db_idx = current_db % db_count;
    current_db++;

    Database* db = server.GetDb(db_idx);
    if (!db || db->ExpiresSize() == 0) continue;

    size_t sampled = 0;
    size_t expired = 0;
    int iteration = 0;

    do {
      if (NowUs() - start_us > timelimit_us) break;

      // Sample keys from expires_ dict.
      auto samples = db->SampleKeys(
          static_cast<size_t>(config.keys_per_loop), /*only_volatile=*/true,
          static_cast<uint64_t>(start_us + iteration));
      sampled += samples.size();

      for (const auto& key : samples) {
        auto expire_at = db->ExpireAt(key);
        if (expire_at.has_value() && *expire_at <= now_ms) {
          db->Delete(key);
          expired++;
        }
      }
      total_expired += expired;
      server.IncrementExpired(expired);
      iteration++;

      // If expire ratio > 10%, keep scanning this DB.
    } while (sampled > 0 && expired * 10 > sampled && iteration < 100);
  }

  return total_expired;
}

}  // namespace miniredis
