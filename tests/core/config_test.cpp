#include "core/config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace miniredis {
namespace {

TEST(ConfigTest, Defaults) {
  ConfigManager cm;
  EXPECT_EQ(cm.Config().port, 6379);
  EXPECT_EQ(cm.Config().databases, 16);
  EXPECT_EQ(cm.Config().command_queue_capacity, 65536u);
}

TEST(ConfigTest, SetAndGet) {
  ConfigManager cm;
  EXPECT_TRUE(cm.Set("port", "8080"));
  EXPECT_EQ(cm.Get("port").value(), "8080");
}

TEST(ConfigTest, LoadFromFile) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("miniredis-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".conf");
  {
    std::ofstream file(path);
    ASSERT_TRUE(file.is_open());
    file << "# MiniRedis configuration\n"
         << "bind 0.0.0.0\n"
         << "port 6380\n"
         << "databases 2\n"
         << "command-queue-capacity 1024\n"
         << "save-enabled no\n"
         << "appendonly yes\n"
         << "maxmemory-policy allkeys-lru\n";
  }

  ConfigManager cm;
  EXPECT_TRUE(cm.LoadFromFile(path.string()));
  EXPECT_EQ(cm.Config().bind, "0.0.0.0");
  EXPECT_EQ(cm.Config().port, 6380);
  EXPECT_EQ(cm.Config().databases, 2);
  EXPECT_EQ(cm.Config().command_queue_capacity, 1024u);
  EXPECT_FALSE(cm.Config().save_enabled);
  EXPECT_TRUE(cm.Config().appendonly);
  EXPECT_EQ(cm.Config().maxmemory_policy, "allkeys_lru");

  EXPECT_TRUE(std::filesystem::remove(path));
}

TEST(ConfigTest, EncodingThresholds) {
  ConfigManager cm;
  EXPECT_TRUE(cm.Set("set_max_intset_entries", "256"));
  EXPECT_TRUE(cm.Set("hash_max_listpack_entries", "300"));
  EXPECT_TRUE(cm.Set("hash_max_listpack_value", "32"));
  EXPECT_TRUE(cm.Set("zset_max_listpack_entries", "64"));
  EXPECT_TRUE(cm.Set("zset_max_listpack_value", "48"));

  EXPECT_EQ(cm.Get("set_max_intset_entries").value(), "256");
  EXPECT_EQ(cm.Get("hash_max_listpack_entries").value(), "300");
  EXPECT_EQ(cm.Get("hash_max_listpack_value").value(), "32");
  EXPECT_EQ(cm.Get("zset_max_listpack_entries").value(), "64");
  EXPECT_EQ(cm.Get("zset_max_listpack_value").value(), "48");

  auto t = cm.GetEncodingThresholds();
  EXPECT_EQ(t.set_max_intset_entries, 256);
  EXPECT_EQ(t.hash_max_listpack_entries, 300);
  EXPECT_EQ(t.hash_max_listpack_value, 32);
  EXPECT_EQ(t.zset_max_listpack_entries, 64);
  EXPECT_EQ(t.zset_max_listpack_value, 48);
}

TEST(ConfigTest, CommandQueueCapacity) {
  ConfigManager cm;

  EXPECT_TRUE(cm.Set("command_queue_capacity", "1024"));
  EXPECT_EQ(cm.Config().command_queue_capacity, 1024u);
  EXPECT_EQ(cm.Get("command_queue_capacity").value(), "1024");
  EXPECT_EQ(cm.Get("command-queue-capacity").value(), "1024");

  EXPECT_TRUE(cm.Set("command-queue-capacity", "2048"));
  EXPECT_EQ(cm.Config().command_queue_capacity, 2048u);
  EXPECT_EQ(cm.Get("command_queue_capacity").value(), "2048");

  EXPECT_FALSE(cm.Set("command_queue_capacity", "0"));
  EXPECT_EQ(cm.Config().command_queue_capacity, 2048u);
}

}  // namespace
}  // namespace miniredis
