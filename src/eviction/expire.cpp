#include "eviction/expire.h"

#include <algorithm>
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

int ClampEffort(int effort) {
  if (effort < 1) return 1;
  if (effort > 10) return 10;
  return effort;
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

  int effort = ClampEffort(server.GetConfig().active_expire_effort);
  int base_keys_per_loop = std::max(config.keys_per_loop, 1);
  size_t keys_per_loop =
      static_cast<size_t>(base_keys_per_loop + (effort - 1) * 5);
  int slow_time_perc = config.slow_time_perc + (effort - 1) * 2;

  int64_t start_us = NowUs();
  int64_t timelimit_us = static_cast<int64_t>(
      server.GetConfig().hz > 0
          ? (1000 / server.GetConfig().hz) * 1000 * slow_time_perc / 100
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

    int iteration = 0;

    do {
      if (NowUs() - start_us > timelimit_us) break;

      auto result = db->ExpireSome(now_ms, keys_per_loop,
                                   static_cast<uint64_t>(start_us + iteration));
      total_expired += result.expired;
      server.IncrementExpired(result.expired);
      iteration++;

      // If expire ratio > 10%, keep scanning this DB.
      if (result.sampled == 0 || result.expired * 10 <= result.sampled) break;
    } while (iteration < 100);
  }

  return total_expired;
}

}  // namespace miniredis
