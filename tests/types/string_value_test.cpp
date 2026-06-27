#include "types/string_value.h"

#include <gtest/gtest.h>

#include <limits>

namespace miniredis {
namespace {

// ===== Construction =====
TEST(StringValueTest, DefaultConstruct) {
  StringValue sv;
  EXPECT_EQ(sv.ToString(), "0");
  EXPECT_EQ(sv.Length(), 1);
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntEmbed);
}

TEST(StringValueTest, ConstructFromInt) {
  StringValue sv(42);
  EXPECT_EQ(sv.ToString(), "42");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntEmbed);
}

TEST(StringValueTest, ConstructFromCanonicalIntString) {
  StringValue sv("123");
  EXPECT_EQ(sv.ToString(), "123");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntEmbed);
  EXPECT_EQ(sv.AsInt().value(), 123);
}

TEST(StringValueTest, ConstructFromNonCanonicalKeepsRaw) {
  StringValue sv("001");
  EXPECT_EQ(sv.ToString(), "001");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kRaw);
  EXPECT_FALSE(sv.AsInt().has_value());
}

TEST(StringValueTest, ConstructFromZero) {
  StringValue sv("0");
  EXPECT_EQ(sv.ToString(), "0");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntEmbed);
}

TEST(StringValueTest, ConstructFromPlainString) {
  StringValue sv("hello");
  EXPECT_EQ(sv.ToString(), "hello");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kRaw);
  EXPECT_EQ(sv.AsString().value(), "hello");
}

// ===== Set =====
TEST(StringValueTest, SetChangesEncoding) {
  StringValue sv("hello");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kRaw);
  sv.Set("123");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntEmbed);
  sv.Set("abc");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kRaw);
}

// ===== Append =====
TEST(StringValueTest, AppendConvertsIntToRaw) {
  StringValue sv(42);
  sv.Append("_suffix");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kRaw);
  EXPECT_EQ(sv.ToString(), "42_suffix");
}

// ===== IncrementBy =====
TEST(StringValueTest, IncrementByOnInt) {
  StringValue sv(10);
  auto result = sv.IncrementBy(5);
  ASSERT_TRUE(std::holds_alternative<int64_t>(result));
  EXPECT_EQ(std::get<int64_t>(result), 15);
  EXPECT_EQ(sv.ToString(), "15");
}

TEST(StringValueTest, IncrementByOverflow) {
  StringValue sv(std::numeric_limits<int64_t>::max());
  auto result = sv.IncrementBy(1);
  EXPECT_TRUE(std::holds_alternative<TypeError>(result));
}

TEST(StringValueTest, IncrementByNonInteger) {
  StringValue sv("abc");
  auto result = sv.IncrementBy(1);
  EXPECT_TRUE(std::holds_alternative<TypeError>(result));
}

// ===== GetRange / SetRange =====
TEST(StringValueTest, GetRangeBasic) {
  StringValue sv("hello");
  EXPECT_EQ(sv.GetRange(0, 1), "he");
  EXPECT_EQ(sv.GetRange(0, -1), "hello");
}

TEST(StringValueTest, SetRangeBasic) {
  StringValue sv("hello");
  sv.SetRange(1, "a");
  EXPECT_EQ(sv.ToString(), "hallo");
}

// ===== Length =====
TEST(StringValueTest, LengthOnInt) {
  StringValue sv(12345);
  EXPECT_EQ(sv.Length(), 5);
}

TEST(StringValueTest, LengthOnRaw) {
  StringValue sv("hello");
  EXPECT_EQ(sv.Length(), 5);
}

// ===== Move-only =====
TEST(StringValueTest, MovePreservesEncoding) {
  StringValue sv(42);
  StringValue sv2(std::move(sv));
  EXPECT_EQ(sv2.Encoding(), ValueEncoding::kIntEmbed);
  EXPECT_EQ(sv2.AsInt().value(), 42);
}

}  // namespace
}  // namespace miniredis
