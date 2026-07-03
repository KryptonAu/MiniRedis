#include "ds/skiplist.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace miniredis::ds {
namespace {

using IntSkiplist = Skiplist<int, double>;
using StringSkiplist = Skiplist<std::string, double>;

// ===== Construction =====
TEST(SkiplistTest, ConstructEmpty) {
  IntSkiplist sl;
  EXPECT_EQ(sl.Size(), 0);
  EXPECT_EQ(sl.First(), nullptr);
  EXPECT_EQ(sl.Tail(), nullptr);
}

// ===== Insert and basic access =====
TEST(SkiplistTest, InsertSingle) {
  IntSkiplist sl;
  auto* node = sl.Insert(1.0, 42);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 42);
  EXPECT_EQ(node->score, 1.0);
  EXPECT_EQ(sl.Size(), 1);
  EXPECT_EQ(sl.First(), node);
  EXPECT_EQ(sl.Tail(), node);
}

TEST(SkiplistTest, InsertMultipleOrdered) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);

  EXPECT_EQ(sl.Size(), 3);

  // Forward traversal
  auto* node = sl.First();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 10);
  EXPECT_EQ(node->score, 1.0);

  node = node->levels[0].forward;
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 20);
  EXPECT_EQ(node->score, 2.0);

  node = node->levels[0].forward;
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 30);
  EXPECT_EQ(node->score, 3.0);

  EXPECT_EQ(node->levels[0].forward, nullptr);
}

TEST(SkiplistTest, BackwardTraversal) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);

  auto* tail = sl.Tail();
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(tail->key, 20);

  auto* prev = tail->backward;
  ASSERT_NE(prev, nullptr);
  EXPECT_EQ(prev->key, 10);

  EXPECT_EQ(prev->backward, nullptr);
}

// ===== Delete =====
TEST(SkiplistTest, DeleteByScoreAndKey) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);

  EXPECT_TRUE(sl.Delete(2.0, 20));
  EXPECT_EQ(sl.Size(), 2);

  // Verify 20 is gone
  auto* node = sl.First();
  EXPECT_EQ(node->key, 10);
  node = node->levels[0].forward;
  EXPECT_EQ(node->key, 30);
}

TEST(SkiplistTest, DeleteByNodePointer) {
  IntSkiplist sl;
  auto* n = sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);

  EXPECT_TRUE(sl.DeleteNode(n));
  EXPECT_EQ(sl.Size(), 1);
  EXPECT_EQ(sl.First()->key, 20);
}

// ===== Rank =====
TEST(SkiplistTest, GetRank1Based) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);

  auto rank = sl.GetRank(1.0, 10);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 1);

  rank = sl.GetRank(3.0, 30);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 3);
}

TEST(SkiplistTest, GetByRank) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);

  auto* node = sl.GetByRank(1);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 10);

  node = sl.GetByRank(2);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 20);

  node = sl.GetByRank(3);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 30);

  EXPECT_EQ(sl.GetByRank(0), nullptr);
  EXPECT_EQ(sl.GetByRank(4), nullptr);
}

// ===== Same score ordering =====
TEST(SkiplistTest, SameScoreOrdersByKey) {
  IntSkiplist sl;
  sl.Insert(1.0, 30);
  sl.Insert(1.0, 10);
  sl.Insert(1.0, 20);

  EXPECT_EQ(sl.Size(), 3);

  auto* node = sl.First();
  EXPECT_EQ(node->key, 10);
  node = node->levels[0].forward;
  EXPECT_EQ(node->key, 20);
  node = node->levels[0].forward;
  EXPECT_EQ(node->key, 30);
}

