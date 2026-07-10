#include "types/zset_value.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <string_view>

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
  EncodingThresholds t;
  t.zset_max_listpack_entries = 1;
  ZSetValue zs(t);
  zs.Add("seed", 0.0);
  zs.Add("b", 2.0);
  ASSERT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);

  zs.Add("a", 1.0);
  zs.Add("b", 3.0);
  EXPECT_TRUE(zs.Remove("seed"));
  EXPECT_EQ(zs.Count(), 2);
  EXPECT_FALSE(zs.Score("seed").has_value());
  EXPECT_DOUBLE_EQ(zs.Score("b").value(), 3.0);
  EXPECT_EQ(zs.Rank("a").value(), 0);
  EXPECT_EQ(zs.Rank("b").value(), 1);

  ZSetValue zs2(std::move(zs));
  EXPECT_EQ(zs2.Count(), 2);
  EXPECT_DOUBLE_EQ(zs2.Score("a").value(), 1.0);
  EXPECT_EQ(zs2.Rank("b").value(), 1);

  EXPECT_TRUE(zs2.Remove("a"));
  EXPECT_EQ(zs2.Count(), 1);
  EXPECT_FALSE(zs2.Score("a").has_value());
  EXPECT_DOUBLE_EQ(zs2.Score("b").value(), 3.0);
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

TEST(ZSetValueTest, ListpackEncodingAcceptsSubviewElements) {
  ZSetValue zs;
  std::string insert_source = "__alpha__";
  std::string_view alpha(insert_source.data() + 2, 5);

  auto added = zs.Add(alpha, 1.5);
  ASSERT_TRUE(std::holds_alternative<bool>(added));
  EXPECT_TRUE(std::get<bool>(added));
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kZSetListpack);
  insert_source[2] = 'X';

  std::string lookup_source = "zzalphazz";
  std::string_view alpha_lookup(lookup_source.data() + 2, 5);
  ASSERT_TRUE(zs.Score(alpha_lookup).has_value());
  EXPECT_DOUBLE_EQ(zs.Score(alpha_lookup).value(), 1.5);
  ASSERT_TRUE(zs.Rank(alpha_lookup).has_value());
  EXPECT_EQ(zs.Rank(alpha_lookup).value(), 0);

  auto updated = zs.Update(alpha_lookup, 0.5);
  ASSERT_TRUE(std::holds_alternative<double>(updated));
  EXPECT_DOUBLE_EQ(std::get<double>(updated), 2.0);
  EXPECT_DOUBLE_EQ(zs.Score(alpha_lookup).value(), 2.0);

  std::string remove_source = "qqalphaqq";
  std::string_view alpha_remove(remove_source.data() + 2, 5);
  EXPECT_TRUE(zs.Remove(alpha_remove));
  EXPECT_FALSE(zs.Score(alpha_lookup).has_value());
}

TEST(ZSetValueTest, ListpackEncodingFindsIntegerEncodedMembers) {
  ZSetValue zs;
  zs.Add("1", 1.0);
  zs.Add("001", 2.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kZSetListpack);
  EXPECT_EQ(zs.Count(), 2);

  ASSERT_TRUE(zs.Score("1").has_value());
  EXPECT_DOUBLE_EQ(zs.Score("1").value(), 1.0);
  ASSERT_TRUE(zs.Score("001").has_value());
  EXPECT_DOUBLE_EQ(zs.Score("001").value(), 2.0);

  auto updated = zs.Update("1", 2.0);
  ASSERT_TRUE(std::holds_alternative<double>(updated));
  EXPECT_DOUBLE_EQ(std::get<double>(updated), 3.0);
  EXPECT_DOUBLE_EQ(zs.Score("1").value(), 3.0);
  EXPECT_DOUBLE_EQ(zs.Score("001").value(), 2.0);

  EXPECT_TRUE(zs.Remove("1"));
  EXPECT_FALSE(zs.Score("1").has_value());
  EXPECT_TRUE(zs.Score("001").has_value());
}

