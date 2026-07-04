#include "commands/command_helpers.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(CommandHelpersTest, MsToSecRoundsUpPositiveTtl) {
  EXPECT_EQ(MsToSec(1), 1);
  EXPECT_EQ(MsToSec(999), 1);
  EXPECT_EQ(MsToSec(1000), 1);
  EXPECT_EQ(MsToSec(1001), 2);
}

TEST(CommandHelpersTest, ZSetRangeUsesStableScoreFormatting) {
  std::vector<ZSetValue::RangeResult> values = {{"member", 1.0}};

  EXPECT_EQ(ArrayOfZSetRange(values, true),
            "*2\r\n$6\r\nmember\r\n$3\r\n1.0\r\n");
  EXPECT_EQ(ArrayOfZSetRange(values, false), "*1\r\n$6\r\nmember\r\n");
}

TEST(CommandHelpersTest, ScanReplyUsesNestedRespArray) {
  EXPECT_EQ(ScanReply(5, {"a", "bb"}),
            "*2\r\n$1\r\n5\r\n*2\r\n$1\r\na\r\n$2\r\nbb\r\n");
}

TEST(CommandHelpersTest, LookupKeyAsDistinguishesMissingValueAndWrongType) {
  Database db;

  auto missing = LookupKeyAs<StringValue>(db, "key");
  EXPECT_TRUE(missing.Missing());
  EXPECT_FALSE(missing.WrongType());
  EXPECT_EQ(missing.value, nullptr);

  db.Set("key", StringValue("value"));
  auto string_lookup = LookupKeyAs<StringValue>(db, "key");
  ASSERT_NE(string_lookup.value, nullptr);
  EXPECT_FALSE(string_lookup.Missing());
  EXPECT_FALSE(string_lookup.WrongType());
  EXPECT_EQ(string_lookup.value->ToString(), "value");

  auto wrong_type = LookupKeyAs<ListValue>(db, "key");
  EXPECT_FALSE(wrong_type.Missing());
  EXPECT_TRUE(wrong_type.WrongType());
  EXPECT_EQ(wrong_type.value, nullptr);
}

TEST(CommandHelpersTest, GetOrCreateValueAsCreatesOnlyWhenMissing) {
  Database db;

  auto missing = LookupKeyAs<StringValue>(db, "key");
  auto& created = GetOrCreateValueAs<StringValue>(
      db, "key", missing, [] { return StringValue("created"); });
  EXPECT_EQ(created.ToString(), "created");
  ASSERT_NE(missing.value, nullptr);
  EXPECT_EQ(missing.value->ToString(), "created");

  auto existing = LookupKeyAs<StringValue>(db, "key");
  auto* before = existing.value;
  auto& reused = GetOrCreateValueAs<StringValue>(
      db, "key", existing, [] { return StringValue("replacement"); });
  EXPECT_EQ(&reused, before);
  EXPECT_EQ(reused.ToString(), "created");
}

}  // namespace
}  // namespace miniredis
