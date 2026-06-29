#include "ds/quicklist.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace miniredis::ds {
namespace {

// ===== Construction =====
TEST(QuicklistTest, ConstructEmpty) {
  Quicklist ql;
  EXPECT_EQ(ql.Size(), 0);
  EXPECT_EQ(ql.NodeCount(), 0);
  EXPECT_TRUE(ql.Empty());
}

// ===== Push/Pop =====
TEST(QuicklistTest, PushHeadAndPopHead) {
  Quicklist ql;
  ql.PushHead(std::string_view("hello"));
  EXPECT_EQ(ql.Size(), 1);
  EXPECT_FALSE(ql.Empty());

  auto val = ql.PopHead();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "hello");
  EXPECT_EQ(ql.Size(), 0);
  EXPECT_TRUE(ql.Empty());
}

TEST(QuicklistTest, PushTailAndPopTail) {
  Quicklist ql;
  ql.PushTail(std::string_view("world"));
  EXPECT_EQ(ql.Size(), 1);

  auto val = ql.PopTail();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "world");
}

TEST(QuicklistTest, MultiplePushHead) {
  Quicklist ql;
  ql.PushHead(std::string_view("c"));
  ql.PushHead(std::string_view("b"));
  ql.PushHead(std::string_view("a"));

  EXPECT_EQ(ql.Size(), 3);
  EXPECT_EQ(*ql.PopHead(), "a");
  EXPECT_EQ(*ql.PopHead(), "b");
  EXPECT_EQ(*ql.PopHead(), "c");
}

TEST(QuicklistTest, MultiplePushTail) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.PushTail(std::string_view("b"));
  ql.PushTail(std::string_view("c"));

  EXPECT_EQ(ql.Size(), 3);
  EXPECT_EQ(*ql.PopHead(), "a");
  EXPECT_EQ(*ql.PopHead(), "b");
  EXPECT_EQ(*ql.PopHead(), "c");
}

TEST(QuicklistTest, IntegerPushPop) {
  Quicklist ql;
  ql.PushTail(42);
  ql.PushTail(-100);

  EXPECT_EQ(ql.Size(), 2);
  // Pop returns RESP string semantics
  EXPECT_EQ(*ql.PopHead(), "42");
  EXPECT_EQ(*ql.PopHead(), "-100");
}

TEST(QuicklistTest, PopEmpty) {
  Quicklist ql;
  EXPECT_FALSE(ql.PopHead().has_value());
  EXPECT_FALSE(ql.PopTail().has_value());
}

// ===== Index access =====
TEST(QuicklistTest, GetByIndex) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.PushTail(std::string_view("b"));
  ql.PushTail(std::string_view("c"));

  EXPECT_EQ(ql.Get(0).value(), "a");
  EXPECT_EQ(ql.Get(1).value(), "b");
  EXPECT_EQ(ql.Get(2).value(), "c");
  EXPECT_FALSE(ql.Get(3).has_value());
}

TEST(QuicklistTest, SetByIndex) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.PushTail(std::string_view("b"));

  EXPECT_TRUE(ql.Set(0, std::string_view("x")));
  EXPECT_EQ(ql.Get(0).value(), "x");
  EXPECT_EQ(ql.Get(1).value(), "b");

  EXPECT_FALSE(ql.Set(5, std::string_view("y")));  // out of bounds
}

TEST(QuicklistTest, SetIntegerByIndex) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.Set(0, 42);

  auto val = ql.GetValue(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val->type, Listpack::Value::Type::kInteger);
  EXPECT_EQ(val->ToString(), "42");
}

// ===== Insert =====
TEST(QuicklistTest, InsertBefore) {
  Quicklist ql;
  ql.PushTail(std::string_view("b"));
  ql.PushTail(std::string_view("d"));

  EXPECT_TRUE(ql.InsertBefore(1, std::string_view("c")));
  EXPECT_TRUE(ql.InsertBefore(0, std::string_view("a")));

  EXPECT_EQ(ql.Size(), 4);
  EXPECT_EQ(ql.Get(0).value(), "a");
  EXPECT_EQ(ql.Get(1).value(), "b");
  EXPECT_EQ(ql.Get(2).value(), "c");
  EXPECT_EQ(ql.Get(3).value(), "d");
}

TEST(QuicklistTest, InsertAfter) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.PushTail(std::string_view("c"));

  EXPECT_TRUE(ql.InsertAfter(0, std::string_view("b")));
  EXPECT_TRUE(ql.InsertAfter(2, std::string_view("d")));

  EXPECT_EQ(ql.Size(), 4);
  EXPECT_EQ(ql.Get(0).value(), "a");
  EXPECT_EQ(ql.Get(1).value(), "b");
  EXPECT_EQ(ql.Get(2).value(), "c");
  EXPECT_EQ(ql.Get(3).value(), "d");
}

