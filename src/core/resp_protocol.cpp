#include "core/resp_protocol.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <utility>

namespace miniredis {

namespace {

size_t DecimalDigits(size_t value) {
  size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    digits++;
  }
  return digits;
}

size_t BulkStringEncodedSize(size_t data_size) {
  return 1 + DecimalDigits(data_size) + 2 + data_size + 2;
}

size_t ArrayHeaderEncodedSize(size_t count) {
  return 1 + DecimalDigits(count) + 2;
}

void AppendSize(std::string& out, size_t value) {
  char buf[32];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  (void)ec;
  out.append(buf, ptr);
}

void AppendInt64(std::string& out, int64_t value) {
  char buf[32];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  (void)ec;
  out.append(buf, ptr);
}

}  // namespace

// ===== RespCommand =====

RespCommand::RespCommand() = default;

RespCommand::RespCommand(std::vector<std::string_view> args)
    : args_(std::move(args)) {}

std::span<const std::string_view> RespCommand::Args() const { return args_; }

size_t RespCommand::size() const { return args_.size(); }

bool RespCommand::empty() const { return args_.empty(); }

std::string_view RespCommand::operator[](size_t index) const {
  return args_[index];
}

std::vector<std::string> RespCommand::ToOwnedVector() const {
  std::vector<std::string> result;
  result.reserve(args_.size());
  for (std::string_view arg : args_) {
    result.emplace_back(arg);
  }
  return result;
}

// ===== RespParser =====

RespParser::RespParser() = default;

RespParseResult RespParser::ParseNext(std::string_view readable) {
  RespParseResult result;
  if (!error_.empty()) {
    result.status = ParseStatus::kError;
    return result;
  }
  if (readable.empty()) return result;

  size_t pos = 0;
  ParsedCommand parsed;
  result.status = ParseOneAt(readable, pos, parsed);
  if (result.status == ParseStatus::kComplete) {
    result.consumed = pos;
    result.command = RespCommand(std::move(parsed.args));
  }

  return result;
}

void RespParser::Reset() { error_.clear(); }

std::optional<std::string_view> RespParser::LastError() const {
  if (error_.empty()) return std::nullopt;
  return error_;
}

ParseStatus RespParser::ParseOneAt(std::string_view readable, size_t& pos,
                                   ParsedCommand& command) {
  size_t start = pos;
  if (pos >= readable.size()) {
    pos = start;
    return ParseStatus::kIncomplete;
  }
  if (readable[pos] != '*') {
    error_ = "Expected '*' for array, got " +
             std::to_string(static_cast<unsigned char>(readable[pos]));
    return ParseStatus::kError;
  }
  pos++;

  // Parse array length (with overflow check)
  static constexpr int64_t kMaxArrayLen = 1024 * 1024;
  int64_t array_len = 0;
  bool negative = false;
  while (pos < readable.size() && readable[pos] != '\r') {
    char c = static_cast<char>(readable[pos]);
    if (c == '-') {
      negative = true;
      pos++;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      error_ = "Invalid array length character";
      return ParseStatus::kError;
    }
    if (array_len > kMaxArrayLen / 10) {
      error_ = "Array length overflow";
      return ParseStatus::kError;
    }
    array_len = array_len * 10 + (c - '0');
    if (array_len > kMaxArrayLen) {
      error_ = "Array length too large";
      return ParseStatus::kError;
    }
    pos++;
  }
  if (pos + 1 >= readable.size() || readable[pos] != '\r' ||
      readable[pos + 1] != '\n') {
    pos = start;
    return ParseStatus::kIncomplete;
  }
  pos += 2;

  if (negative || array_len < 0) {
    error_ = "Null array not allowed in request";
    return ParseStatus::kError;
  }
  if (array_len == 0) {
    error_ = "Empty array not allowed in request";
    return ParseStatus::kError;
  }

  // Parse array elements
  command.args.reserve(static_cast<size_t>(array_len));
  for (int64_t i = 0; i < array_len; i++) {
    if (pos >= readable.size()) {
      pos = start;
      return ParseStatus::kIncomplete;
    }

    // Must be bulk string ($)
    if (readable[pos] != '$') {
      error_ = "Expected '$' for bulk string";
      return ParseStatus::kError;
    }
    pos++;

    // Parse bulk length (with overflow check)
    static constexpr int64_t kMaxBulkLen = 512LL * 1024 * 1024;  // 512MB
    int64_t bulk_len = 0;
    bool bulk_null = false;
    while (pos < readable.size() && readable[pos] != '\r') {
      char c = static_cast<char>(readable[pos]);
      if (c == '-') {
        bulk_null = true;
        pos++;
        continue;
      }
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        error_ = "Invalid bulk string length";
        return ParseStatus::kError;
      }
      if (bulk_len > kMaxBulkLen / 10) {
        error_ = "Bulk string length overflow";
        return ParseStatus::kError;
      }
      bulk_len = bulk_len * 10 + (c - '0');
      if (bulk_len > kMaxBulkLen) {
        error_ = "Bulk string length too large";
        return ParseStatus::kError;
      }
      pos++;
    }
    if (pos + 1 >= readable.size() || readable[pos] != '\r' ||
        readable[pos + 1] != '\n') {
      pos = start;
      return ParseStatus::kIncomplete;
    }
    pos += 2;

    if (bulk_null) {
      error_ = "Null bulk string not allowed in command";
      return ParseStatus::kError;
    }

    // Read bulk data
    if (pos + static_cast<size_t>(bulk_len) + 2 > readable.size()) {
      pos = start;
      return ParseStatus::kIncomplete;
    }
    command.args.emplace_back(readable.data() + pos,
                              static_cast<size_t>(bulk_len));
    pos += static_cast<size_t>(bulk_len);
    if (readable[pos] != '\r' || readable[pos + 1] != '\n') {
      error_ = "Missing CRLF after bulk data";
      return ParseStatus::kError;
    }
    pos += 2;
  }

  return ParseStatus::kComplete;
}

