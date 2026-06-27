#include "core/config.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(ConfigTest, Defaults) {
  ConfigManager cm;
  EXPECT_EQ(cm.Config().port, 6379);
  EXPECT_EQ(cm.Config().databases, 16);
}

TEST(ConfigTest, SetAndGet) {
  ConfigManager cm;
  EXPECT_TRUE(cm.Set("port", "8080"));
  EXPECT_EQ(cm.Get("port").value(), "8080");
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

}  // namespace
}  // namespace miniredis
