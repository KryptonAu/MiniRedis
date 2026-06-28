#include "commands/registry.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

static std::string TestCmd(CommandContext&, const std::vector<std::string>&) {
  return "+OK\r\n";
}

TEST(RegistryTest, FindCaseInsensitive) {
  CommandRegistry reg;
  reg.Register({"PING", 1, 0, TestCmd});
  EXPECT_NE(reg.Find("ping"), nullptr);
  EXPECT_NE(reg.Find("PING"), nullptr);
  EXPECT_NE(reg.Find("Ping"), nullptr);
  EXPECT_EQ(reg.Find("unknown"), nullptr);
}

TEST(RegistryTest, CommandNamesSorted) {
  CommandRegistry reg;
  reg.Register({"ECHO", 2, 0, TestCmd});
  reg.Register({"PING", 1, 0, TestCmd});
  auto names = reg.CommandNames();
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_EQ(names.size(), 2);
}

}  // namespace
}  // namespace miniredis
