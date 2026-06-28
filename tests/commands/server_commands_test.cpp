#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/registry.h"

namespace miniredis {
namespace {

TEST(ServerCommandsTest, Ping) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"PING"}), "+PONG\r\n");
}

TEST(ServerCommandsTest, Echo) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"ECHO", "hello"}), "$5\r\nhello\r\n");
}

TEST(ServerCommandsTest, SelectValid) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"SELECT", "2"}), "+OK\r\n");
  EXPECT_EQ(h.client.CurrentDb(), 2);
}

TEST(ServerCommandsTest, SelectOutOfRange) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  auto r = h.Call(reg, {"SELECT", "99"});
  EXPECT_TRUE(r.starts_with("-ERR"));
}

TEST(ServerCommandsTest, DbSizeAndFlush) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"DBSIZE"}), ":0\r\n");
  EXPECT_EQ(h.Call(reg, {"FLUSHDB"}), "+OK\r\n");
}

TEST(ServerCommandsTest, CommandListsRegisteredCommands) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  auto reply = h.Call(reg, {"COMMAND"});
  for (const auto& name : reg.CommandNames()) {
    EXPECT_TRUE(ContainsBulk(reply, name)) << name << " missing from " << reply;
  }
}

TEST(ServerCommandsTest, ServerCommandsRejectUnexpectedExtraArguments) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"PING", "one", "two"})));
  EXPECT_TRUE(IsErr(h.Call(reg, {"COMMAND", "COUNT"})));
  EXPECT_TRUE(IsErr(h.Call(reg, {"INFO", "default", "extra"})));
  EXPECT_TRUE(IsErr(h.Call(reg, {"CONFIG", "GET", "databases", "extra"})));
}

TEST(ServerCommandsTest, ConfigGetReturnsKnownField) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  auto reply = h.Call(reg, {"CONFIG", "GET", "databases"});
  EXPECT_EQ(ParseBulkArray(reply),
            std::vector<std::string>({"databases", "4"}));
}

TEST(ServerCommandsTest, ConfigSetIsRejectedUntilImplemented) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_TRUE(IsErr(h.Call(reg, {"CONFIG", "SET", "databases", "2"})));
}

TEST(ServerCommandsTest, InfoIsRegistered) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_NE(reg.Find("INFO"), nullptr);
  EXPECT_FALSE(h.Call(reg, {"INFO"}).starts_with("-ERR unknown command"));
}

}  // namespace
}  // namespace miniredis
