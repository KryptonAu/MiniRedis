#include "core/resp_protocol.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace miniredis {
namespace {

// ===== Parser: basic single command =====
TEST(RespParserTest, ParseSimpleCommand) {
  RespParser parser;
  auto status = parser.Feed("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");
  EXPECT_EQ(status, ParseStatus::kComplete);
  EXPECT_TRUE(parser.HasCommand());
  auto cmd = parser.TakeCommand();
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[0], "GET");
  EXPECT_EQ(cmd[1], "key");
  EXPECT_EQ(cmd.ToOwnedVector(), std::vector<std::string>({"GET", "key"}));
}

TEST(RespParserTest, ParseSetCommand) {
  RespParser parser;
  auto status = parser.Feed("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n");
  EXPECT_EQ(status, ParseStatus::kComplete);
  EXPECT_TRUE(parser.HasCommand());
  auto cmd = parser.TakeCommand();
  ASSERT_EQ(cmd.size(), 3);
  EXPECT_EQ(cmd[0], "SET");
  EXPECT_EQ(cmd[1], "key");
  EXPECT_EQ(cmd[2], "value");
}

TEST(RespParserTest, EmptyBulkStringArgument) {
  RespParser parser;
  auto status = parser.Feed("*2\r\n$3\r\nSET\r\n$0\r\n\r\n");
  EXPECT_EQ(status, ParseStatus::kComplete);
  auto cmd = parser.TakeCommand();
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[1], "");
}

// ===== Parser: partial reads =====
TEST(RespParserTest, PartialFeedIncomplete) {
  RespParser parser;
  auto s1 = parser.Feed("*2\r\n$3\r\n");
  EXPECT_EQ(s1, ParseStatus::kIncomplete);
  EXPECT_FALSE(parser.HasCommand());

  auto s2 = parser.Feed("GET\r\n$3\r\nkey\r\n");
  EXPECT_EQ(s2, ParseStatus::kComplete);
  EXPECT_TRUE(parser.HasCommand());
}

// ===== Parser: pipelining =====
TEST(RespParserTest, PipeliningTwoCommands) {
  RespParser parser;
  auto status = parser.Feed("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(status, ParseStatus::kComplete);
  EXPECT_EQ(parser.PendingCommandCount(), 2);

  auto cmd1 = parser.TakeCommand();
  EXPECT_EQ(parser.PendingCommandCount(), 1);
  auto cmd2 = parser.TakeCommand();
  EXPECT_EQ(parser.PendingCommandCount(), 0);
}

TEST(RespParserTest, TakenCommandSurvivesLaterFeed) {
  RespParser parser;
  ASSERT_EQ(parser.Feed("*1\r\n$4\r\nPING\r\n"), ParseStatus::kComplete);
  RespCommand first = parser.TakeCommand();

  ASSERT_EQ(parser.Feed("*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n"),
            ParseStatus::kComplete);
  RespCommand second = parser.TakeCommand();

  EXPECT_EQ(first.size(), 1);
  EXPECT_EQ(first[0], "PING");
  EXPECT_EQ(first.ToOwnedVector(), std::vector<std::string>({"PING"}));
  ASSERT_EQ(second.size(), 2);
  EXPECT_EQ(second[0], "ECHO");
  EXPECT_EQ(second[1], "hey");
}

TEST(RespParserTest, LargePipelineKeepsOrderAndClearsBuffer) {
  RespParser parser;
  std::string input;
  constexpr int kCommandCount = 10000;
  std::string frame = "*1\r\n$4\r\nPING\r\n";
  input.reserve(frame.size() * kCommandCount);
  for (int i = 0; i < kCommandCount; i++) {
    input += frame;
  }

  EXPECT_EQ(parser.Feed(input), ParseStatus::kComplete);
  EXPECT_EQ(parser.PendingCommandCount(), static_cast<size_t>(kCommandCount));
  EXPECT_EQ(parser.BufferSize(), 0);

  for (int i = 0; i < kCommandCount; i++) {
    RespCommand cmd = parser.TakeCommand();
    ASSERT_EQ(cmd.size(), 1);
    EXPECT_EQ(cmd[0], "PING");
  }
  EXPECT_FALSE(parser.HasCommand());
}

TEST(RespParserTest, LargeBulkStringCanArriveInPieces) {
  RespParser parser;
  std::string value(64 * 1024, 'x');
  std::string prefix =
      "*2\r\n$3\r\nSET\r\n$" + std::to_string(value.size()) + "\r\n";

  EXPECT_EQ(parser.Feed(prefix), ParseStatus::kIncomplete);
  EXPECT_FALSE(parser.HasCommand());
  EXPECT_EQ(parser.BufferSize(), prefix.size());

  EXPECT_EQ(parser.Feed(value + "\r\n"), ParseStatus::kComplete);
  ASSERT_TRUE(parser.HasCommand());
  RespCommand cmd = parser.TakeCommand();
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[0], "SET");
  EXPECT_EQ(cmd[1], value);
  EXPECT_EQ(parser.BufferSize(), 0);
}

