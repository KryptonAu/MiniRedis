#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(KeyCommandsTest, ExpireAtRejectsNegativeTimestamp) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "key", "value"}), "+OK\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"EXPIREAT", "key", "-1"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$5\r\nvalue\r\n");
}

TEST(KeyCommandsTest, KeysRejectsUnsupportedPattern) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"KEYS", "prefix*"})));
}

TEST(KeyCommandsTest, ExpireRejectsOverflowWithoutChangingKey) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "key", "value"}), "+OK\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"EXPIRE", "key", "9223372036854775807"})));
  EXPECT_EQ(h.Call(reg, {"TTL", "key"}), ":-1\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"PEXPIRE", "key", "9223372036854775807"})));
  EXPECT_EQ(h.Call(reg, {"TTL", "key"}), ":-1\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"EXPIREAT", "key", "9223372036854775807"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$5\r\nvalue\r\n");
}

}  // namespace
}  // namespace miniredis
