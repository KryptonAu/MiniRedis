#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(HashCommandsTest, HSetRejectsOddFieldValueCount) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"HSET", "hash", "f1", "v1", "dangling"})));
  EXPECT_EQ(h.Call(reg, {"HGET", "hash", "f1"}), "$-1\r\n");
}

TEST(HashCommandsTest, ExistingWrongTypeReturnsWrongType) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "hash", "not-a-hash"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"HGET", "hash", "field"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"HSET", "hash", "field", "value"})));
}

TEST(HashCommandsTest, HIncrByWrongTypeReturnsWrongType) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "hash", "not-a-hash"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"HINCRBY", "hash", "field", "1"})));
  EXPECT_EQ(h.Call(reg, {"GET", "hash"}), "$10\r\nnot-a-hash\r\n");
}

TEST(HashCommandsTest, HIncrByInvalidDeltaDoesNotCreateHash) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"HINCRBY", "hash", "field", "not-int"})));
  EXPECT_EQ(h.Call(reg, {"TYPE", "hash"}), "+none\r\n");
}

}  // namespace
}  // namespace miniredis
