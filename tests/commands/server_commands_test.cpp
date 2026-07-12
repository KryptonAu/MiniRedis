#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

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

TEST(ServerCommandsTest, TimeReturnsTwoBulkStrings) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  auto reply = h.Call(reg, {"TIME"});
  auto parts = ParseBulkArray(reply);
  ASSERT_EQ(parts.size(), 2);
  EXPECT_FALSE(parts[0].empty());
  EXPECT_FALSE(parts[1].empty());
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

TEST(ServerCommandsTest, ConfigSetAppendOnlyUsesRuntimeHook) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  auto ctx = h.Context();
  bool hook_called = false;
  ctx.apply_config = [&](std::string_view key, std::string_view value) {
    hook_called = key == "appendonly" && value == "yes";
    return h.server.ApplyConfig(key, value);
  };

  EXPECT_EQ(ExecuteCommand(reg, ctx, {"CONFIG", "SET", "appendonly", "yes"}),
            "+OK\r\n");
  EXPECT_TRUE(hook_called);
  EXPECT_TRUE(h.server.GetConfig().appendonly);
}

TEST(ServerCommandsTest, SaveIsRejectedWhenRdbPersistenceIsDisabled) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  const auto rdb_path =
      std::filesystem::temp_directory_path() /
      ("miniredis-disabled-save-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".rdb");
  std::filesystem::remove(rdb_path);

  MiniRedisConfig config;
  config.databases = 4;
  config.save_enabled = false;
  config.rdb_filename = rdb_path.string();
  ASSERT_TRUE(h.server.Init(config));

  const auto reply = h.Call(reg, {"SAVE"});

  EXPECT_EQ(reply, "-ERR RDB persistence is disabled\r\n");
  EXPECT_FALSE(std::filesystem::exists(rdb_path));
}

TEST(ServerCommandsTest, InfoIsRegistered) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;

  EXPECT_NE(reg.Find("INFO"), nullptr);
  EXPECT_FALSE(h.Call(reg, {"INFO"}).starts_with("-ERR unknown command"));
}

}  // namespace
}  // namespace miniredis
