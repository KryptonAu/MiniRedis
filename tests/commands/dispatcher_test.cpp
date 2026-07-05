#include "commands/dispatcher.h"

#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/command_context.h"
#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"

namespace miniredis {
namespace {

static std::string echo(CommandContext&, CommandArgs args) {
  std::string reply = "$" + std::to_string(args[1].size()) + "\r\n";
  reply.append(args[1]);
  reply += "\r\n";
  return reply;
}

static std::string ok_write(CommandContext&, CommandArgs) { return "+OK\r\n"; }

TEST(DispatcherTest, UnknownCommand) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  auto ctx = h.Context();
  auto r = ExecuteCommand(reg, ctx, {"foobar"});
  EXPECT_TRUE(r.starts_with("-ERR"));
}

TEST(DispatcherTest, EmptyCommand) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  auto ctx = h.Context();
  auto r = ExecuteCommand(reg, ctx, {});
  EXPECT_TRUE(r.starts_with("-ERR"));
}

TEST(DispatcherTest, ArityCheckExact) {
  CommandRegistry reg;
  reg.Register({"ECHO", 2, 0, echo});
  CommandTestHarness h;
  auto ctx = h.Context();
  auto r = ExecuteCommand(reg, ctx, {"ECHO", "msg"});
  EXPECT_EQ(r, "$3\r\nmsg\r\n");
}

TEST(DispatcherTest, ArityCheckTooFew) {
  CommandRegistry reg;
  reg.Register({"ECHO", 2, 0, echo});
  CommandTestHarness h;
  auto ctx = h.Context();
  auto r = ExecuteCommand(reg, ctx, {"ECHO"});
  EXPECT_TRUE(r.starts_with("-ERR"));
}

TEST(DispatcherTest, SuccessfulWriteIncrementsDirtyAndPropagates) {
  CommandRegistry reg;
  reg.Register(
      {"WRITE", 1, static_cast<uint32_t>(CommandFlag::kWrite), ok_write});
  CommandTestHarness h;
  auto ctx = h.Context();
  std::vector<std::string> propagated;
  int propagated_db = -1;
  ctx.propagate = [&](int db_index, const std::vector<std::string>& args) {
    propagated_db = db_index;
    propagated = args;
    return true;
  };

  auto result = ExecuteCommandDetailed(reg, ctx, {"WRITE"});

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_EQ(result.propagate_args, std::vector<std::string>({"WRITE"}));
  EXPECT_EQ(propagated_db, 0);
  EXPECT_EQ(propagated, std::vector<std::string>({"WRITE"}));
  EXPECT_EQ(h.server.Stats().dirty, 1u);
}

TEST(DispatcherTest, PropagationReceivesSelectedDb) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  ASSERT_TRUE(h.client.SelectDb(2, h.server.DbCount()));
  auto ctx = h.Context();
  int propagated_db = -1;
  std::vector<std::string> propagated;
  ctx.propagate = [&](int db_index, const std::vector<std::string>& args) {
    propagated_db = db_index;
    propagated = args;
    return true;
  };

  auto result = ExecuteCommandDetailed(reg, ctx, {"SET", "key", "value"});

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_EQ(propagated_db, 2);
  EXPECT_EQ(propagated, std::vector<std::string>({"SET", "key", "value"}));
}

TEST(DispatcherTest, IntegerZeroCanStillBeMutatingWrite) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  EXPECT_EQ(h.Call(reg, {"HSET", "hash", "field", "old"}), ":1\r\n");
  h.server.ResetDirty();

  auto ctx = h.Context();
  std::vector<std::string> propagated;
  ctx.propagate = [&](int, const std::vector<std::string>& args) {
    propagated = args;
    return true;
  };

  auto result =
      ExecuteCommandDetailed(reg, ctx, {"HSET", "hash", "field", "new"});

  EXPECT_EQ(result.reply, ":0\r\n");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_EQ(propagated,
            std::vector<std::string>({"HSET", "hash", "field", "new"}));
  EXPECT_EQ(h.server.Stats().dirty, 1u);
  EXPECT_EQ(h.Call(reg, {"HGET", "hash", "field"}), "$3\r\nnew\r\n");
}

TEST(DispatcherTest, NoEvictionRejectsWritesWhenAlreadyOverMaxmemory) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  std::string large_value(256, 'x');
  EXPECT_EQ(h.Call(reg, {"SET", "existing", large_value}), "+OK\r\n");
  ASSERT_TRUE(h.server.ApplyConfig("maxmemory", "1"));
  ASSERT_TRUE(h.server.ApplyConfig("maxmemory-policy", "noeviction"));
  h.server.ResetDirty();

  auto ctx = h.Context();
  bool propagated = false;
  ctx.propagate = [&](int, const std::vector<std::string>&) {
    propagated = true;
    return true;
  };

  auto result = ExecuteCommandDetailed(reg, ctx, {"SET", "new", "value"});

  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.reply.starts_with("-OOM"));
  EXPECT_FALSE(h.server.GetDb(0)->Exists("new"));
  EXPECT_FALSE(propagated);
  EXPECT_EQ(h.server.Stats().dirty, 0u);

  propagated = false;
  result = ExecuteCommandDetailed(reg, ctx, {"DEL", "existing"});
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_EQ(result.reply, ":1\r\n");
  EXPECT_FALSE(h.server.GetDb(0)->Exists("existing"));
  EXPECT_TRUE(propagated);
  EXPECT_EQ(h.server.Stats().dirty, 1u);
}

TEST(DispatcherTest, NoEvictionAllowsPersistWhenAlreadyOverMaxmemory) {
  auto reg = CreateDefaultCommandRegistry();
  CommandTestHarness h;
  std::string large_value(256, 'x');
  EXPECT_EQ(h.Call(reg, {"SET", "existing", large_value}), "+OK\r\n");
  EXPECT_EQ(h.Call(reg, {"EXPIRE", "existing", "60"}), ":1\r\n");
  ASSERT_TRUE(h.server.ApplyConfig("maxmemory", "1"));
  ASSERT_TRUE(h.server.ApplyConfig("maxmemory-policy", "noeviction"));
  h.server.ResetDirty();

  auto ctx = h.Context();
  auto result = ExecuteCommandDetailed(reg, ctx, {"PERSIST", "existing"});

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_EQ(result.reply, ":1\r\n");
  EXPECT_EQ(h.server.GetDb(0)->TTL("existing"), -1);
  EXPECT_EQ(h.server.Stats().dirty, 1u);
}

TEST(DispatcherTest, ReplayModeDoesNotIncrementDirtyOrPropagate) {
  CommandRegistry reg;
  reg.Register(
      {"WRITE", 1, static_cast<uint32_t>(CommandFlag::kWrite), ok_write});
  CommandTestHarness h;
  auto ctx = h.Context();

  auto result = ExecuteCommandDetailed(reg, ctx, {"WRITE"},
                                       /*replay_mode=*/true);

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.mutated);
  EXPECT_TRUE(result.propagate_args.empty());
  EXPECT_EQ(h.server.Stats().dirty, 0u);
}

}  // namespace
}  // namespace miniredis
