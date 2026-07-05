#include <chrono>
#include <limits>
#include <utility>

#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/numeric_parse.h"
#include "types/string_value.h"
#include "types/value.h"

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

static StringValue& GetOrCreateString(CommandContext& ctx, std::string_view key,
                                      TypedKeyLookup<StringValue>& lookup,
                                      StringValue initial) {
  return GetOrCreateValueAs<StringValue>(
      ctx.db, key, lookup, [&initial]() mutable { return std::move(initial); });
}

static std::string IncrementStringBy(CommandContext& ctx, std::string_view key,
                                     int64_t delta) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, key);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& existing = GetOrCreateString(ctx, key, lookup, StringValue(0));
  auto result = existing.IncrementBy(delta);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::Integer(std::get<int64_t>(result));
}

static std::string GetCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.Missing()) return RespReply::Nil();
  if (lookup.WrongType()) return RespReply::WrongType();
  StringValue::StringViewScratch scratch;
  return RespReply::BulkString(lookup.value->ToStringView(scratch));
}

static std::string SetCmd(CommandContext& ctx, CommandArgs args) {
  ctx.db.Set(args[1], StringValue(args[2]));
  return RespReply::Ok();
}

static std::string SetNXCmd(CommandContext& ctx, CommandArgs args) {
  if (ctx.db.Exists(args[1])) return RespReply::Nil();
  ctx.db.Set(args[1], StringValue(args[2]));
  return RespReply::Integer(1);
}

static std::string SetExCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t sec = std::get<ParsedInt>(parsed).value;
  if (sec <= 0) return InvalidExpireTime();
  int64_t duration_ms = 0;
  if (!CheckedMsFromSeconds(sec, duration_ms)) return InvalidExpireTime();
  int64_t expire_at = 0;
  if (!CheckedExpireAtFromNow(duration_ms, expire_at))
    return InvalidExpireTime();
  ctx.db.Set(args[1], StringValue(args[3]));
  ctx.db.SetExpire(args[1], expire_at);
  return RespReply::Ok();
}

static std::string PSetExCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t ms = std::get<ParsedInt>(parsed).value;
  if (ms <= 0) return InvalidExpireTime();
  int64_t expire_at = 0;
  if (!CheckedExpireAtFromNow(ms, expire_at)) return InvalidExpireTime();
  ctx.db.Set(args[1], StringValue(args[3]));
  ctx.db.SetExpire(args[1], expire_at);
  return RespReply::Ok();
}

static std::string GetSetCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  std::string old;
  if (!lookup.Missing()) {
    if (lookup.WrongType()) return RespReply::WrongType();
    old = lookup.value->ToString();
  }
  ctx.db.Set(args[1], StringValue(args[2]));
  if (!lookup.Missing()) return RespReply::BulkString(old);
  return RespReply::Nil();
}

static std::string GetDelCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.Missing()) return RespReply::Nil();
  if (lookup.WrongType()) return RespReply::WrongType();
  std::string old = lookup.value->ToString();
  ctx.db.Delete(args[1]);
  return RespReply::BulkString(old);
}

static std::string GetExCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.Missing()) return RespReply::Nil();
  if (lookup.WrongType()) return RespReply::WrongType();
  StringValue::StringViewScratch scratch;
  return RespReply::BulkString(lookup.value->ToStringView(scratch));
}

static std::string AppendCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  if (lookup.Missing()) {
    GetOrCreateString(ctx, args[1], lookup, StringValue(args[2]));
    return RespReply::Integer(static_cast<int64_t>(args[2].size()));
  }
  lookup.value->Append(args[2]);
  return RespReply::Integer(static_cast<int64_t>(lookup.value->Length()));
}

static std::string StrlenCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.Missing()) return RespReply::Integer(0);
  if (lookup.WrongType()) return RespReply::WrongType();
  return RespReply::Integer(static_cast<int64_t>(lookup.value->Length()));
}

static std::string GetRangeCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.Missing()) return RespReply::BulkString("");
  if (lookup.WrongType()) return RespReply::WrongType();
  auto p1 = ParseCanonicalInt(args[2]);
  auto p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  return RespReply::BulkString(lookup.value->GetRange(
      std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value));
}

static std::string SetRangeCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t offset = std::get<ParsedInt>(parsed).value;
  if (offset < 0) return RespReply::Error("ERR offset is out of range");
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& sv = GetOrCreateString(ctx, args[1], lookup, StringValue(""));
  sv.SetRange(static_cast<size_t>(offset), args[3]);
  return RespReply::Integer(static_cast<int64_t>(sv.Length()));
}

static std::string IncrCmd(CommandContext& ctx, CommandArgs args) {
  return IncrementStringBy(ctx, args[1], 1);
}

static std::string DecrCmd(CommandContext& ctx, CommandArgs args) {
  return IncrementStringBy(ctx, args[1], -1);
}

