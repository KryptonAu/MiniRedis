#include "core/resp_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {
namespace {

static bool ViewInside(std::string_view view, std::string_view storage) {
  auto base = reinterpret_cast<uintptr_t>(storage.data());
  auto end = base + storage.size();
  auto view_base = reinterpret_cast<uintptr_t>(view.data());
  return view_base >= base && view_base + view.size() <= end;
}

// ===== Parser: basic single command =====
TEST(RespParserTest, ParseSimpleCommand) {
  RespParser parser;
  std::string input = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
  auto parsed = parser.ParseNext(input);
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  EXPECT_EQ(parsed.consumed, input.size());
  auto& cmd = parsed.command;
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[0], "GET");
  EXPECT_EQ(cmd[1], "key");
  EXPECT_TRUE(ViewInside(cmd[0], input));
  EXPECT_TRUE(ViewInside(cmd[1], input));
  EXPECT_EQ(cmd.ToOwnedVector(), std::vector<std::string>({"GET", "key"}));
}

TEST(RespParserTest, ParseSetCommand) {
  RespParser parser;
  std::string input = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
  auto parsed = parser.ParseNext(input);
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  EXPECT_EQ(parsed.consumed, input.size());
  auto& cmd = parsed.command;
  ASSERT_EQ(cmd.size(), 3);
  EXPECT_EQ(cmd[0], "SET");
  EXPECT_EQ(cmd[1], "key");
  EXPECT_EQ(cmd[2], "value");
  EXPECT_TRUE(ViewInside(cmd[0], input));
  EXPECT_TRUE(ViewInside(cmd[1], input));
  EXPECT_TRUE(ViewInside(cmd[2], input));
}

TEST(RespParserTest, EmptyBulkStringArgument) {
  RespParser parser;
  std::string input = "*2\r\n$3\r\nSET\r\n$0\r\n\r\n";
  auto parsed = parser.ParseNext(input);
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  auto& cmd = parsed.command;
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[1], "");
  EXPECT_TRUE(ViewInside(cmd[1], input));
}

// ===== Parser: partial reads =====
TEST(RespParserTest, PartialInputIncomplete) {
  RespParser parser;
  std::string partial = "*2\r\n$3\r\n";
  auto parsed = parser.ParseNext(partial);
  EXPECT_EQ(parsed.status, ParseStatus::kIncomplete);
  EXPECT_EQ(parsed.consumed, 0u);

  std::string complete = partial + "GET\r\n$3\r\nkey\r\n";
  parsed = parser.ParseNext(complete);
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  EXPECT_EQ(parsed.consumed, complete.size());
  ASSERT_EQ(parsed.command.size(), 2);
  EXPECT_EQ(parsed.command[0], "GET");
  EXPECT_EQ(parsed.command[1], "key");
}

// ===== Parser: pipelining =====
TEST(RespParserTest, PipeliningTwoCommands) {
  RespParser parser;
  std::string input = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";

  auto first = parser.ParseNext(input);
  ASSERT_EQ(first.status, ParseStatus::kComplete);
  ASSERT_EQ(first.command.size(), 1);
  EXPECT_EQ(first.command[0], "PING");

  auto second =
      parser.ParseNext(std::string_view(input).substr(first.consumed));
  ASSERT_EQ(second.status, ParseStatus::kComplete);
  ASSERT_EQ(second.command.size(), 1);
  EXPECT_EQ(second.command[0], "PING");
  EXPECT_EQ(first.consumed + second.consumed, input.size());
}

TEST(RespParserTest, TakenCommandSurvivesWhileOriginalStorageLives) {
  RespParser parser;
  std::string first_input = "*1\r\n$4\r\nPING\r\n";
  auto first_parse = parser.ParseNext(first_input);
  ASSERT_EQ(first_parse.status, ParseStatus::kComplete);
  RespCommand first = std::move(first_parse.command);

  std::string second_input = "*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n";
  auto second_parse = parser.ParseNext(second_input);
  ASSERT_EQ(second_parse.status, ParseStatus::kComplete);
  RespCommand second = std::move(second_parse.command);

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

  size_t offset = 0;
  for (int i = 0; i < kCommandCount; i++) {
    auto parsed = parser.ParseNext(std::string_view(input).substr(offset));
    ASSERT_EQ(parsed.status, ParseStatus::kComplete);
    RespCommand cmd = std::move(parsed.command);
    ASSERT_EQ(cmd.size(), 1);
    EXPECT_EQ(cmd[0], "PING");
    offset += parsed.consumed;
  }
  EXPECT_EQ(offset, input.size());
}

TEST(RespParserTest, LargeBulkStringCanArriveInPieces) {
  RespParser parser;
  std::string value(64 * 1024, 'x');
  std::string prefix =
      "*2\r\n$3\r\nSET\r\n$" + std::to_string(value.size()) + "\r\n";

  auto partial = parser.ParseNext(prefix);
  EXPECT_EQ(partial.status, ParseStatus::kIncomplete);
  EXPECT_EQ(partial.consumed, 0u);

  std::string complete = prefix + value + "\r\n";
  auto parsed = parser.ParseNext(complete);
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  EXPECT_EQ(parsed.consumed, complete.size());
  RespCommand cmd = std::move(parsed.command);
  ASSERT_EQ(cmd.size(), 2);
  EXPECT_EQ(cmd[0], "SET");
  EXPECT_EQ(cmd[1], value);
  EXPECT_TRUE(ViewInside(cmd[1], complete));
}

