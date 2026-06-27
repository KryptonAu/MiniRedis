#include "core/resp_protocol.h"

#include <cctype>
#include <cstdlib>

namespace miniredis {

// ===== RespParser =====

RespParser::RespParser() = default;

ParseStatus RespParser::Feed(std::string_view data) {
  if (!error_.empty()) return ParseStatus::kError;

  buffer_.insert(buffer_.end(), data.begin(), data.end());
  size_t pos = 0;
  bool any_complete = false;

  while (pos < buffer_.size()) {
    auto status = ParseOneAt(pos);
    if (status == ParseStatus::kError) return ParseStatus::kError;
    if (status == ParseStatus::kIncomplete) break;
    any_complete = true;
  }

  // Remove consumed bytes
  if (pos > 0)
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(pos));

  return any_complete ? ParseStatus::kComplete : ParseStatus::kIncomplete;
}

bool RespParser::HasCommand() const { return !ready_commands_.empty(); }

size_t RespParser::PendingCommandCount() const {
  return ready_commands_.size();
}

std::vector<std::string> RespParser::TakeCommand() {
  if (ready_commands_.empty()) return {};
  auto cmd = std::move(ready_commands_.front());
  ready_commands_.pop_front();
  return cmd;
}

void RespParser::Reset() {
  buffer_.clear();
  ready_commands_.clear();
  error_.clear();
}

std::optional<std::string_view> RespParser::LastError() const {
  if (error_.empty()) return std::nullopt;
  return error_;
}

size_t RespParser::BufferSize() const { return buffer_.size(); }

ParseStatus RespParser::ParseOneAt(size_t& pos) {
  size_t start = pos;
  if (pos >= buffer_.size()) {
    pos = start;
    return ParseStatus::kIncomplete;
  }
  if (buffer_[pos] != '*') {
    error_ = "Expected '*' for array, got " + std::to_string(buffer_[pos]);
    return ParseStatus::kError;
  }
  pos++;

  // Parse array length (with overflow check)
  static constexpr int64_t kMaxArrayLen = 1024 * 1024;
  int64_t array_len = 0;
  bool negative = false;
  while (pos < buffer_.size() && buffer_[pos] != '\r') {
    char c = static_cast<char>(buffer_[pos]);
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
  if (pos >= buffer_.size() + 1 || pos + 1 >= buffer_.size() ||
      buffer_[pos] != '\r' || buffer_[pos + 1] != '\n') {
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
  std::vector<std::string> args;
  for (int64_t i = 0; i < array_len; i++) {
    if (pos >= buffer_.size()) {
      pos = start;
      return ParseStatus::kIncomplete;
    }

    // Must be bulk string ($)
    if (buffer_[pos] != '$') {
      error_ = "Expected '$' for bulk string";
      return ParseStatus::kError;
    }
    pos++;

    // Parse bulk length (with overflow check)
    static constexpr int64_t kMaxBulkLen = 512LL * 1024 * 1024;  // 512MB
    int64_t bulk_len = 0;
    bool bulk_null = false;
    while (pos < buffer_.size() && buffer_[pos] != '\r') {
      char c = static_cast<char>(buffer_[pos]);
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
    if (pos + 1 >= buffer_.size() || buffer_[pos] != '\r' ||
        buffer_[pos + 1] != '\n') {
      pos = start;
      return ParseStatus::kIncomplete;
    }
    pos += 2;

    if (bulk_null) {
      error_ = "Null bulk string not allowed in command";
      return ParseStatus::kError;
    }

    // Read bulk data
    if (pos + static_cast<size_t>(bulk_len) + 2 > buffer_.size()) {
      pos = start;
      return ParseStatus::kIncomplete;
    }
    args.emplace_back(reinterpret_cast<const char*>(buffer_.data() + pos),
                      static_cast<size_t>(bulk_len));
    pos += static_cast<size_t>(bulk_len);
    if (buffer_[pos] != '\r' || buffer_[pos + 1] != '\n') {
      error_ = "Missing CRLF after bulk data";
      return ParseStatus::kError;
    }
    pos += 2;
  }

  ready_commands_.push_back(std::move(args));
  return ParseStatus::kComplete;
}

// ===== RespReply =====

std::string RespReply::SimpleString(std::string_view msg) {
  std::string result;
  result.reserve(3 + msg.size());
  result += '+';
  result.append(msg);
  result += "\r\n";
  return result;
}

std::string RespReply::Error(std::string_view msg) {
  std::string result;
  result.reserve(3 + msg.size());
  result += '-';
  result.append(msg);
  result += "\r\n";
  return result;
}

std::string RespReply::Integer(int64_t n) {
  return ":" + std::to_string(n) + "\r\n";
}

std::string RespReply::BulkString(std::string_view data) {
  std::string result;
  result.reserve(5 + std::to_string(data.size()).size() + data.size());
  result += '$';
  result += std::to_string(data.size());
  result += "\r\n";
  result.append(data);
  result += "\r\n";
  return result;
}

std::string RespReply::NullBulkString() { return "$-1\r\n"; }

std::string RespReply::ArrayOfBulkStrings(
    const std::vector<std::string>& elements) {
  std::string result;
  result += '*';
  result += std::to_string(elements.size());
  result += "\r\n";
  for (const auto& e : elements) {
    result += BulkString(e);
  }
  return result;
}

std::string RespReply::ArrayOfEncoded(
    const std::vector<std::string>& encoded_elements) {
  std::string result;
  result += '*';
  result += std::to_string(encoded_elements.size());
  result += "\r\n";
  for (const auto& e : encoded_elements) {
    result += e;
  }
  return result;
}

std::string RespReply::EmptyArray() { return "*0\r\n"; }

std::string RespReply::Ok() { return SimpleString("OK"); }

std::string RespReply::Nil() { return NullBulkString(); }

std::string RespReply::WrongType() {
  return Error(
      "WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string RespReply::UnknownCommand(std::string_view cmd) {
  return Error("ERR unknown command '" + std::string(cmd) + "'");
}

}  // namespace miniredis
