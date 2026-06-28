#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(SetCommandsTest, ExistingWrongTypeReturnsWrongType) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "set", "not-a-set"}), "+OK\r\n");
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SCARD", "set"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SMEMBERS", "set"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SISMEMBER", "set", "member"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SREM", "set", "member"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SPOP", "set"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SRANDMEMBER", "set"})));
  EXPECT_TRUE(IsWrongType(h.Call(reg, {"SADD", "set", "member"})));
}

TEST(SetCommandsTest, CountOptionsAreRejectedUntilImplemented) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SADD", "set", "a", "b"}), ":2\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"SPOP", "set", "2"})));
  EXPECT_TRUE(IsErr(h.Call(reg, {"SRANDMEMBER", "set", "2"})));
}

}  // namespace
}  // namespace miniredis