// ===== Parser: error cases =====
TEST(RespParserTest, EmptyArrayError) {
  RespParser parser;
  auto parsed = parser.ParseNext("*0\r\n");
  EXPECT_EQ(parsed.status, ParseStatus::kError);
  EXPECT_TRUE(parser.LastError().has_value());
}

TEST(RespParserTest, NullBulkInCommandError) {
  RespParser parser;
  auto parsed = parser.ParseNext("*2\r\n$-1\r\n\r\n$3\r\nfoo\r\n");
  EXPECT_EQ(parsed.status, ParseStatus::kError);
}

TEST(RespParserTest, NestedArrayError) {
  RespParser parser;
  auto parsed = parser.ParseNext("*2\r\n*1\r\n$4\r\nNEST\r\n\r\n$3\r\nfoo\r\n");
  EXPECT_EQ(parsed.status, ParseStatus::kError);
}

TEST(RespParserTest, ResetAfterError) {
  RespParser parser;
  parser.ParseNext("*0\r\n");  // error
  EXPECT_EQ(parser.ParseNext("*2\r\n").status,
            ParseStatus::kError);  // still errors
  parser.Reset();
  auto parsed = parser.ParseNext("*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(parsed.status, ParseStatus::kComplete);
  ASSERT_EQ(parsed.command.size(), 1);
  EXPECT_EQ(parsed.command[0], "PING");
}

// ===== Reply: Simple types =====
TEST(RespReplyTest, SimpleString) {
  EXPECT_EQ(RespReply::SimpleString("OK"), "+OK\r\n");
}

TEST(RespReplyTest, Error) {
  EXPECT_EQ(RespReply::Error("ERR msg"), "-ERR msg\r\n");
}

TEST(RespReplyTest, Integer) {
  EXPECT_EQ(RespReply::Integer(0), ":0\r\n");
  EXPECT_EQ(RespReply::Integer(42), ":42\r\n");
  EXPECT_EQ(RespReply::Integer(-1), ":-1\r\n");
  EXPECT_EQ(RespReply::Integer(std::numeric_limits<int64_t>::min()),
            ":-9223372036854775808\r\n");
  EXPECT_EQ(RespReply::Integer(std::numeric_limits<int64_t>::max()),
            ":9223372036854775807\r\n");
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

TEST(RespReplyTest, AppendHelpersMatchStringFactories) {
  std::string out;
  RespReply::AppendSimpleString(out, "OK");
  EXPECT_EQ(out, RespReply::SimpleString("OK"));

  out.clear();
  RespReply::AppendError(out, "ERR msg");
  EXPECT_EQ(out, RespReply::Error("ERR msg"));

  out.clear();
  RespReply::AppendInteger(out, -42);
  EXPECT_EQ(out, RespReply::Integer(-42));

  out.clear();
  RespReply::AppendBulkString(out, "hello");
  EXPECT_EQ(out, RespReply::BulkString("hello"));

  out.clear();
  RespReply::AppendNullBulkString(out);
  EXPECT_EQ(out, RespReply::NullBulkString());
}

TEST(RespReplyTest, AppendArrayHeaderAndEncoded) {
  std::string out;
  RespReply::AppendArrayHeader(out, 2);
  RespReply::AppendEncoded(out, RespReply::BulkString("a"));
  RespReply::AppendEncoded(out, RespReply::Integer(1));
  EXPECT_EQ(out, "*2\r\n$1\r\na\r\n:1\r\n");
}

TEST(RespReplyTest, BulkStringLargePayload) {
  std::string data(64 * 1024, 'x');
  std::string encoded = RespReply::BulkString(data);
  EXPECT_TRUE(encoded.starts_with("$65536\r\n"));
  EXPECT_TRUE(encoded.ends_with("\r\n"));
  EXPECT_EQ(encoded.size(), std::string("$65536\r\n").size() + data.size() + 2);
}

TEST(RespReplyTest, ArrayOfBulkStringViewsMatchesOwnedArray) {
  std::vector<std::string> owned;
  std::vector<std::string_view> views;
  owned.reserve(1000);
  views.reserve(1000);
  for (int i = 0; i < 1000; i++) {
    owned.push_back("value-" + std::to_string(i));
    views.push_back(owned.back());
  }

  EXPECT_EQ(RespReply::ArrayOfBulkStringViews(views),
            RespReply::ArrayOfBulkStrings(owned));
}

TEST(RespReplyTest, EncodedArraysSupportEmptyAndNestedValues) {
  EXPECT_EQ(RespReply::ArrayOfEncoded({}), "*0\r\n");

  std::vector<std::string> nested = {RespReply::BulkString("cursor"),
                                     RespReply::ArrayOfBulkStrings({"a"})};
  EXPECT_EQ(RespReply::ArrayOfEncoded(nested),
            "*2\r\n$6\r\ncursor\r\n*1\r\n$1\r\na\r\n");
}

// ===== Reply: convenience =====
TEST(RespReplyTest, Ok) { EXPECT_EQ(RespReply::Ok(), "+OK\r\n"); }

TEST(RespReplyTest, Nil) { EXPECT_EQ(RespReply::Nil(), "$-1\r\n"); }

}  // namespace
}  // namespace miniredis
