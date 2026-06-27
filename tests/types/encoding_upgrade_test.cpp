#include <gtest/gtest.h>

#include "types/hash_value.h"
#include "types/set_value.h"
#include "types/zset_value.h"

namespace miniredis {
namespace {

// ===== Set upgrades =====
TEST(EncodingUpgradeTest, SetUpgradeOnEntryThreshold) {
  EncodingThresholds t;
  t.set_max_intset_entries = 3;
  SetValue sv(t);
  for (int i = 0; i < 4; i++) sv.Add(std::to_string(i));
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kHashtable);
  EXPECT_EQ(sv.Size(), 4);
}

TEST(EncodingUpgradeTest, SetUpgradeOnNonCanonical) {
  EncodingThresholds t;
  t.set_max_intset_entries = 512;
  SetValue sv(t);
  sv.Add("1");
  sv.Add("2");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntset);
  sv.Add("001");  // non-canonical forces upgrade
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kHashtable);
  EXPECT_TRUE(sv.Contains("001"));
  EXPECT_TRUE(sv.Contains("1"));
}

// ===== Hash upgrades =====
TEST(EncodingUpgradeTest, HashUpgradeOnEntryThreshold) {
  EncodingThresholds t;
  t.hash_max_listpack_entries = 3;
  HashValue hv(t);
  for (int i = 0; i < 4; i++) hv.Set(std::to_string(i), "v");
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kHashHT);
  EXPECT_EQ(hv.Size(), 4);
}

TEST(EncodingUpgradeTest, HashNoUpgradeOnUpdateAtThreshold) {
  EncodingThresholds t;
  t.hash_max_listpack_entries = 3;
  HashValue hv(t);
  hv.Set("a", "1");
  hv.Set("b", "2");
  hv.Set("c", "3");  // 3 fields, at threshold
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kListpack);
  // Update existing field at threshold — must NOT upgrade
  hv.Set("a", "x");
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kListpack);
  EXPECT_EQ(hv.Size(), 3);
}

TEST(EncodingUpgradeTest, HashUpgradeOnValueLengthThreshold) {
  EncodingThresholds t;
  t.hash_max_listpack_value = 64;
  HashValue hv(t);
  hv.Set("short", "value");
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kListpack);
  std::string long_value(65, 'x');
  hv.Set("long", long_value);
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kHashHT);
}

// ===== ZSet upgrades =====
TEST(EncodingUpgradeTest, ZSetUpgradeOnEntryThreshold) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 3;
  ZSetValue zs(t);
  for (int i = 0; i < 4; i++) zs.Add(std::to_string(i), static_cast<double>(i));
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);
  EXPECT_EQ(zs.Count(), 4);
}

TEST(EncodingUpgradeTest, ZSetUpgradeOnValueLengthThreshold) {
  EncodingThresholds t;
  t.zset_max_listpack_value = 10;
  ZSetValue zs(t);
  zs.Add("short", 1.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kZSetListpack);
  zs.Add("very_long_member_name", 2.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);
}

TEST(EncodingUpgradeTest, ZSetOrderPreservedAfterUpgrade) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 2;
  ZSetValue zs(t);
  zs.Add("c", 3.0);
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);
  auto r = zs.Range(0, -1);
  ASSERT_EQ(r.size(), 3);
  EXPECT_EQ(r[0].element, "a");
  EXPECT_EQ(r[1].element, "b");
  EXPECT_EQ(r[2].element, "c");
}

}  // namespace
}  // namespace miniredis
