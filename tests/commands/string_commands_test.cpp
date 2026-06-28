#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(StringCommandsTest, SetAndGet) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"GET", "k"}), "$1\r\nv\r\n");
  EXPECT_EQ(h.Call(reg, {"GET", "missing"}), "$-1\r\n");
}

TEST(StringCommandsTest, Incr) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"INCR", "cnt"}), ":1\r\n");
  EXPECT_EQ(h.Call(reg, {"INCRBY", "cnt", "4"}), ":5\r\n");
  EXPECT_EQ(h.Call(reg, {"GET", "cnt"}), "$1\r\n5\r\n");
}

TEST(StringCommandsTest, DecrBySubtractsDelta) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "cnt", "5"}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"DECRBY", "cnt", "2"}), ":3\r\n");
  EXPECT_EQ(h.Call(reg, {"GET", "cnt"}), "$1\r\n3\r\n");
}

TEST(StringCommandsTest, IncrErrorPreservesOriginalValue) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "counter", "not-an-int"}), "+OK\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"INCR", "counter"})));
  EXPECT_EQ(h.Call(reg, {"GET", "counter"}), "$10\r\nnot-an-int\r\n");
}

TEST(StringCommandsTest, InPlaceIncrementPreservesTtl) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "counter", "1"}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"PEXPIRE", "counter", "10000"}), ":1\r\n");
  EXPECT_EQ(h.Call(reg, {"INCR", "counter"}), ":2\r\n");
  auto ttl = h.Call(reg, {"PTTL", "counter"});
  EXPECT_FALSE(ttl == ":-1\r\n") << ttl;
  EXPECT_FALSE(ttl == ":-2\r\n") << ttl;
}

TEST(StringCommandsTest, IncrByFloatErrorPreservesOriginalValue) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "score", "not-a-float"}), "+OK\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"INCRBYFLOAT", "score", "1.5"})));
  EXPECT_EQ(h.Call(reg, {"GET", "score"}), "$11\r\nnot-a-float\r\n");
}

TEST(StringCommandsTest, IncrByFloatPreservesTtl) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "score", "1.5"}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"PEXPIRE", "score", "10000"}), ":1\r\n");
  EXPECT_EQ(h.Call(reg, {"INCRBYFLOAT", "score", "0.5"}), "$3\r\n2.0\r\n");
  auto ttl = h.Call(reg, {"PTTL", "score"});
  EXPECT_FALSE(ttl == ":-1\r\n") << ttl;
  EXPECT_FALSE(ttl == ":-2\r\n") << ttl;
}

TEST(StringCommandsTest, MSetRejectsOddArgumentCount) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"MSET", "a", "1", "dangling"})));
  EXPECT_EQ(h.Call(reg, {"GET", "a"}), "$-1\r\n");
}

TEST(StringCommandsTest, SetRangeUsesOffsetForNewKey) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SETRANGE", "key", "3", "x"}), ":4\r\n");
  std::string expected = "$4\r\n";
  expected.append(3, '\0');
  expected += "x\r\n";
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), expected);
}

TEST(StringCommandsTest, SetExRejectsNegativeTtl) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"SETEX", "key", "-1", "value"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$-1\r\n");
}

TEST(StringCommandsTest, PSetExRejectsNonPositiveTtlWithoutMutation) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "key", "old"}), "+OK\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"PSETEX", "key", "0", "new"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$3\r\nold\r\n");
  EXPECT_TRUE(IsErr(h.Call(reg, {"PSETEX", "key", "-1", "new"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$3\r\nold\r\n");
}

TEST(StringCommandsTest, ExpiringSetRejectsOverflowWithoutMutation) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_EQ(h.Call(reg, {"SET", "key", "old"}), "+OK\r\n");
  EXPECT_TRUE(
      IsErr(h.Call(reg, {"SETEX", "key", "9223372036854775807", "new"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$3\r\nold\r\n");
  EXPECT_TRUE(
      IsErr(h.Call(reg, {"PSETEX", "key", "9223372036854775807", "new"})));
  EXPECT_EQ(h.Call(reg, {"GET", "key"}), "$3\r\nold\r\n");
}

}  // namespace
}  // namespace miniredis
