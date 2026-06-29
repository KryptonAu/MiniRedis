#include "eviction/server_cron.h"

#include "core/database.h"
#include "core/server.h"
#include "eviction/evict.h"
#include "eviction/expire.h"

namespace miniredis {

void ServerCron(Server& server, CronServices services, int64_t now_ms) {
  // 1. Update LRU clock
  server.UpdateLruClock(now_ms);

  // 2. Active expiration
  ActiveExpireCycle(server, now_ms);

  // 3. Memory eviction
  if (server.GetConfig().maxmemory > 0) {
    PerformEvictions(server);
  }

  // 4. AOF flush (if enabled)
  if (services.flush_aof_if_needed) {
    services.flush_aof_if_needed();
  }

  // 5. Auto-save check
  if (services.trigger_autosave) {
    services.trigger_autosave();
  }
}

}  // namespace miniredis
