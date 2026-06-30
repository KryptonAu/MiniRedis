#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {

enum class ParseStatus {
  kComplete,
  kIncomplete,
  kError,
};

class RespCommand {
 public:
  RespCommand();

  std::span<const std::string_view> Args() const;
  size_t size() const;
  bool empty() const;
  std::string_view operator[](size_t index) const;
  std::vector<std::string> ToOwnedVector() const;

 private:
  friend class RespParser;

  struct ArgSpan {
    size_t offset = 0;
    size_t length = 0;
  };

  RespCommand(std::shared_ptr<const std::string> storage,
              std::vector<ArgSpan> spans);

  std::shared_ptr<const std::string> storage_;
  std::vector<std::string_view> args_;
};

class RespParser {
 public:
  RespParser();

  ParseStatus Feed(std::string_view data);

  bool HasCommand() const;
  size_t PendingCommandCount() const;
  RespCommand TakeCommand();

  void Reset();
  std::optional<std::string_view> LastError() const;
  size_t BufferSize() const;

 private:
  struct ParsedCommand {
    std::vector<RespCommand::ArgSpan> args;
  };

  std::string buffer_;
  std::deque<RespCommand> ready_commands_;
  std::string error_;

  ParseStatus ParseOneAt(size_t& pos, ParsedCommand& command);
  void CommitParsedCommands(std::vector<ParsedCommand> commands,
                            size_t consumed);
};

class RespReply {
 public:
  static std::string SimpleString(std::string_view msg);
  static std::string Error(std::string_view msg);
  static std::string Integer(int64_t n);
  static std::string BulkString(std::string_view data);
  static std::string NullBulkString();
  static std::string ArrayOfBulkStrings(
      const std::vector<std::string>& elements);
  static std::string ArrayOfEncoded(
      const std::vector<std::string>& encoded_elements);
  static std::string EmptyArray();

  static std::string Ok();
  static std::string Nil();
  static std::string WrongType();
  static std::string UnknownCommand(std::string_view cmd);
};

}  // namespace miniredis
