#pragma once

#include <cstddef>
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

  explicit RespCommand(std::vector<std::string_view> args);

  std::vector<std::string_view> args_;
};

struct RespParseResult {
  ParseStatus status = ParseStatus::kIncomplete;
  RespCommand command;
  size_t consumed = 0;
};

class RespParser {
 public:
  RespParser();

  // Parses one command in-place from caller-owned storage. A completed
  // RespCommand contains string_views into `readable`, so the caller must keep
  // that storage stable until command execution finishes.
  RespParseResult ParseNext(std::string_view readable);

  void Reset();
  std::optional<std::string_view> LastError() const;

 private:
  struct ParsedCommand {
    std::vector<std::string_view> args;
  };

  std::string error_;

  ParseStatus ParseOneAt(std::string_view readable, size_t& pos,
                         ParsedCommand& command);
};

class RespReply {
 public:
  static void AppendSimpleString(std::string& out, std::string_view msg);
  static void AppendError(std::string& out, std::string_view msg);
  static void AppendInteger(std::string& out, int64_t n);
  static void AppendBulkString(std::string& out, std::string_view data);
  static void AppendNullBulkString(std::string& out);
  static void AppendArrayHeader(std::string& out, size_t count);
  static void AppendEncoded(std::string& out, std::string_view encoded);

  static std::string SimpleString(std::string_view msg);
  static std::string Error(std::string_view msg);
  static std::string Integer(int64_t n);
  static std::string BulkString(std::string_view data);
  static std::string NullBulkString();
  static std::string ArrayOfBulkStrings(
      const std::vector<std::string>& elements);
  static std::string ArrayOfBulkStringViews(
      std::span<const std::string_view> elements);
  static std::string ArrayOfEncoded(
      const std::vector<std::string>& encoded_elements);
  static std::string EmptyArray();

  static std::string Ok();
  static std::string Nil();
  static std::string WrongType();
  static std::string UnknownCommand(std::string_view cmd);
};

}  // namespace miniredis
