#include "persistence/aof.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace miniredis {
namespace {

std::filesystem::path TempAofPath(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("miniredis_" + std::string(name) + "_" +
          std::to_string(static_cast<long long>(::getpid())) + ".aof");
}

TEST(AofTest, AppendAndReadCommandsRoundTrip) {
  auto path = TempAofPath("append_read");

  AofWriter writer;
  ASSERT_TRUE(writer.Open(path.string(), "always"));
  ASSERT_TRUE(writer.AppendCommand({"SET", "key", "value"}));
  ASSERT_TRUE(writer.AppendCommand({"PEXPIREAT", "key", "12345"}));
  writer.Close();

  AofReader reader;
  std::vector<std::vector<std::string>> commands;
  ASSERT_TRUE(reader.ReadCommands(path.string(), commands));

  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0], std::vector<std::string>({"SET", "key", "value"}));
  EXPECT_EQ(commands[1],
            std::vector<std::string>({"PEXPIREAT", "key", "12345"}));

  std::filesystem::remove(path);
}

TEST(AofTest, AppendCommandForDbWritesSelectWhenDbChanges) {
  auto path = TempAofPath("append_db_select");

  AofWriter writer;
  ASSERT_TRUE(writer.Open(path.string(), "always"));
  int selected_db = 0;
  ASSERT_TRUE(
      writer.AppendCommandForDb(2, {"SET", "key", "value"}, selected_db));
  EXPECT_EQ(selected_db, 2);
  ASSERT_TRUE(
      writer.AppendCommandForDb(2, {"SET", "other", "value"}, selected_db));
  ASSERT_TRUE(
      writer.AppendCommandForDb(0, {"SET", "root", "value"}, selected_db));
  EXPECT_EQ(selected_db, 0);
  writer.Close();

  AofReader reader;
  std::vector<std::vector<std::string>> commands;
  ASSERT_TRUE(reader.ReadCommands(path.string(), commands));

  ASSERT_EQ(commands.size(), 5u);
  EXPECT_EQ(commands[0], std::vector<std::string>({"SELECT", "2"}));
  EXPECT_EQ(commands[1], std::vector<std::string>({"SET", "key", "value"}));
  EXPECT_EQ(commands[2], std::vector<std::string>({"SET", "other", "value"}));
  EXPECT_EQ(commands[3], std::vector<std::string>({"SELECT", "0"}));
  EXPECT_EQ(commands[4], std::vector<std::string>({"SET", "root", "value"}));

  std::filesystem::remove(path);
}

}  // namespace
}  // namespace miniredis
