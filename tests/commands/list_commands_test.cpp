#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(ListCommandsTest, RPopReturnsValuesAndDeletesDrainedKey) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), "$-1\r\n");
  EXPECT_EQ(h.Call(reg, {"RPUSH", "list", "head", "", "42",
                          std::string("a\0b", 3)}), ":4\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), std::string("$3\r\na\0b\r\n", 9));
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), "$2\r\n42\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), "$0\r\n\r\n");
  EXPECT_EQ(h.Call(reg, {"LLEN", "list"}), ":1\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), "$4\r\nhead\r\n");
  EXPECT_EQ(h.Call(reg, {"EXISTS", "list"}), ":0\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOP", "list"}), "$-1\r\n");
}

TEST(ListCommandsTest, LTrimRemovesTailAcrossNodes) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  std::vector<std::string> values;
  for (int i = 0; i < 6; ++i) {
    values.push_back(std::to_string(i) + std::string(3000, 'x'));
    EXPECT_EQ(h.Call(reg, {"RPUSH", "list", values.back()}),
              ":" + std::to_string(i + 1) + "\r\n");
  }
  EXPECT_EQ(h.Call(reg, {"LTRIM", "list", "1", "3"}), "+OK\r\n");
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "list", "0", "-1"})),
            (std::vector<std::string>{values[1], values[2], values[3]}));
  EXPECT_EQ(h.Call(reg, {"LTRIM", "list", "1", "0"}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"EXISTS", "list"}), ":0\r\n");
}

TEST(ListCommandsTest, RPopLPushMovesElementToDestination) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"RPUSH", "src", "a", "b"}), ":2\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOPLPUSH", "src", "dst"}), "$1\r\nb\r\n");
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "src", "0", "-1"})),
            std::vector<std::string>({"a"}));
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "dst", "0", "-1"})),
            std::vector<std::string>({"b"}));
}

TEST(ListCommandsTest, RPopLPushSameKeyRotatesList) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"RPUSH", "list", "a", "b"}), ":2\r\n");
  EXPECT_EQ(h.Call(reg, {"RPOPLPUSH", "list", "list"}), "$1\r\nb\r\n");
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "list", "0", "-1"})),
            std::vector<std::string>({"b", "a"}));
}

TEST(ListCommandsTest, RPopLPushWrongTypeDestinationDoesNotPopSource) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"RPUSH", "src", "a", "b"}), ":2\r\n");
  EXPECT_EQ(h.Call(reg, {"SET", "dst", "not-a-list"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"RPOPLPUSH", "src", "dst"})));
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "src", "0", "-1"})),
            std::vector<std::string>({"a", "b"}));
  EXPECT_EQ(h.Call(reg, {"GET", "dst"}), "$10\r\nnot-a-list\r\n");
}

TEST(ListCommandsTest, ExistingWrongTypeReturnsWrongType) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "list", "not-a-list"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LLEN", "list"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LPOP", "list"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"RPOP", "list"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LINDEX", "list", "0"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LRANGE", "list", "0", "-1"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LTRIM", "list", "0", "-1"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LREM", "list", "0", "x"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LPUSH", "list", "x"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"LPUSHX", "list", "x"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"RPOPLPUSH", "list", "dst"})));
}

TEST(ListCommandsTest, PopCountArgumentsAreRejectedUntilImplemented) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"RPUSH", "list", "a", "b"}), ":2\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"LPOP", "list", "2"})));
  EXPECT_TRUE(IsErr(h.Call(reg, {"RPOP", "list", "2"})));
  EXPECT_EQ(ParseBulkArray(h.Call(reg, {"LRANGE", "list", "0", "-1"})),
            std::vector<std::string>({"a", "b"}));
}

}  // namespace
}  // namespace miniredis
