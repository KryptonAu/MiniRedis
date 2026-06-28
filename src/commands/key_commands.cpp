#include <chrono>
#include <limits>

#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/numeric_parse.h"

namespace miniredis {

static int64_t NowMs() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static bool CheckedMsFromSeconds(int64_t sec, int64_t& ms) {
  if (sec > std::numeric_limits<int64_t>::max() / 1000) return false;
  ms = sec * 1000;
  return true;
}

static bool CheckedExpireAtFromNow(int64_t duration_ms, int64_t& expire_at) {
  int64_t now = NowMs();
  if (duration_ms > std::numeric_limits<int64_t>::max() - now) return false;
  expire_at = now + duration_ms;
  return true;
}

static std::string DelCmd(CommandContext& ctx,
                          const std::vector<std::string>& args) {
  int deleted = 0;
  for (size_t i = 1; i < args.size(); i++) {
    if (ctx.db.Delete(args[i])) deleted++;
  }
  return RespReply::Integer(deleted);
}

static std::string ExistsCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  int count = 0;
  for (size_t i = 1; i < args.size(); i++) {
    if (ctx.db.Exists(args[i])) count++;
  }
  return RespReply::Integer(count);
}

static std::string TypeCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto t = ctx.db.Type(args[1]);
  if (!t) return RespReply::SimpleString("none");
  return RespReply::SimpleString(std::string(TypeName(*t)));
}

static std::string ExpireCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t sec = std::get<ParsedInt>(parsed).value;
  if (sec < 0) return InvalidExpireTime();
  int64_t duration_ms = 0;
  if (!CheckedMsFromSeconds(sec, duration_ms)) return InvalidExpireTime();
  int64_t expire_at = 0;
  if (!CheckedExpireAtFromNow(duration_ms, expire_at))
    return InvalidExpireTime();
  return RespReply::Integer(ctx.db.SetExpire(args[1], expire_at) ? 1 : 0);
}

static std::string PExpireCmd(CommandContext& ctx,
                              const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t ms = std::get<ParsedInt>(parsed).value;
  if (ms < 0) return InvalidExpireTime();
  int64_t expire_at = 0;
  if (!CheckedExpireAtFromNow(ms, expire_at)) return InvalidExpireTime();
  return RespReply::Integer(ctx.db.SetExpire(args[1], expire_at) ? 1 : 0);
}

static std::string ExpireAtCmd(CommandContext& ctx,
                               const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t unix_sec = std::get<ParsedInt>(parsed).value;
  if (unix_sec < 0) return InvalidExpireTime();
  int64_t expire_at = 0;
  if (!CheckedMsFromSeconds(unix_sec, expire_at)) return InvalidExpireTime();
  return RespReply::Integer(ctx.db.SetExpire(args[1], expire_at) ? 1 : 0);
}

static std::string PExpireAtCmd(CommandContext& ctx,
                                const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t unix_ms = std::get<ParsedInt>(parsed).value;
  if (unix_ms < 0) return InvalidExpireTime();
  return RespReply::Integer(ctx.db.SetExpire(args[1], unix_ms) ? 1 : 0);
}

static std::string TtlCmd(CommandContext& ctx,
                          const std::vector<std::string>& args) {
  int64_t ms = ctx.db.TTL(args[1]);
  if (ms < 0) return RespReply::Integer(ms);  // -2 or -1
  return RespReply::Integer(MsToSec(ms));
}

static std::string PTtlCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  return RespReply::Integer(ctx.db.TTL(args[1]));
}

static std::string PersistCmd(CommandContext& ctx,
                              const std::vector<std::string>& args) {
  return RespReply::Integer(ctx.db.Persist(args[1]) ? 1 : 0);
}

static std::string KeysCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  if (args[1] != "*") return Unsupported("KEYS pattern");
  auto keys = ctx.db.Keys("*");
  return RespReply::ArrayOfBulkStrings(keys);
}

static std::string RandomKeyCmd(CommandContext& ctx,
                                const std::vector<std::string>&) {
  auto key = ctx.db.RandomKey();
  if (!key) return RespReply::Nil();
  return RespReply::BulkString(*key);
}

static std::string RenameCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (!ctx.db.Rename(args[1], args[2]))
    return RespReply::Error("ERR no such key");
  return RespReply::Ok();
}

static std::string RenameNXCmd(CommandContext& ctx,
                               const std::vector<std::string>& args) {
  return RespReply::Integer(ctx.db.RenameNX(args[1], args[2]) ? 1 : 0);
}

void RegisterKeyCommands(CommandRegistry& registry) {
  registry.Register(
      {"DEL", -2, static_cast<uint32_t>(CommandFlag::kWrite), DelCmd});
  registry.Register(
      {"EXISTS", -2, static_cast<uint32_t>(CommandFlag::kReadOnly), ExistsCmd});
  registry.Register(
      {"TYPE", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), TypeCmd});
  registry.Register(
      {"EXPIRE", 3, static_cast<uint32_t>(CommandFlag::kWrite), ExpireCmd});
  registry.Register(
      {"PEXPIRE", 3, static_cast<uint32_t>(CommandFlag::kWrite), PExpireCmd});
  registry.Register(
      {"EXPIREAT", 3, static_cast<uint32_t>(CommandFlag::kWrite), ExpireAtCmd});
  registry.Register({"PEXPIREAT", 3, static_cast<uint32_t>(CommandFlag::kWrite),
                     PExpireAtCmd});
  registry.Register(
      {"TTL", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), TtlCmd});
  registry.Register(
      {"PTTL", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), PTtlCmd});
  registry.Register(
      {"PERSIST", 2, static_cast<uint32_t>(CommandFlag::kWrite), PersistCmd});
  registry.Register(
      {"KEYS", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), KeysCmd});
  registry.Register({"RANDOMKEY", 1,
                     static_cast<uint32_t>(CommandFlag::kReadOnly),
                     RandomKeyCmd});
  registry.Register(
      {"RENAME", 3, static_cast<uint32_t>(CommandFlag::kWrite), RenameCmd});
  registry.Register(
      {"RENAMENX", 3, static_cast<uint32_t>(CommandFlag::kWrite), RenameNXCmd});
}

}  // namespace miniredis
