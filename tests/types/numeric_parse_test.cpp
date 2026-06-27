#include "types/numeric_parse.h"

#include <gtest/gtest.h>

#include <limits>

namespace miniredis {
namespace {

// ===== ParseCanonicalInt: acceptance =====
TEST(NumericParseTest, AcceptZero) {
  auto r = ParseCanonicalInt("0");
  ASSERT_TRUE(std::holds_alternative<ParsedInt>(r));
  EXPECT_EQ(std::get<ParsedInt>(r).value, 0);
}

TEST(NumericParseTest, AcceptPositive) {
  auto r = ParseCanonicalInt("123");
  ASSERT_TRUE(std::holds_alternative<ParsedInt>(r));
  EXPECT_EQ(std::get<ParsedInt>(r).value, 123);
}

TEST(NumericParseTest, AcceptNegative) {
  auto r = ParseCanonicalInt("-456");
  ASSERT_TRUE(std::holds_alternative<ParsedInt>(r));
  EXPECT_EQ(std::get<ParsedInt>(r).value, -456);
}

TEST(NumericParseTest, AcceptInt64Max) {
  auto r = ParseCanonicalInt("9223372036854775807");
  ASSERT_TRUE(std::holds_alternative<ParsedInt>(r));
  EXPECT_EQ(std::get<ParsedInt>(r).value, std::numeric_limits<int64_t>::max());
}

TEST(NumericParseTest, AcceptInt64Min) {
  auto r = ParseCanonicalInt("-9223372036854775808");
  ASSERT_TRUE(std::holds_alternative<ParsedInt>(r));
  EXPECT_EQ(std::get<ParsedInt>(r).value, std::numeric_limits<int64_t>::min());
}

// ===== ParseCanonicalInt: rejection =====
TEST(NumericParseTest, RejectEmpty) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("")));
}

TEST(NumericParseTest, RejectLeadingZeros) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("001")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("00")));
}

TEST(NumericParseTest, RejectLeadingPlus) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("+1")));
}

TEST(NumericParseTest, RejectMinusZero) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("-0")));
}

TEST(NumericParseTest, RejectNonNumeric) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("abc")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt("12a")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseCanonicalInt(" 1")));
}

TEST(NumericParseTest, RejectOverflow) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(
      ParseCanonicalInt("9223372036854775808")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(
      ParseCanonicalInt("-9223372036854775809")));
}

// ===== AddChecked =====
TEST(NumericParseTest, AddCheckedNormal) {
  auto r = AddChecked(100, 200);
  ASSERT_TRUE(std::holds_alternative<int64_t>(r));
  EXPECT_EQ(std::get<int64_t>(r), 300);
}

TEST(NumericParseTest, AddCheckedOverflow) {
  auto r = AddChecked(std::numeric_limits<int64_t>::max(), 1);
  EXPECT_TRUE(std::holds_alternative<TypeError>(r));
}

TEST(NumericParseTest, AddCheckedUnderflow) {
  auto r = AddChecked(std::numeric_limits<int64_t>::min(), -1);
  EXPECT_TRUE(std::holds_alternative<TypeError>(r));
}

// ===== ParseFiniteDouble =====
TEST(NumericParseTest, ParseFiniteDoubleNormal) {
  auto r = ParseFiniteDouble("3.14");
  ASSERT_TRUE(std::holds_alternative<double>(r));
  EXPECT_DOUBLE_EQ(std::get<double>(r), 3.14);
}

TEST(NumericParseTest, ParseFiniteDoubleRejectsNaN) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseFiniteDouble("nan")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseFiniteDouble("NAN")));
}

TEST(NumericParseTest, ParseFiniteDoubleRejectsInf) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseFiniteDouble("inf")));
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseFiniteDouble("-inf")));
}

TEST(NumericParseTest, ParseFiniteDoubleRejectsEmpty) {
  EXPECT_TRUE(std::holds_alternative<TypeError>(ParseFiniteDouble("")));
}

// ===== FormatDoubleForStorage =====
TEST(NumericParseTest, FormatDoubleRoundTrip) {
  std::string s = FormatDoubleForStorage(1.5);
  auto r = ParseFiniteDouble(s);
  ASSERT_TRUE(std::holds_alternative<double>(r));
  EXPECT_DOUBLE_EQ(std::get<double>(r), 1.5);
}

}  // namespace
}  // namespace miniredis
