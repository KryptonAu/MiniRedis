#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {

enum class ParseStatus {
  kComplete,
  kIncomplete,
  kError,
};

class RespParser {
 public:
  RespParser();

  ParseStatus Feed(std::string_view data);

  bool HasCommand() const;
  size_t PendingCommandCount() const;
  std::vector<std::string> TakeCommand();

  void Reset();
  std::optional<std::string_view> LastError() const;
  size_t BufferSize() const;

 private:
  std::vector<uint8_t> buffer_;
  std::deque<std::vector<std::string>> ready_commands_;
  std::string error_;

  ParseStatus ParseOneAt(size_t& pos);
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