TEST(ZSetValueTest, ListpackLexOperationsCompareEncodedMembersInPlace) {
  ZSetValue zs;
  zs.Add("1", 1.0);
  zs.Add("2", 1.0);
  zs.Add("a", 1.0);
  zs.Add("b", 1.0);
  EXPECT_EQ(zs.Encoding(), ValueEncoding::kZSetListpack);

  EXPECT_EQ(zs.LexCount("1", "a", false, false), 3);
  auto range = zs.RangeByLex("1", "a", false, false);
  ASSERT_EQ(range.size(), 3);
  EXPECT_EQ(range[0].element, "1");
  EXPECT_EQ(range[1].element, "2");
  EXPECT_EQ(range[2].element, "a");

  EXPECT_EQ(zs.RemoveRangeByLex("2", "a", false, false), 2);
  EXPECT_EQ(zs.Count(), 2);
  EXPECT_TRUE(zs.Score("1").has_value());
  EXPECT_TRUE(zs.Score("b").has_value());
  EXPECT_FALSE(zs.Score("2").has_value());
  EXPECT_FALSE(zs.Score("a").has_value());
}

TEST(ZSetValueTest, SkiplistEncodingAcceptsSubviewElements) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 1;
  ZSetValue zs(t);
  zs.Add("seed", 0.0);
  zs.Add("omega", 10.0);
  ASSERT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);

  std::string insert_source = "__alpha__";
  std::string_view alpha(insert_source.data() + 2, 5);
  auto added = zs.Add(alpha, 1.5);
  ASSERT_TRUE(std::holds_alternative<bool>(added));
  EXPECT_TRUE(std::get<bool>(added));
  insert_source[2] = 'X';

  std::string lookup_source = "zzalphazz";
  std::string_view alpha_lookup(lookup_source.data() + 2, 5);
  ASSERT_TRUE(zs.Score(alpha_lookup).has_value());
  EXPECT_DOUBLE_EQ(zs.Score(alpha_lookup).value(), 1.5);
  ASSERT_TRUE(zs.Rank(alpha_lookup).has_value());
  EXPECT_EQ(zs.Rank(alpha_lookup).value(), 1);

  auto updated = zs.Update(alpha_lookup, 0.5);
  ASSERT_TRUE(std::holds_alternative<double>(updated));
  EXPECT_DOUBLE_EQ(std::get<double>(updated), 2.0);
  EXPECT_DOUBLE_EQ(zs.Score(alpha_lookup).value(), 2.0);

  std::string remove_source = "qqalphaqq";
  std::string_view alpha_remove(remove_source.data() + 2, 5);
  EXPECT_TRUE(zs.Remove(alpha_remove));
  EXPECT_FALSE(zs.Score(alpha_lookup).has_value());
}

TEST(ZSetValueTest, ListpackToSkiplistConversionAcceptsSubviewElements) {
  EncodingThresholds t;
  t.zset_max_listpack_entries = 2;
  ZSetValue zs(t);

  std::string insert_source = "__alpha__";
  std::string_view alpha(insert_source.data() + 2, 5);
  ASSERT_TRUE(std::holds_alternative<bool>(zs.Add(alpha, 1.5)));
  insert_source[2] = 'X';
  zs.Add("beta", 0.5);
  zs.Add("gamma", 2.5);
  ASSERT_EQ(zs.Encoding(), ValueEncoding::kSkiplist);

  std::string lookup_source = "zzalphazz";
  std::string_view alpha_lookup(lookup_source.data() + 2, 5);
  ASSERT_TRUE(zs.Score(alpha_lookup).has_value());
  EXPECT_DOUBLE_EQ(zs.Score(alpha_lookup).value(), 1.5);
  ASSERT_TRUE(zs.Rank(alpha_lookup).has_value());
  EXPECT_EQ(zs.Rank(alpha_lookup).value(), 1);

  std::string remove_source = "qqalphaqq";
  std::string_view alpha_remove(remove_source.data() + 2, 5);
  EXPECT_TRUE(zs.Remove(alpha_remove));
  EXPECT_FALSE(zs.Score(alpha_lookup).has_value());
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
