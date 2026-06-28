#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(ZSetCommandsTest, ZAddRejectsOddScoreMemberCount) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"ZADD", "zset", "1", "one", "2"})));
  EXPECT_EQ(h.Call(reg, {"ZCARD", "zset"}), ":0\r\n");
}

TEST(ZSetCommandsTest, ZAddInvalidScoreDoesNotPartiallyMutate) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"ZADD", "zset", "1", "one", "bad", "two"})));
  EXPECT_EQ(h.Call(reg, {"TYPE", "zset"}), "+none\r\n");
}

TEST(ZSetCommandsTest, ZAddXxOnMissingKeyDoesNotCreateEmptyZSet) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "XX", "1", "one"}), ":0\r\n");
  EXPECT_EQ(h.Call(reg, {"TYPE", "zset"}), "+none\r\n");
  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "XX", "INCR", "1", "one"}), "$-1\r\n");
  EXPECT_EQ(h.Call(reg, {"TYPE", "zset"}), "+none\r\n");
}

TEST(ZSetCommandsTest, ZAddIncrAddsDeltaToExistingScore) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "2", "member"}), ":1\r\n");
  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "INCR", "1", "member"}),
            "$3\r\n3.0\r\n");
  EXPECT_EQ(h.Call(reg, {"ZSCORE", "zset", "member"}), "$3\r\n3.0\r\n");
}

TEST(ZSetCommandsTest, ZAddIncrRejectsExtraArgumentsWithoutMutation) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "2", "member"}), ":1\r\n");
  EXPECT_TRUE(
      IsErr(h.Call(reg, {"ZADD", "zset", "INCR", "1", "member", "extra"})));
  EXPECT_EQ(h.Call(reg, {"ZSCORE", "zset", "member"}), "$3\r\n2.0\r\n");
}

TEST(ZSetCommandsTest, ZRangeRejectsUnknownOption) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "1", "one"}), ":1\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"ZRANGE", "zset", "0", "-1", "BAD"})));
  EXPECT_TRUE(
      IsErr(h.Call(reg, {"ZRANGE", "zset", "0", "-1", "WITHSCORES", "BAD"})));
}

TEST(ZSetCommandsTest, ZRangeByScoreRejectsInvalidOptions) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "1", "one"}), ":1\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"ZRANGEBYSCORE", "zset", "0", "2", "BAD"})));
  EXPECT_TRUE(IsErr(
      h.Call(reg, {"ZRANGEBYSCORE", "zset", "0", "2", "LIMIT", "bad", "1"})));
  EXPECT_TRUE(
      IsErr(h.Call(reg, {"ZRANGEBYSCORE", "zset", "0", "2", "LIMIT", "0"})));
}

TEST(ZSetCommandsTest, ExistingWrongTypeReturnsWrongType) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "zset", "not-a-zset"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZCARD", "zset"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZRANGE", "zset", "0", "-1"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZRANGEBYSCORE", "zset", "0", "1"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZPOPMIN", "zset"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZREMRANGEBYRANK", "zset", "0", "-1"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"ZADD", "zset", "1", "one"})));
}

TEST(ZSetCommandsTest, ZIncrByInvalidScoreDoesNotCreateZSet) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"ZINCRBY", "zset", "bad", "member"})));
  EXPECT_EQ(h.Call(reg, {"TYPE", "zset"}), "+none\r\n");
}

TEST(ZSetCommandsTest, ZPopCountPopsMultipleElements) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "1", "one", "2", "two", "3", "three"}),
            ":3\r\n");
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"ZPOPMIN", "zset", "2"})),
            std::vector<std::string>({"one", "1.0", "two", "2.0"}));
  EXPECT_EQ(h.Call(reg, {"ZCARD", "zset"}), ":1\r\n");
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"ZPOPMAX", "zset", "2"})),
            std::vector<std::string>({"three", "3.0"}));
  EXPECT_EQ(h.Call(reg, {"TYPE", "zset"}), "+none\r\n");
}

TEST(ZSetCommandsTest, ZPopInvalidCountDoesNotMutate) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"ZADD", "zset", "1", "one"}), ":1\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"ZPOPMIN", "zset", "-1"})));
  EXPECT_EQ(h.Call(reg, {"ZCARD", "zset"}), ":1\r\n");
}

}  // namespace
}  // namespace miniredis
