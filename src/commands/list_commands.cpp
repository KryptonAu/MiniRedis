#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/list_value.h"
#include "types/numeric_parse.h"

namespace miniredis {

using Flag = CommandFlag;

static ListValue* GetList(CommandContext& ctx, const std::string& key) {
  auto* val = ctx.db.Find(key);
  if (!val) return nullptr;
  return std::get_if<ListValue>(val);
}

static bool IsListWrongType(CommandContext& ctx, const std::string& key) {
  auto* val = ctx.db.Find(key);
  return val && !std::holds_alternative<ListValue>(*val);
}

static ListValue& GetOrCreateList(CommandContext& ctx, const std::string& key) {
  auto* val = ctx.db.Find(key);
  if (!val) {
    ctx.db.Set(key, ListValue{});
    return std::get<ListValue>(*ctx.db.Find(key));
  }
  auto* lv = std::get_if<ListValue>(val);
  if (!lv) return std::get<ListValue>(*val);
  return *lv;
}

static std::string LPushCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ListValue>(*val))
    return RespReply::WrongType();
  auto& lv = GetOrCreateList(ctx, args[1]);
  for (size_t i = 2; i < args.size(); i++) lv.PushHead(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv.Size()));
}

static std::string RPushCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ListValue>(*val))
    return RespReply::WrongType();
  auto& lv = GetOrCreateList(ctx, args[1]);
  for (size_t i = 2; i < args.size(); i++) lv.PushTail(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv.Size()));
}

static std::string LPushXCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Integer(0);
  for (size_t i = 2; i < args.size(); i++) lv->PushHead(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv->Size()));
}

static std::string RPushXCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Integer(0);
  for (size_t i = 2; i < args.size(); i++) lv->PushTail(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv->Size()));
}

static std::string LPopCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ListValue>(*val))
    return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Nil();
  auto r = lv->PopHead();
  if (!r) return RespReply::Nil();
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::BulkString(*r);
}

static std::string RPopCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val_rp = ctx.db.Find(args[1]);
  if (val_rp && !std::holds_alternative<ListValue>(*val_rp))
    return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Nil();
  auto r = lv->PopTail();
  if (!r) return RespReply::Nil();
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::BulkString(*r);
}

static std::string LLenCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ListValue>(*val))
    return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  return RespReply::Integer(lv ? static_cast<int64_t>(lv->Size()) : 0);
}

static std::string LIndexCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Nil();
  auto p = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(p)) return InvalidInteger();
  auto v = lv->Get(std::get<ParsedInt>(p).value);
  return v ? RespReply::BulkString(*v) : RespReply::Nil();
}

static std::string LRangeCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::EmptyArray();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  return RespReply::ArrayOfBulkStrings(
      lv->Range(std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value));
}

static std::string LTrimCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Ok();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  lv->Trim(std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value);
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::Ok();
}

static std::string LRemCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* lv = GetList(ctx, args[1]);
  if (!lv) return RespReply::Integer(0);
  auto p = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(p)) return InvalidInteger();
  auto r = lv->Remove(std::get<ParsedInt>(p).value, args[3]);
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::Integer(static_cast<int64_t>(r));
}

static std::string RPopLPushCmd(CommandContext& ctx,
                                const std::vector<std::string>& args) {
  if (IsListWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* src = GetList(ctx, args[1]);
  if (!src) return RespReply::Nil();

  // Check destination type before mutating source
  auto* dst_check = ctx.db.Find(args[2]);
  if (dst_check && !std::holds_alternative<ListValue>(*dst_check))
    return RespReply::WrongType();

  auto v = src->PopTail();
  if (!v) return RespReply::Nil();
  if (src->Empty()) ctx.db.Delete(args[1]);
  if (!dst_check) {
    ctx.db.Set(args[2], ListValue{});
    dst_check = ctx.db.Find(args[2]);
  }
  std::get<ListValue>(*dst_check).PushHead(*v);
  return RespReply::BulkString(*v);
}
void RegisterListCommands(CommandRegistry& registry) {
  auto ro = static_cast<uint32_t>(Flag::kReadOnly);
  auto wr = static_cast<uint32_t>(Flag::kWrite);
  registry.Register({"LPUSH", -3, wr, LPushCmd});
  registry.Register({"RPUSH", -3, wr, RPushCmd});
  registry.Register({"LPUSHX", -3, wr, LPushXCmd});
  registry.Register({"RPUSHX", -3, wr, RPushXCmd});
  registry.Register({"LPOP", 2, wr, LPopCmd});
  registry.Register({"RPOP", 2, wr, RPopCmd});
  registry.Register({"LLEN", 2, ro, LLenCmd});
  registry.Register({"LINDEX", 3, ro, LIndexCmd});
  registry.Register({"LRANGE", 4, ro, LRangeCmd});
  registry.Register({"LTRIM", 4, wr, LTrimCmd});
  registry.Register({"LREM", 4, wr, LRemCmd});
  registry.Register({"RPOPLPUSH", 3, wr, RPopLPushCmd});
}

}  // namespace miniredis