// ===== Delete =====
TEST(QuicklistTest, DeleteSingleElement) {
  Quicklist ql;
  ql.PushTail(std::string_view("a"));
  ql.PushTail(std::string_view("b"));
  ql.PushTail(std::string_view("c"));

  EXPECT_TRUE(ql.Delete(1));
  EXPECT_EQ(ql.Size(), 2);
  EXPECT_EQ(ql.Get(0).value(), "a");
  EXPECT_EQ(ql.Get(1).value(), "c");
}

TEST(QuicklistTest, DeleteRange) {
  Quicklist ql;
  for (int i = 0; i < 10; i++) {
    ql.PushTail(std::to_string(i));
  }
  EXPECT_EQ(ql.Size(), 10);

  EXPECT_TRUE(ql.DeleteRange(2, 3));  // delete indices 2,3,4
  EXPECT_EQ(ql.Size(), 7);
  EXPECT_EQ(ql.Get(0).value(), "0");
  EXPECT_EQ(ql.Get(1).value(), "1");
  EXPECT_EQ(ql.Get(2).value(), "5");
}

TEST(QuicklistTest, DeleteRangeAcrossNodesFromMiddle) {
  Quicklist ql;
  std::vector<std::string> values;
  values.reserve(6);
  for (int i = 0; i < 6; i++) {
    values.push_back(std::to_string(i) + std::string(3000, 'x'));
    ql.PushTail(values.back());
  }
  ASSERT_GT(ql.NodeCount(), 1);

  EXPECT_TRUE(ql.DeleteRange(1, 4));
  ASSERT_EQ(ql.Size(), 2);
  EXPECT_EQ(ql.Get(0).value(), values[0]);
  EXPECT_EQ(ql.Get(1).value(), values[5]);
}

// ===== Find =====
TEST(QuicklistTest, FindValue) {
  Quicklist ql;
  ql.PushTail(std::string_view("apple"));
  ql.PushTail(std::string_view("banana"));
  ql.PushTail(std::string_view("cherry"));

  auto idx = ql.Find(std::string_view("banana"));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1);

  EXPECT_FALSE(ql.Find(std::string_view("grape")).has_value());
}

// ===== Large data — node split =====
TEST(QuicklistTest, ManyElementsCausesNodeSplit) {
  Quicklist ql;
  for (int i = 0; i < 10000; i++) {
    ql.PushTail(std::to_string(i));
  }
  EXPECT_EQ(ql.Size(), 10000);
  EXPECT_GT(ql.NodeCount(), 1);

  EXPECT_EQ(ql.Get(0).value(), "0");
  EXPECT_EQ(ql.Get(9999).value(), "9999");
}

// ===== Empty operations =====
TEST(QuicklistTest, PopEmptyList) {
  Quicklist ql;
  EXPECT_FALSE(ql.PopHead().has_value());
  EXPECT_FALSE(ql.PopTail().has_value());
}

TEST(QuicklistTest, DeleteOnEmpty) {
  Quicklist ql;
  EXPECT_FALSE(ql.Delete(0));
}

// Regression: PushTail on empty list must not invoke UB
TEST(QuicklistTest, PushTailOnEmptyList) {
  Quicklist ql;
  ql.PushTail(std::string_view("first"));  // must not crash
  EXPECT_EQ(ql.Size(), 1);
  EXPECT_EQ(ql.Get(0).value(), "first");
}

// Regression: PushHead on empty list must not invoke UB
TEST(QuicklistTest, PushHeadOnEmptyList) {
  Quicklist ql;
  ql.PushHead(std::string_view("first"));  // must not crash
  EXPECT_EQ(ql.Size(), 1);
  EXPECT_EQ(ql.Get(0).value(), "first");
}

// PushTail then PushHead interleaved (stresses node creation paths)
TEST(QuicklistTest, InterleavedPushTailHead) {
  Quicklist ql;
  ql.PushTail(std::string_view("b"));
  ql.PushHead(std::string_view("a"));
  ql.PushTail(std::string_view("c"));

  EXPECT_EQ(ql.Size(), 3);
  EXPECT_EQ(ql.Get(0).value(), "a");
  EXPECT_EQ(ql.Get(1).value(), "b");
  EXPECT_EQ(ql.Get(2).value(), "c");
}

}  // namespace
}  // namespace miniredis::ds
