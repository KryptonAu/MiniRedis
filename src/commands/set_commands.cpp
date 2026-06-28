#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/set_value.h"
#include "types/value.h"

namespace miniredis {

using Flag = CommandFlag;

static SetValue* GetSet(CommandContext& ctx, const std::string& key) {
  auto* v = ctx.db.Find(key);
  return v ? std::get_if<SetValue>(v) : nullptr;
}

static bool IsSetWrongType(CommandContext& ctx, const std::string& key) {
  auto* val = ctx.db.Find(key);
  return val && !std::holds_alternative<SetValue>(*val);
}

static SetValue& GetOrCreateSet(CommandContext& ctx, const std::string& key) {
  auto* v = ctx.db.Find(key);
  if (!v) {
    ctx.db.Set(key, MakeSetValue(ctx));
    return std::get<SetValue>(*ctx.db.Find(key));
  }
  return std::get<SetValue>(*v);
}

static std::string SAddCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<SetValue>(*val))
    return RespReply::WrongType();
  auto& sv = GetOrCreateSet(ctx, args[1]);
  int added = 0;
  for (size_t i = 2; i < args.size(); i++)
    if (sv.Add(args[i])) added++;
  return RespReply::Integer(added);
}
static std::string SRemCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  auto* val_sr = ctx.db.Find(args[1]);
  if (val_sr && !std::holds_alternative<SetValue>(*val_sr))
    return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  if (!sv) return RespReply::Integer(0);
  int removed = 0;
  for (size_t i = 2; i < args.size(); i++)
    if (sv->Remove(args[i])) removed++;
  if (sv->Size() == 0) ctx.db.Delete(args[1]);
  return RespReply::Integer(removed);
}
static std::string SMembersCmd(CommandContext& ctx,
                               const std::vector<std::string>& args) {
  if (IsSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  if (!sv) return RespReply::EmptyArray();
  return RespReply::ArrayOfBulkStrings(sv->Members());
}
static std::string SCardCmd(CommandContext& ctx,
                            const std::vector<std::string>& args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<SetValue>(*val))
    return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  return RespReply::Integer(sv ? static_cast<int64_t>(sv->Size()) : 0);
}
static std::string SIsMemberCmd(CommandContext& ctx,
                                const std::vector<std::string>& args) {
  if (IsSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  return RespReply::Integer(sv && sv->Contains(args[2]) ? 1 : 0);
}
static std::string SPopCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  if (args.size() > 2) return Unsupported("SPOP count");
  if (IsSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  if (!sv) return RespReply::Nil();
  auto m = sv->Pop();
  if (!m) return RespReply::Nil();
  if (sv->Size() == 0) ctx.db.Delete(args[1]);
  return RespReply::BulkString(*m);
}
static std::string SRandMemberCmd(CommandContext& ctx,
                                  const std::vector<std::string>& args) {
  if (args.size() > 2) return Unsupported("SRANDMEMBER count");
  if (IsSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* sv = GetSet(ctx, args[1]);
  if (!sv) return RespReply::Nil();
  auto m = sv->RandomMember();
  return m ? RespReply::BulkString(*m) : RespReply::Nil();
}

void RegisterSetCommands(CommandRegistry& registry) {
  auto ro = static_cast<uint32_t>(Flag::kReadOnly);
  auto wr = static_cast<uint32_t>(Flag::kWrite);
  registry.Register({"SADD", -3, wr, SAddCmd});
  registry.Register({"SREM", -3, wr, SRemCmd});
  registry.Register({"SMEMBERS", 2, ro, SMembersCmd});
  registry.Register({"SCARD", 2, ro, SCardCmd});
  registry.Register({"SISMEMBER", 3, ro, SIsMemberCmd});
  registry.Register({"SPOP", -2, wr, SPopCmd});
  registry.Register({"SRANDMEMBER", -2, ro, SRandMemberCmd});
}

}  // namespace miniredis
