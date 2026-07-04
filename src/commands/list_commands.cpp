#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/list_value.h"
#include "types/numeric_parse.h"

namespace miniredis {

using Flag = CommandFlag;

static ListValue& GetOrCreateList(CommandContext& ctx, std::string_view key,
                                  TypedKeyLookup<ListValue>& lookup) {
  return GetOrCreateValueAs<ListValue>(ctx.db, key, lookup,
                                       [] { return ListValue{}; });
}

static std::string LPushCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& lv = GetOrCreateList(ctx, args[1], lookup);
  for (size_t i = 2; i < args.size(); i++) lv.PushHead(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv.Size()));
}

static std::string RPushCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& lv = GetOrCreateList(ctx, args[1], lookup);
  for (size_t i = 2; i < args.size(); i++) lv.PushTail(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv.Size()));
}

static std::string LPushXCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Integer(0);
  for (size_t i = 2; i < args.size(); i++) lv->PushHead(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv->Size()));
}

static std::string RPushXCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Integer(0);
  for (size_t i = 2; i < args.size(); i++) lv->PushTail(args[i]);
  return RespReply::Integer(static_cast<int64_t>(lv->Size()));
}

static std::string LPopCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Nil();
  auto r = lv->PopHead();
  if (!r) return RespReply::Nil();
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::BulkString(*r);
}

static std::string RPopCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Nil();
  auto r = lv->PopTail();
  if (!r) return RespReply::Nil();
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::BulkString(*r);
}

static std::string LLenCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  return RespReply::Integer(lv ? static_cast<int64_t>(lv->Size()) : 0);
}

static std::string LIndexCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Nil();
  auto p = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(p)) return InvalidInteger();
  auto v = lv->Get(std::get<ParsedInt>(p).value);
  return v ? RespReply::BulkString(*v) : RespReply::Nil();
}

static std::string LRangeCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::EmptyArray();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  return RespReply::ArrayOfBulkStrings(
      lv->Range(std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value));
}

static std::string LTrimCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Ok();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  lv->Trim(std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value);
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::Ok();
}

static std::string LRemCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* lv = lookup.value;
  if (!lv) return RespReply::Integer(0);
  auto p = ParseCanonicalInt(args[2]);
  if (!std::holds_alternative<ParsedInt>(p)) return InvalidInteger();
  auto r = lv->Remove(std::get<ParsedInt>(p).value, args[3]);
  if (lv->Empty()) ctx.db.Delete(args[1]);
  return RespReply::Integer(static_cast<int64_t>(r));
}

static std::string RPopLPushCmd(CommandContext& ctx, CommandArgs args) {
  auto src_lookup = LookupKeyAs<ListValue>(ctx.db, args[1]);
  if (src_lookup.WrongType()) return RespReply::WrongType();
  auto* src = src_lookup.value;
  if (!src) return RespReply::Nil();

  auto dst_lookup = LookupKeyAs<ListValue>(ctx.db, args[2]);
  if (dst_lookup.WrongType()) return RespReply::WrongType();

  auto v = src->PopTail();
  if (!v) return RespReply::Nil();
  if (args[1] == args[2]) {
    src->PushHead(*v);
    return RespReply::BulkString(*v);
  }
  if (src->Empty()) ctx.db.Delete(args[1]);
  auto& dst = GetOrCreateList(ctx, args[2], dst_lookup);
  dst.PushHead(*v);
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
