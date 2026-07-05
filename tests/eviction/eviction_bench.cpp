#include <chrono>
#include <iostream>
#include <string>

#include "core/config.h"
#include "core/database.h"
#include "core/server.h"
#include "eviction/evict.h"
#include "eviction/expire.h"

namespace miniredis {
namespace {

using Clock = std::chrono::steady_clock;

std::string Payload(size_t len, char ch) { return std::string(len, ch); }

void FillKeys(Server& server, size_t count, size_t payload_size,
              bool with_expire) {
  auto* db = server.GetDb(0);
  for (size_t i = 0; i < count; ++i) {
    std::string key = "key:" + std::to_string(i);
    db->Set(key, StringValue(Payload(payload_size, 'a')));
    if (with_expire) {
      db->SetExpire(key, 100);
    }
  }
}

void RunEvictionBench() {
  auto& server = Server::Instance();
  MiniRedisConfig cfg;
  cfg.databases = 1;
  cfg.maxmemory_policy = "allkeys_lru";
  cfg.maxmemory_samples = 5;
  server.Init(cfg);
  server.UpdateLruClock(1000);
  FillKeys(server, 5000, 128, false);

  size_t used = server.ApproxMemoryUsage();
  server.ApplyConfig("maxmemory", std::to_string(used / 2));

  auto start = Clock::now();
  auto result = PerformEvictions(server);
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - start);

  std::cout << "eviction_us=" << elapsed.count()
            << " result=" << (result == EvictionResult::kOk ? "ok" : "oom")
            << " evicted=" << server.Stats().evicted_keys
            << " remaining_memory=" << server.ApproxMemoryUsage() << '\n';
}

void RunExpireBench() {
  auto& server = Server::Instance();
  MiniRedisConfig cfg;
  cfg.databases = 1;
  cfg.hz = 10;
  cfg.active_expire_effort = 1;
  server.Init(cfg);
  FillKeys(server, 5000, 32, true);

  ActiveExpireConfig expire_cfg;
  expire_cfg.keys_per_loop = 20;
  expire_cfg.slow_time_perc = 100;

  auto start = Clock::now();
  size_t expired = ActiveExpireCycle(server, 1000, expire_cfg);
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - start);

  std::cout << "expire_us=" << elapsed.count() << " expired=" << expired
            << " remaining_expires=" << server.GetDb(0)->ExpiresSize() << '\n';
}

}  // namespace
}  // namespace miniredis

int main() {
  miniredis::RunEvictionBench();
  miniredis::RunExpireBench();
  return 0;
}
