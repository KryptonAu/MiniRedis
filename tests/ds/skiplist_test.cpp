#include "ds/skiplist.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

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
  EXPECT_EQ(node->member, 42);
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
  EXPECT_EQ(node->member, 10);
  EXPECT_EQ(node->score, 1.0);

  node = node->Next();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 20);
  EXPECT_EQ(node->score, 2.0);

  node = node->Next();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 30);
  EXPECT_EQ(node->score, 3.0);

  EXPECT_EQ(node->Next(), nullptr);
}

TEST(SkiplistTest, BackwardTraversal) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);

  auto* tail = sl.Tail();
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(tail->member, 20);

  auto* prev = tail->backward;
  ASSERT_NE(prev, nullptr);
  EXPECT_EQ(prev->member, 10);

  EXPECT_EQ(prev->backward, nullptr);
}

// ===== Delete =====
TEST(SkiplistTest, DeleteByScoreAndMember) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  sl.Insert(3.0, 30);

  EXPECT_TRUE(sl.Delete(2.0, 20));
  EXPECT_EQ(sl.Size(), 2);

  // Verify 20 is gone
  auto* node = sl.First();
  EXPECT_EQ(node->member, 10);
  node = node->Next();
  EXPECT_EQ(node->member, 30);
}

TEST(SkiplistTest, DeleteByNodePointer) {
  IntSkiplist sl;
  auto* n = sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);

  EXPECT_TRUE(sl.DeleteNode(n));
  EXPECT_EQ(sl.Size(), 1);
  EXPECT_EQ(sl.First()->member, 20);
}

TEST(SkiplistTest, DeleteByScoreAndMemberMaintainsRanksInLargeList) {
  IntSkiplist sl;
  for (int i = 1; i <= 2048; i++) {
    sl.Insert(static_cast<double>(i), i);
  }

  EXPECT_TRUE(sl.Delete(1024.0, 1024));
  EXPECT_EQ(sl.Size(), 2047);
  EXPECT_FALSE(sl.GetRank(1024.0, 1024).has_value());

  auto rank = sl.GetRank(1025.0, 1025);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 1024);

  auto* node = sl.GetByRank(1024);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 1025);
  ASSERT_NE(sl.Tail(), nullptr);
  EXPECT_EQ(sl.Tail()->member, 2048);
}

TEST(SkiplistTest, DeleteByNodePointerKeepsLevelZeroOwnershipChain) {
  IntSkiplist sl;
  sl.Insert(1.0, 10);
  sl.Insert(2.0, 20);
  auto* middle = sl.Insert(3.0, 30);
  sl.Insert(4.0, 40);
  sl.Insert(5.0, 50);

  EXPECT_TRUE(sl.DeleteNode(middle));
  EXPECT_EQ(sl.Size(), 4);

  std::vector<int> members;
  for (auto* node = sl.First(); node; node = node->Next()) {
    members.push_back(node->member);
  }
  EXPECT_EQ(members, (std::vector<int>{10, 20, 40, 50}));
  ASSERT_NE(sl.Tail(), nullptr);
  EXPECT_EQ(sl.Tail()->member, 50);

  auto rank = sl.GetRank(4.0, 40);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 3);
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
  EXPECT_EQ(node->member, 10);

  node = sl.GetByRank(2);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 20);

  node = sl.GetByRank(3);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 30);

  EXPECT_EQ(sl.GetByRank(0), nullptr);
  EXPECT_EQ(sl.GetByRank(4), nullptr);
}

// ===== Same score ordering =====
TEST(SkiplistTest, SameScoreOrdersByMember) {
  IntSkiplist sl;
  sl.Insert(1.0, 30);
  sl.Insert(1.0, 10);
  sl.Insert(1.0, 20);

  EXPECT_EQ(sl.Size(), 3);

  auto* node = sl.First();
  EXPECT_EQ(node->member, 10);
  node = node->Next();
  EXPECT_EQ(node->member, 20);
  node = node->Next();
  EXPECT_EQ(node->member, 30);
}

TEST(SkiplistTest, StringViewOperationsUseSubviewContents) {
  StringSkiplist sl;
  std::string source = "xxalphayy";
  std::string_view alpha(source.data() + 2, 5);

  auto* alpha_node = sl.InsertView(1.0, alpha);
  ASSERT_NE(alpha_node, nullptr);
  EXPECT_EQ(alpha_node->member, "alpha");
  source[2] = 'X';
  EXPECT_EQ(alpha_node->member, "alpha");

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
  EXPECT_EQ(node->member, "alpha");
  node = node->Next();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, "beta");
  node = node->Next();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, "gamma");

  std::string delete_source = "qqbetaqq";
  std::string_view beta_lookup(delete_source.data() + 2, 4);
  EXPECT_TRUE(sl.DeleteView(1.0, beta_lookup));
  EXPECT_EQ(sl.Size(), 2);
  EXPECT_FALSE(sl.GetRankView(1.0, beta_lookup).has_value());
  ASSERT_NE(sl.First(), nullptr);
  EXPECT_EQ(sl.First()->member, "alpha");
  ASSERT_NE(sl.First()->Next(), nullptr);
  EXPECT_EQ(sl.First()->Next()->member, "gamma");
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
  EXPECT_EQ(sl.First()->member, 10);

  auto* n = sl.First()->Next();
  EXPECT_EQ(n->member, 40);
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
  EXPECT_EQ(sl.First()->member, 10);
  EXPECT_EQ(sl.First()->Next()->member, 40);
}

TEST(SkiplistTest, DeleteRangeByRankPreservesCallbacksTailAndRanks) {
  IntSkiplist sl;
  for (int i = 1; i <= 6; i++) {
    sl.Insert(static_cast<double>(i), i * 10);
  }

  std::vector<int> deleted_members;
  size_t deleted = sl.DeleteRangeByRank(
      2, 5, [&](int member, double) { deleted_members.push_back(member); });

  EXPECT_EQ(deleted, 4);
  EXPECT_EQ(deleted_members, (std::vector<int>{20, 30, 40, 50}));
  EXPECT_EQ(sl.Size(), 2);
  ASSERT_NE(sl.First(), nullptr);
  EXPECT_EQ(sl.First()->member, 10);
  ASSERT_NE(sl.Tail(), nullptr);
  EXPECT_EQ(sl.Tail()->member, 60);
  EXPECT_EQ(sl.First()->Next(), sl.Tail());
  EXPECT_EQ(sl.Tail()->Next(), nullptr);

  auto rank = sl.GetRank(6.0, 60);
  ASSERT_TRUE(rank.has_value());
  EXPECT_EQ(*rank, 2);
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
  EXPECT_EQ(node->member, 0);

  node = sl.GetByRank(10000);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->member, 9999);
}

}  // namespace
}  // namespace miniredis::ds
