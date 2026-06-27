#include "types/list_value.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(ListValueTest, ConstructEmpty) {
  ListValue lv;
  EXPECT_EQ(lv.Size(), 0);
  EXPECT_TRUE(lv.Empty());
  EXPECT_EQ(lv.Encoding(), ValueEncoding::kQuicklist);
}

TEST(ListValueTest, PushTailAndGet) {
  ListValue lv;
  lv.PushTail("a");
  lv.PushTail("b");
  EXPECT_EQ(lv.Size(), 2);
  EXPECT_EQ(lv.Get(0).value(), "a");
  EXPECT_EQ(lv.Get(1).value(), "b");
}

TEST(ListValueTest, NegativeIndex) {
  ListValue lv;
  lv.PushTail("a");
  lv.PushTail("b");
  lv.PushTail("c");
  EXPECT_EQ(lv.Get(-1).value(), "c");
  EXPECT_EQ(lv.Get(-2).value(), "b");
  EXPECT_EQ(lv.Get(-3).value(), "a");
  EXPECT_FALSE(lv.Get(-4).has_value());
}

TEST(ListValueTest, PopHeadTail) {
  ListValue lv;
  lv.PushTail("a");
  lv.PushTail("b");
  EXPECT_EQ(lv.PopHead().value(), "a");
  EXPECT_EQ(lv.PopTail().value(), "b");
  EXPECT_TRUE(lv.Empty());
}

TEST(ListValueTest, Range) {
  ListValue lv;
  for (int i = 0; i < 5; i++) lv.PushTail(std::to_string(i));
  auto r = lv.Range(1, 3);
  ASSERT_EQ(r.size(), 3);
  EXPECT_EQ(r[0], "1");
  EXPECT_EQ(r[1], "2");
  EXPECT_EQ(r[2], "3");
}

TEST(ListValueTest, RangeNegativeIndex) {
  ListValue lv;
  for (int i = 0; i < 5; i++) lv.PushTail(std::to_string(i));
  auto r = lv.Range(-3, -1);
  ASSERT_EQ(r.size(), 3);
  EXPECT_EQ(r[0], "2");
  EXPECT_EQ(r[1], "3");
  EXPECT_EQ(r[2], "4");
}

TEST(ListValueTest, Trim) {
  ListValue lv;
  for (int i = 0; i < 5; i++) lv.PushTail(std::to_string(i));
  EXPECT_TRUE(lv.Trim(1, 3));
  EXPECT_EQ(lv.Size(), 3);
  EXPECT_EQ(lv.Get(0).value(), "1");
}

TEST(ListValueTest, SetByIndex) {
  ListValue lv;
  lv.PushTail("a");
  lv.PushTail("b");
  EXPECT_TRUE(lv.Set(0, "x"));
  EXPECT_EQ(lv.Get(0).value(), "x");
}

TEST(ListValueTest, Find) {
  ListValue lv;
  lv.PushTail("x");
  lv.PushTail("y");
  lv.PushTail("z");
  EXPECT_EQ(lv.Find("y").value(), 1);
  EXPECT_FALSE(lv.Find("w").has_value());
}

}  // namespace
}  // namespace miniredis
