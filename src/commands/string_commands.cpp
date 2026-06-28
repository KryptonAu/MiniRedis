#include <chrono>
#include <limits>

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

static std::string GetCmd(CommandContext& ctx,
                          const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) return RespReply::Nil();
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  return RespReply::BulkString(sv->ToString());
}

static std::string SetCmd(CommandContext& ctx,
                          const std::vector<std::string>& args) {
  ctx.db.Set(args[1], StringValue(args[2]));
  return RespReply::Ok();
}

static std::string SetNXCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  if (ctx.db.Exists(args[1])) return RespReply::Nil();
  ctx.db.Set(args[1], StringValue(args[2]));
  return RespReply::Integer(1);
}

static std::string SetExCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
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

static std::string PSetExCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
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

static std::string GetSetCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  std::string old;
  if (val) {
    auto* sv = std::get_if<StringValue>(val);
    if (!sv) return RespReply::WrongType();
    old = sv->ToString();
  }
  ctx.db.Set(args[1], StringValue(args[2]));
  if (val) return RespReply::BulkString(old);
  return RespReply::Nil();
}

static std::string GetDelCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) return RespReply::Nil();
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  std::string old = sv->ToString();
  ctx.db.Delete(args[1]);
  return RespReply::BulkString(old);
}

static std::string GetExCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) return RespReply::Nil();
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  return RespReply::BulkString(sv->ToString());
}

static std::string AppendCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(args[2]));
    return RespReply::Integer(static_cast<int64_t>(args[2].size()));
  }
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  sv->Append(args[2]);
  return RespReply::Integer(static_cast<int64_t>(sv->Length()));
}

static std::string StrlenCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) return RespReply::Integer(0);
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  return RespReply::Integer(static_cast<int64_t>(sv->Length()));
}

static std::string GetRangeCmd(CommandContext& ctx,
                               const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) return RespReply::BulkString("");
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  auto p1 = ParseCanonicalInt(args[2]);
  auto p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  return RespReply::BulkString(sv->GetRange(std::get<ParsedInt>(p1).value,
                                            std::get<ParsedInt>(p2).value));
}

static std::string SetRangeCmd(CommandContext& ctx,
                               const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t offset = std::get<ParsedInt>(parsed).value;
  if (offset < 0) return RespReply::Error("ERR offset is out of range");
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    StringValue sv("");
    sv.SetRange(static_cast<size_t>(offset), args[3]);
    ctx.db.Set(args[1], std::move(sv));
    auto* nv = ctx.db.Find(args[1]);
    auto* nsv = nv ? std::get_if<StringValue>(nv) : nullptr;
    return RespReply::Integer(static_cast<int64_t>(nsv ? nsv->Length() : 0));
  }
  auto* sv = std::get_if<StringValue>(val);
  if (!sv) return RespReply::WrongType();
  sv->SetRange(static_cast<size_t>(offset), args[3]);
  return RespReply::Integer(static_cast<int64_t>(sv->Length()));
}

static std::string IncrCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(0));
    val = ctx.db.Find(args[1]);
  }
  auto* existing = std::get_if<StringValue>(val);
  if (!existing) return RespReply::WrongType();
  auto result = existing->IncrementBy(1);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::Integer(std::get<int64_t>(result));
}

static std::string DecrCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(0));
    val = ctx.db.Find(args[1]);
  }
  auto* existing = std::get_if<StringValue>(val);
  if (!existing) return RespReply::WrongType();
  auto result = existing->IncrementBy(-1);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::Integer(std::get<int64_t>(result));
}

static std::string IncrByCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t delta = std::get<ParsedInt>(parsed).value;
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(0));
    val = ctx.db.Find(args[1]);
  }
  auto* existing = std::get_if<StringValue>(val);
  if (!existing) return RespReply::WrongType();
  auto result = existing->IncrementBy(delta);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::Integer(std::get<int64_t>(result));
}

static std::string DecrByCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  auto parsed = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  int64_t delta = -std::get<ParsedInt>(parsed).value;
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(0));
    val = ctx.db.Find(args[1]);
  }
  auto* existing = std::get_if<StringValue>(val);
  if (!existing) return RespReply::WrongType();
  auto result = existing->IncrementBy(delta);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::Integer(std::get<int64_t>(result));
}

static std::string IncrByFloatCmd(CommandContext& ctx,
                                  const std::vector<std::string>& args) {
  auto parsed = ParseFiniteDouble(args[2]);
  if (std::holds_alternative<TypeError>(parsed))
    return TypeErrorToResp(std::get<TypeError>(parsed));
  double delta = std::get<double>(parsed);
  auto* val = ctx.db.Find(args[1]);
  if (!val) {
    ctx.db.Set(args[1], StringValue(0));
    val = ctx.db.Find(args[1]);
  }
  auto* existing = std::get_if<StringValue>(val);
  if (!existing) return RespReply::WrongType();
  auto result = existing->IncrementByFloat(delta);
  if (std::holds_alternative<TypeError>(result))
    return TypeErrorToResp(std::get<TypeError>(result));
  return RespReply::BulkString(existing->ToString());
}

static std::string MGetCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  std::vector<std::string> result;
  for (size_t i = 1; i < args.size(); i++) {
    auto* val = ctx.db.Find(args[i]);
    if (!val || !std::holds_alternative<StringValue>(*val)) {
      result.push_back(RespReply::NullBulkString());
    } else {
      result.push_back(
          RespReply::BulkString(std::get<StringValue>(*val).ToString()));
    }
  }
  return RespReply::ArrayOfEncoded(result);
}

static std::string MSetCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  if ((args.size() - 1) % 2 != 0) return WrongArity("MSET");
  for (size_t i = 1; i + 1 < args.size(); i += 2)
    ctx.db.Set(args[i], StringValue(args[i + 1]));
  return RespReply::Ok();
}

static std::string MSetNXCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
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