// ===== RespReply =====

void RespReply::AppendSimpleString(std::string& out, std::string_view msg) {
  out += '+';
  out.append(msg);
  out += "\r\n";
}

void RespReply::AppendError(std::string& out, std::string_view msg) {
  out += '-';
  out.append(msg);
  out += "\r\n";
}

void RespReply::AppendInteger(std::string& out, int64_t n) {
  out += ':';
  AppendInt64(out, n);
  out += "\r\n";
}

void RespReply::AppendBulkString(std::string& out, std::string_view data) {
  out += '$';
  AppendSize(out, data.size());
  out += "\r\n";
  out.append(data);
  out += "\r\n";
}

void RespReply::AppendNullBulkString(std::string& out) { out += "$-1\r\n"; }

void RespReply::AppendArrayHeader(std::string& out, size_t count) {
  out += '*';
  AppendSize(out, count);
  out += "\r\n";
}

void RespReply::AppendEncoded(std::string& out, std::string_view encoded) {
  out.append(encoded);
}

std::string RespReply::SimpleString(std::string_view msg) {
  std::string result;
  result.reserve(3 + msg.size());
  AppendSimpleString(result, msg);
  return result;
}

std::string RespReply::Error(std::string_view msg) {
  std::string result;
  result.reserve(3 + msg.size());
  AppendError(result, msg);
  return result;
}

std::string RespReply::Integer(int64_t n) {
  std::string result;
  result.reserve(24);
  AppendInteger(result, n);
  return result;
}

std::string RespReply::BulkString(std::string_view data) {
  std::string result;
  result.reserve(BulkStringEncodedSize(data.size()));
  AppendBulkString(result, data);
  return result;
}

std::string RespReply::NullBulkString() { return "$-1\r\n"; }

std::string RespReply::ArrayOfBulkStrings(
    const std::vector<std::string>& elements) {
  std::string result;
  size_t total_size = ArrayHeaderEncodedSize(elements.size());
  for (const auto& e : elements) {
    total_size += BulkStringEncodedSize(e.size());
  }
  result.reserve(total_size);
  AppendArrayHeader(result, elements.size());
  for (const auto& e : elements) {
    AppendBulkString(result, e);
  }
  return result;
}

std::string RespReply::ArrayOfBulkStringViews(
    std::span<const std::string_view> elements) {
  std::string result;
  size_t total_size = ArrayHeaderEncodedSize(elements.size());
  for (std::string_view e : elements) {
    total_size += BulkStringEncodedSize(e.size());
  }
  result.reserve(total_size);
  AppendArrayHeader(result, elements.size());
  for (std::string_view e : elements) {
    AppendBulkString(result, e);
  }
  return result;
}

std::string RespReply::ArrayOfEncoded(
    const std::vector<std::string>& encoded_elements) {
  std::string result;
  size_t total_size = ArrayHeaderEncodedSize(encoded_elements.size());
  for (const auto& e : encoded_elements) {
    total_size += e.size();
  }
  result.reserve(total_size);
  AppendArrayHeader(result, encoded_elements.size());
  for (const auto& e : encoded_elements) {
    AppendEncoded(result, e);
  }
  return result;
}

std::string RespReply::EmptyArray() { return "*0\r\n"; }

std::string RespReply::Ok() { return "+OK\r\n"; }

std::string RespReply::Nil() { return NullBulkString(); }

std::string RespReply::WrongType() {
  return RespReply::Error(
      "WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string RespReply::UnknownCommand(std::string_view cmd) {
  return Error("ERR unknown command '" + std::string(cmd) + "'");
}

}  // namespace miniredis