// ===== Parser: error cases =====
TEST(RespParserTest, EmptyArrayError) {
  RespParser parser;
  auto status = parser.Feed("*0\r\n");
  EXPECT_EQ(status, ParseStatus::kError);
  EXPECT_TRUE(parser.LastError().has_value());
}

TEST(RespParserTest, NullBulkInCommandError) {
  RespParser parser;
  auto status = parser.Feed("*2\r\n$-1\r\n\r\n$3\r\nfoo\r\n");
  EXPECT_EQ(status, ParseStatus::kError);
}

TEST(RespParserTest, NestedArrayError) {
  RespParser parser;
  auto status = parser.Feed("*2\r\n*1\r\n$4\r\nNEST\r\n\r\n$3\r\nfoo\r\n");
  EXPECT_EQ(status, ParseStatus::kError);
}

TEST(RespParserTest, ResetAfterError) {
  RespParser parser;
  parser.Feed("*0\r\n");                                  // error
  EXPECT_EQ(parser.Feed("*2\r\n"), ParseStatus::kError);  // still errors
  parser.Reset();
  auto status = parser.Feed("*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(status, ParseStatus::kComplete);
  EXPECT_TRUE(parser.HasCommand());
}

// ===== Reply: Simple types =====
TEST(RespReplyTest, SimpleString) {
  EXPECT_EQ(RespReply::SimpleString("OK"), "+OK\r\n");
}

TEST(RespReplyTest, Error) {
  EXPECT_EQ(RespReply::Error("ERR msg"), "-ERR msg\r\n");
}

TEST(RespReplyTest, Integer) {
  EXPECT_EQ(RespReply::Integer(42), ":42\r\n");
  EXPECT_EQ(RespReply::Integer(-1), ":-1\r\n");
}

TEST(RespReplyTest, BulkString) {
  EXPECT_EQ(RespReply::BulkString("hello"), "$5\r\nhello\r\n");
}

TEST(RespReplyTest, NullBulkString) {
  EXPECT_EQ(RespReply::NullBulkString(), "$-1\r\n");
}

TEST(RespReplyTest, EmptyArray) {
  EXPECT_EQ(RespReply::EmptyArray(), "*0\r\n");
}

TEST(RespReplyTest, ArrayOfBulkStrings) {
  std::vector<std::string> v = {"a", "bb"};
  EXPECT_EQ(RespReply::ArrayOfBulkStrings(v), "*2\r\n$1\r\na\r\n$2\r\nbb\r\n");
}

// ===== Reply: convenience =====
TEST(RespReplyTest, Ok) { EXPECT_EQ(RespReply::Ok(), "+OK\r\n"); }

TEST(RespReplyTest, Nil) { EXPECT_EQ(RespReply::Nil(), "$-1\r\n"); }

}  // namespace
}  // namespace miniredis
