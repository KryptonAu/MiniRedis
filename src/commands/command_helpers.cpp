#include "commands/command_helpers.h"

#include "core/resp_protocol.h"
#include "core/server.h"
#include "types/numeric_parse.h"

namespace miniredis {

std::string WrongArity(std::string_view command) {
  return RespReply::Error("ERR wrong number of arguments for '" +
                          std::string(command) + "' command");
}

std::string SyntaxError() { return RespReply::Error("ERR syntax error"); }

std::string InvalidInteger() {
  return RespReply::Error("ERR value is not an integer or out of range");
}

std::string InvalidExpireTime() {
  return RespReply::Error("ERR invalid expire time");
}

std::string InvalidDbIndex() {
  return RespReply::Error("ERR DB index is out of range");
}

std::string NotImplemented(std::string_view command) {
  return RespReply::Error("ERR not implemented: " + std::string(command));
}

std::string Unsupported(std::string_view feature) {
  return RespReply::Error("ERR unsupported " + std::string(feature));
}

std::string TypeErrorToResp(TypeError error) {
  switch (error) {
    case TypeError::kInvalidInteger:
      return InvalidInteger();
    case TypeError::kIntegerOverflow:
      return RespReply::Error("ERR increment or decrement would overflow");
    case TypeError::kInvalidFloat:
      return RespReply::Error("ERR value is not a valid float");
    case TypeError::kInvalidScore:
      return RespReply::Error("ERR score is not a valid float");
    case TypeError::kOutOfRange:
      return RespReply::Error("ERR index out of range");
  }
  return RespReply::Error("ERR unknown error");
}

SetValue MakeSetValue(CommandContext& ctx) {
  return SetValue(ctx.server.GetEncodingThresholds());
}
HashValue MakeHashValue(CommandContext& ctx) {
  return HashValue(ctx.server.GetEncodingThresholds());
}
ZSetValue MakeZSetValue(CommandContext& ctx) {
  return ZSetValue(ctx.server.GetEncodingThresholds());
}

std::string ArrayOfZSetRange(const std::vector<ZSetValue::RangeResult>& values,
                             bool with_scores) {
  std::vector<std::string> elements;
  for (const auto& r : values) {
    elements.push_back(RespReply::BulkString(r.element));
    if (with_scores)
      elements.push_back(
          RespReply::BulkString(FormatDoubleForStorage(r.score)));
  }
  return RespReply::ArrayOfEncoded(elements);
}

std::string ScanReply(size_t next_cursor,
                      const std::vector<std::string>& elements) {
  std::vector<std::string> parts;
  parts.push_back(RespReply::BulkString(std::to_string(next_cursor)));
  parts.push_back(RespReply::ArrayOfBulkStrings(elements));
  return RespReply::ArrayOfEncoded(parts);
}

int64_t MsToSec(int64_t ms) { return (ms + 999) / 1000; }

}  // namespace miniredis
