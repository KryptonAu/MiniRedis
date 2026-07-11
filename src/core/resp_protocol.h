#pragma once

#include <array>
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

// Reusable, client-owned storage for parsed argument descriptors. The bulk
// string bytes remain in QueryBuffer; this object stores only their views.
class CommandArgStorage {
 public:
  static constexpr size_t kInlineCapacity = 8;
  static constexpr size_t kMaxRetainedHeapArgs = 64;

  void Begin(size_t expected_count);
  void Append(std::string_view arg);
  std::span<const std::string_view> Args() const;

  // Call only after the command using Args() has completed.
  void ReleaseOversizedHeap();

 private:
  std::array<std::string_view, kInlineCapacity> inline_args_;
  std::vector<std::string_view> heap_args_;
  size_t inline_size_ = 0;
  bool using_heap_ = false;
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

  explicit RespCommand(std::span<const std::string_view> args);

  // The Client-owned CommandArgStorage and QueryBuffer must remain stable
  // until command execution finishes. Args must not escape that boundary.
  std::span<const std::string_view> args_;
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
  // RespCommand views both `readable` and `args`, which must remain stable
  // until command execution finishes.
  RespParseResult ParseNext(std::string_view readable, CommandArgStorage& args);

  void Reset();
  std::optional<std::string_view> LastError() const;

 private:
  std::string error_;

  ParseStatus ParseOneAt(std::string_view readable, size_t& pos,
                         CommandArgStorage& args);
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
