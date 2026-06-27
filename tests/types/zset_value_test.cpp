#include "types/zset_value.h"

#include <gtest/gtest.h>

#include <cmath>

namespace miniredis {
namespace {

TEST(ZSetValueTest, ConstructEmpty) {
  ZSetValue zs;
  EXPECT_EQ(zs.Count(), 0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kZSetListpack);
}

TEST(ZSetValueTest, AddAndScore) {
  ZSetValue zs;
  auto r = zs.Add("a", 1.0);
  ASSERT_TRUE(std::holds_alternative<bool>(r));
  EXPECT_TRUE(std::get<bool>(r));
  EXPECT_EQ(zs.Count(), 1);
  EXPECT_EQ(zs.Score("a").value(), 1.0);
}

TEST(ZSetValueTest, AddDuplicateSameScore) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  auto r = zs.Add("a", 1.0);
  ASSERT_TRUE(std::holds_alternative<bool>(r));
  EXPECT_FALSE(std::get<bool>(r));
}

TEST(ZSetValueTest, AddDuplicateUpdate) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  auto r = zs.Add("a", 2.0);
  ASSERT_TRUE(std::holds_alternative<bool>(r));
  EXPECT_FALSE(std::get<bool>(r));  // element existed
  EXPECT_EQ(zs.Score("a").value(), 2.0);
}

TEST(ZSetValueTest, Rank) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  EXPECT_EQ(zs.Rank("a").value(), 0);
  EXPECT_EQ(zs.Rank("b").value(), 1);
  EXPECT_EQ(zs.Rank("c").value(), 2);
}

TEST(ZSetValueTest, RevRank) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  EXPECT_EQ(zs.RevRank("a").value(), 1);
  EXPECT_EQ(zs.RevRank("b").value(), 0);
}

TEST(ZSetValueTest, Remove) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  EXPECT_TRUE(zs.Remove("a"));
  EXPECT_EQ(zs.Count(), 1);
  EXPECT_FALSE(zs.Score("a").has_value());
}

TEST(ZSetValueTest, Update) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  auto r = zs.Update("a", 0.5);
  ASSERT_TRUE(std::holds_alternative<double>(r));
  EXPECT_DOUBLE_EQ(std::get<double>(r), 1.5);
  EXPECT_DOUBLE_EQ(zs.Score("a").value(), 1.5);
}

TEST(ZSetValueTest, UpdateNewElement) {
  ZSetValue zs;
  auto r = zs.Update("new", 5.0);
  ASSERT_TRUE(std::holds_alternative<double>(r));
  EXPECT_DOUBLE_EQ(std::get<double>(r), 5.0);
  EXPECT_EQ(zs.Count(), 1);
}

TEST(ZSetValueTest, Range) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  auto result = zs.Range(0, 1);
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].element, "a");
  EXPECT_EQ(result[1].element, "b");
}

TEST(ZSetValueTest, PopMin) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  auto min = zs.PopMin();
  ASSERT_TRUE(min.has_value());
  EXPECT_EQ(min->element, "a");
  EXPECT_EQ(zs.Count(), 1);
}

TEST(ZSetValueTest, InvalidScore) {
  ZSetValue zs;
  auto r = zs.Add("x", NAN);
  EXPECT_TRUE(std::holds_alternative<TypeError>(r));
  EXPECT_EQ(std::get<TypeError>(r), TypeError::kInvalidScore);
}

TEST(ZSetValueTest, UpgradeToSkiplist) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 3;
  ZSetValue zs(t);
  for (int i = 0; i < 5; i++) {
    zs.Add(std::to_string(i), static_cast<double>(i));
  }
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);
  EXPECT_EQ(zs.Count(), 5);
  EXPECT_EQ(zs.Rank("4").value(), 4);
}

TEST(ZSetValueTest, SkiplistDictConsistencyAfterMove) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  ZSetValue zs2(std::move(zs));
  EXPECT_EQ(zs2.Count(), 2);
  EXPECT_DOUBLE_EQ(zs2.Score("a").value(), 1.0);
  EXPECT_EQ(zs2.Rank("b").value(), 1);
  EXPECT_TRUE(zs2.Remove("a"));
  EXPECT_EQ(zs2.Count(), 1);
}

// Regression: RevRange(0,0) must return MAX, not min
TEST(ZSetValueTest, RevRangeReturnsMax) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 3.0);
  zs.Add("c", 2.0);
  auto result = zs.RevRange(0, 0);
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].element, "b");  // highest score
}

// Regression: update existing member's score must maintain ordering
TEST(ZSetValueTest, AddUpdateMaintainsOrder) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 3.0);
  // Update b from 3.0 to 0.5 — should move before a
  zs.Add("b", 0.5);
  auto r = zs.Range(0, 1);
  ASSERT_EQ(r.size(), 2);
  EXPECT_EQ(r[0].element, "b");  // score 0.5
  EXPECT_EQ(r[1].element, "a");  // score 1.0
}

// CountByScore / RangeByScore with skiplist encoding
TEST(ZSetValueTest, CountByScoreWithSkiplist) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 2;
  ZSetValue zs(t);
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  zs.Add("d", 4.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);
  EXPECT_EQ(zs.CountByScore(2.0, 4.0, false, false), 3);  // b,c,d
  EXPECT_EQ(zs.CountByScore(2.0, 4.0, true, false), 2);   // c,d
}

TEST(ZSetValueTest, RangeByScoreWithSkiplist) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 2;
  ZSetValue zs(t);
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  zs.Add("d", 4.0);
  auto r = zs.RangeByScore(2.0, 3.0, false, false, 0, -1);
  ASSERT_EQ(r.size(), 2);
  EXPECT_EQ(r[0].element, "b");
  EXPECT_EQ(r[1].element, "c");
}

// RevRange(1, -1) should skip max element
TEST(ZSetValueTest, RevRangeSkipFirst) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  auto r = zs.RevRange(1, -1);
  ASSERT_EQ(r.size(), 2);
  EXPECT_EQ(r[0].element, "b");  // second highest
  EXPECT_EQ(r[1].element, "a");  // lowest
}

// RevRange with negative stop
TEST(ZSetValueTest, RevRangeNegativeStop) {
  ZSetValue zs;
  zs.Add("a", 1.0);
  zs.Add("b", 2.0);
  zs.Add("c", 3.0);
  auto r = zs.RevRange(0, -2);
  ASSERT_EQ(r.size(), 2);
  EXPECT_EQ(r[0].element, "c");  // highest
  EXPECT_EQ(r[1].element, "b");  // second highest
}

}  // namespace
}  // namespace miniredis
