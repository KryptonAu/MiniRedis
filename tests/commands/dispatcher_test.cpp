#include "commands/dispatcher.h"

#include <gtest/gtest.h>

#include "command_test_util.h"
#include "commands/command_context.h"
#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"

namespace miniredis {
namespace {

static std::string echo(CommandContext&, const std::vector<std::string>& args) {
  return "$" + std::to_string(args[1].size()) + "\r\n" + args[1] + "\r\n";
}

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

}  // namespace
}  // namespace miniredis