static std::string IncrByCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t delta = std::get<ParsedInt>(parsed).value;
  return IncrementStringBy(ctx, args[1], delta);
}

static std::string DecrByCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t delta = -std::get<ParsedInt>(parsed).value;
  return IncrementStringBy(ctx, args[1], delta);
}

static std::string IncrByFloatCmd(CommandContext& ctx, CommandArgs args) {
  auto parsed = ParseFiniteDouble(args[2]);
  if (std::holds_alternative<TypeError>(parsed))
    return TypeErrorToResp(std::get<TypeError>(parsed));
  double delta = std::get<double>(parsed);
  auto lookup = LookupKeyAs<StringValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& existing = GetOrCreateString(ctx, args[1], lookup, StringValue(0));
  auto result = existing.IncrementByFloat(delta);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  StringValue::StringViewScratch scratch;
  return RespReply::BulkString(existing.ToStringView(scratch));
}

static std::string MGetCmd(CommandContext& ctx, CommandArgs args) {
  std::string result;
  RespReply::AppendArrayHeader(result, args.size() - 1);
  for (size_t i = 1; i < args.size(); i++) {
    auto lookup = LookupKeyAs<StringValue>(ctx.db, args[i]);
    if (!lookup.value) {
      RespReply::AppendNullBulkString(result);
    } else {
      StringValue::StringViewScratch scratch;
      RespReply::AppendBulkString(result, lookup.value->ToStringView(scratch));
    }
  }
  return result;
}

static std::string MSetCmd(CommandContext& ctx, CommandArgs args) {
  if ((args.size() - 1) % 2 != 0) return WrongArity("MSET");
  for (size_t i = 1; i + 1 < args.size(); i += 2)
    ctx.db.Set(args[i], StringValue(args[i + 1]));
  return RespReply::Ok();
}

static std::string MSetNXCmd(CommandContext& ctx, CommandArgs args) {
  if ((args.size() - 1) % 2 != 0) return WrongArity("MSETNX");
  for (size_t i = 1; i + 1 < args.size(); i += 2)
    if (ctx.db.Exists(args[i])) return RespReply::Integer(0);
  for (size_t i = 1; i + 1 < args.size(); i += 2) {
    ctx.db.Set(args[i], StringValue(args[i + 1]));
  }
  return RespReply::Integer(1);
}

void RegisterStringCommands(CommandRegistry& registry) {
  registry.Register(
      {"GET", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), GetCmd});
  registry.Register(
      {"SET", 3, static_cast<uint32_t>(CommandFlag::kWrite), SetCmd});
  registry.Register(
      {"SETNX", 3, static_cast<uint32_t>(CommandFlag::kWrite), SetNXCmd});
  registry.Register(
      {"SETEX", 4, static_cast<uint32_t>(CommandFlag::kWrite), SetExCmd});
  registry.Register(
      {"PSETEX", 4, static_cast<uint32_t>(CommandFlag::kWrite), PSetExCmd});
  registry.Register(
      {"GETSET", 3, static_cast<uint32_t>(CommandFlag::kWrite), GetSetCmd});
  registry.Register(
      {"GETDEL", 2, static_cast<uint32_t>(CommandFlag::kWrite), GetDelCmd});
  registry.Register(
      {"GETEX", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), GetExCmd});
  registry.Register(
      {"APPEND", 3, static_cast<uint32_t>(CommandFlag::kWrite), AppendCmd});
  registry.Register(
      {"STRLEN", 2, static_cast<uint32_t>(CommandFlag::kReadOnly), StrlenCmd});
  registry.Register({"GETRANGE", 4,
                     static_cast<uint32_t>(CommandFlag::kReadOnly),
                     GetRangeCmd});
  registry.Register(
      {"SETRANGE", 4, static_cast<uint32_t>(CommandFlag::kWrite), SetRangeCmd});
  registry.Register(
      {"INCR", 2, static_cast<uint32_t>(CommandFlag::kWrite), IncrCmd});
  registry.Register(
      {"DECR", 2, static_cast<uint32_t>(CommandFlag::kWrite), DecrCmd});
  registry.Register(
      {"INCRBY", 3, static_cast<uint32_t>(CommandFlag::kWrite), IncrByCmd});
  registry.Register(
      {"DECRBY", 3, static_cast<uint32_t>(CommandFlag::kWrite), DecrByCmd});
  registry.Register({"INCRBYFLOAT", 3,
                     static_cast<uint32_t>(CommandFlag::kWrite),
                     IncrByFloatCmd});
  registry.Register(
      {"MGET", -2, static_cast<uint32_t>(CommandFlag::kReadOnly), MGetCmd});
  registry.Register(
      {"MSET", -3, static_cast<uint32_t>(CommandFlag::kWrite), MSetCmd});
  registry.Register(
      {"MSETNX", -3, static_cast<uint32_t>(CommandFlag::kWrite), MSetNXCmd});
}

}  // namespace miniredis