TEST(SkiplistTest, StringViewOperationsUseSubviewContents) {
  StringSkiplist sl;
  std::string source = "xxalphayy";
  std::string_view alpha(source.data() + 2, 5);

  auto* alpha_node = sl.InsertView(1.0, alpha);
  ASSERT_NE(alpha_node, nullptr);
  EXPECT_EQ(alpha_node->key, "alpha");
  source[2] = 'X';
  EXPECT_EQ(alpha_node->key, "alpha");

  sl.InsertView(1.0, "gamma");
  std::string beta_source = "__beta__";
  std::string_view beta(beta_source.data() + 2, 4);
  sl.InsertView(1.0, beta);

  std::string lookup_source = "zzalphazz";
  std::string_view alpha_lookup(lookup_source.data() + 2, 5);
  auto rank = sl.GetRankView(1.0, alpha_lookup);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 1);

  auto* node = sl.First();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, "alpha");
  node = node->levels[0].forward;
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, "beta");
  node = node->levels[0].forward;
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, "gamma");

  std::string delete_source = "qqbetaqq";
  std::string_view beta_lookup(delete_source.data() + 2, 4);
  EXPECT_TRUE(sl.DeleteView(1.0, beta_lookup));
  EXPECT_EQ(sl.Size(), 2);
  EXPECT_FALSE(sl.GetRankView(1.0, beta_lookup).has_value());
  ASSERT_NE(sl.First(), nullptr);
  EXPECT_EQ(sl.First()->key, "alpha");
  ASSERT_NE(sl.First()->levels[0].forward, nullptr);
  EXPECT_EQ(sl.First()->levels[0].forward->key, "gamma");
}

// ===== Range queries =====
TEST(SkiplistTest, RangeSpecScoreInRange) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(5.0, 20);
  sl.Insert(10.0, 30);

  typename IntSkiplist::RangeSpec range{1.0, 10.0, false, false};
  EXPECT_TRUE(sl.ScoreInRange(range));

  typename IntSkiplist::RangeSpec out_of_range{100.0, 200.0, false, false};
  EXPECT_FALSE(sl.ScoreInRange(out_of_range));
}

TEST(SkiplistTest, FirstAndLastInRange) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(3.0, 20);
  sl.Insert(5.0, 30);
  sl.Insert(7.0, 40);

  typename IntSkiplist::RangeSpec range{2.0, 6.0, true, true};
  // Exclusive on both sides: should find 3.0 and 5.0

  auto* first = sl.FirstInRange(range);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->score, 3.0);

  auto* last = sl.LastInRange(range);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->score, 5.0);
}

// ===== Delete range =====
TEST(SkiplistTest, DeleteRangeByScore) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);
  sl.Insert(4.0, 40);

  typename IntSkiplist::RangeSpec range{2.0, 3.0, false, false};
  size_t deleted = sl.DeleteRangeByScore(range, [](int, double) {});
  EXPECT_EQ(deleted, 2);
  EXPECT_EQ(sl.Size(), 2);
  EXPECT_EQ(sl.First()->key, 10);

  auto* n = sl.First()->levels[0].forward;
  EXPECT_EQ(n->key, 40);
}

TEST(SkiplistTest, DeleteRangeByRank) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);
  sl.Insert(4.0, 40);

  size_t deleted = sl.DeleteRangeByRank(2, 3, [](int, double) {});
  EXPECT_EQ(deleted, 2);
  EXPECT_EQ(sl.Size(), 2);
  EXPECT_EQ(sl.First()->key, 10);
  EXPECT_EQ(sl.First()->levels[0].forward->key, 40);
}

// ===== Random level =====
TEST(SkiplistTest, RandomLevelInRange) {
  for (int i = 0; i < 1000; i++) {
    int level = IntSkiplist::RandomLevel();
    EXPECT_GE(level, 1);
    EXPECT_LE(level, 32);
  }
}

// ===== Large data =====
TEST(SkiplistTest, LargeNumberOfEntries) {
  IntSkiplist sl;
  for (int i = 0; i < 10000; i++) {
    sl.Insert(static_cast<double>(i), i);
  }
  EXPECT_EQ(sl.Size(), 10000);

  auto* node = sl.GetByRank(1);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 0);

  node = sl.GetByRank(10000);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->key, 9999);
}

}  // namespace
}  // namespace miniredis::ds
