#include "eviction/expire.h"

#include <gtest/gtest.h>

#include <string>

#include "core/config.h"
#include "core/database.h"
#include "core/server.h"
#include "eviction/evict.h"

namespace miniredis {
namespace {

std::string Payload(size_t len) { return std::string(len, 'x'); }

Server& ResetServer(MiniRedisConfig cfg = {}) {
  cfg.databases = 1;
  auto& server = Server::Instance();
  server.Init(cfg);
  server.UpdateLruClock(1000);
  return server;
}

void FillKeys(Database& db, size_t count, size_t payload_size,
              bool with_expire) {
  for (size_t i = 0; i < count; ++i) {
    std::string key = "key:" + std::to_string(i);
    db.Set(key, StringValue(Payload(payload_size)));
    if (with_expire) {
      db.SetExpire(key, 100);
    }
  }
}

TEST(EvictionTest, AllKeysLruEvictsUntilBelowMaxmemory) {
  MiniRedisConfig cfg;
  cfg.maxmemory_policy = "allkeys_lru";
  cfg.maxmemory_samples = 5;
  auto& server = ResetServer(cfg);
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  FillKeys(*db, 128, 128, false);

  size_t used = server.ApproxMemoryUsage();
  ASSERT_TRUE(server.ApplyConfig("maxmemory", std::to_string(used / 2)));

  EXPECT_EQ(PerformEvictions(server), EvictionResult::kOk);
  EXPECT_LE(server.ApproxMemoryUsage(), server.GetConfig().maxmemory);
  EXPECT_GT(server.Stats().evicted_keys, 0u);
}

TEST(EvictionTest, VolatileLruOnlyEvictsKeysWithTtl) {
  MiniRedisConfig cfg;
  cfg.maxmemory_policy = "volatile_lru";
  cfg.maxmemory_samples = 5;
  auto& server = ResetServer(cfg);
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);

  for (int i = 0; i < 16; ++i) {
    db->Set("persist:" + std::to_string(i), StringValue(Payload(128)));
    std::string volatile_key = "volatile:" + std::to_string(i);
    db->Set(volatile_key, StringValue(Payload(128)));
    db->SetExpire(volatile_key, 9999999999999LL);
  }
  ASSERT_TRUE(server.ApplyConfig("maxmemory", "1"));

  EXPECT_EQ(PerformEvictions(server), EvictionResult::kNoMemory);
  for (int i = 0; i < 16; ++i) {
    EXPECT_TRUE(db->Exists("persist:" + std::to_string(i)));
  }
  EXPECT_EQ(server.Stats().evicted_keys, 16u);
}

TEST(EvictionTest, VolatileLruDoesNotOvercountWhenNoCandidatesExist) {
  MiniRedisConfig cfg;
  cfg.maxmemory_policy = "volatile_lru";
  cfg.maxmemory_samples = 5;
  auto& server = ResetServer(cfg);
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  FillKeys(*db, 16, 128, false);
  ASSERT_TRUE(server.ApplyConfig("maxmemory", "1"));

  EXPECT_EQ(PerformEvictions(server), EvictionResult::kNoMemory);
  EXPECT_EQ(server.Stats().evicted_keys, 0u);
}

TEST(ExpireTest, ActiveExpireRemovesExpiredKeysAndIncrementsStats) {
  auto& server = ResetServer();
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  FillKeys(*db, 10, 16, true);

  ActiveExpireConfig config;
  size_t expired = ActiveExpireCycle(server, 1000, config);

  EXPECT_EQ(expired, 10u);
  EXPECT_EQ(server.Stats().expired_keys, 10u);
  EXPECT_EQ(db->ExpiresSize(), 0u);
  EXPECT_EQ(db->Size(), 0u);
}

TEST(ExpireTest, ActiveExpireLeavesFutureKeysAlive) {
  auto& server = ResetServer();
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  for (int i = 0; i < 10; ++i) {
    std::string key = "future:" + std::to_string(i);
    db->Set(key, StringValue("value"));
    db->SetExpire(key, 9999999999999LL);
  }

  ActiveExpireConfig config;
  EXPECT_EQ(ActiveExpireCycle(server, 1000, config), 0u);
  EXPECT_EQ(server.Stats().expired_keys, 0u);
  EXPECT_EQ(db->ExpiresSize(), 10u);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(db->Exists("future:" + std::to_string(i)));
  }
}

TEST(ExpireTest, ExpireSomeHonorsSampleCount) {
  Database db;
  FillKeys(db, 10, 16, true);

  auto result = db.ExpireSome(1000, 3, 0);

  EXPECT_LE(result.sampled, 3u);
  EXPECT_EQ(result.expired, result.sampled);
  EXPECT_EQ(db.ExpiresSize(), 10u - result.expired);
}

TEST(ExpireTest, ExpireSomeSamplesOnlyVolatileKeys) {
  Database db;
  for (int i = 0; i < 10; ++i) {
    db.Set("persist:" + std::to_string(i), StringValue("value"));
    std::string volatile_key = "volatile:" + std::to_string(i);
    db.Set(volatile_key, StringValue("value"));
    db.SetExpire(volatile_key, 100);
  }

  auto result = db.ExpireSome(1000, 3, 0);

  EXPECT_EQ(result.sampled, 3u);
  EXPECT_EQ(result.expired, 3u);
  EXPECT_EQ(db.ExpiresSize(), 7u);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(db.Exists("persist:" + std::to_string(i)));
  }
}

TEST(ExpireTest, ActiveExpireEffortIncreasesPerCycleWork) {
  MiniRedisConfig low_effort_cfg;
  low_effort_cfg.active_expire_effort = 1;
  auto& low_effort_server = ResetServer(low_effort_cfg);
  FillKeys(*low_effort_server.GetDb(0), 250, 16, true);
  ActiveExpireConfig config;
  config.keys_per_loop = 1;
  config.slow_time_perc = 100;
  size_t low_effort_expired =
      ActiveExpireCycle(low_effort_server, 1000, config);
  EXPECT_LE(low_effort_expired, 100u);

  MiniRedisConfig high_effort_cfg;
  high_effort_cfg.active_expire_effort = 10;
  auto& high_effort_server = ResetServer(high_effort_cfg);
  FillKeys(*high_effort_server.GetDb(0), 250, 16, true);
  size_t high_effort_expired =
      ActiveExpireCycle(high_effort_server, 1000, config);

  EXPECT_GT(high_effort_expired, low_effort_expired);
}

}  // namespace
}  // namespace miniredis
