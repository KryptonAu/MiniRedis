#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/hash_value.h"
#include "types/numeric_parse.h"
#include "types/value.h"

namespace miniredis {

using Flag = CommandFlag;

static HashValue& GetOrCreateHash(CommandContext& ctx, std::string_view key,
                                  TypedKeyLookup<HashValue>& lookup) {
  return GetOrCreateValueAs<HashValue>(ctx.db, key, lookup,
                                       [&ctx] { return MakeHashValue(ctx); });
}

static std::string HSetCmd(CommandContext& ctx, CommandArgs args) {
  if ((args.size() - 2) % 2 != 0) return WrongArity("HSET");
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto& hv = GetOrCreateHash(ctx, args[1], lookup);
  int created = 0;
  for (size_t i = 2; i + 1 < args.size(); i += 2)
    if (hv.Set(args[i], args[i + 1])) created++;
  return RespReply::Integer(created);
}
static std::string HGetCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  if (!hv) return RespReply::Nil();
  auto v = hv->Get(args[2]);
  return v ? RespReply::BulkString(*v) : RespReply::Nil();
}
static std::string HDelCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  if (!hv) return RespReply::Integer(0);
  int deleted = 0;
  for (size_t i = 2; i < args.size(); i++)
    if (hv->Delete(args[i])) deleted++;
  if (hv->Size() == 0) ctx.db.Delete(args[1]);
  return RespReply::Integer(deleted);
}
static std::string HLenCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  return RespReply::Integer(hv ? static_cast<int64_t>(hv->Size()) : 0);
}
static std::string HExistsCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  return RespReply::Integer(hv && hv->Exists(args[2]) ? 1 : 0);
}
static std::string HKeysCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  if (!hv) return RespReply::EmptyArray();
  return RespReply::ArrayOfBulkStrings(hv->Keys());
}
static std::string HValsCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  if (!hv) return RespReply::EmptyArray();
  return RespReply::ArrayOfBulkStrings(hv->Values());
}
static std::string HGetAllCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto* hv = lookup.value;
  if (!hv) return RespReply::EmptyArray();
  auto all = hv->GetAll();
  std::vector<std::string> flat;
  for (auto& [k, v] : all) {
    flat.push_back(k);
    flat.push_back(v);
  }
  return RespReply::ArrayOfBulkStrings(flat);
}
static std::string HIncrByCmd(CommandContext& ctx, CommandArgs args) {
  auto lookup = LookupKeyAs<HashValue>(ctx.db, args[1]);
  if (lookup.WrongType()) return RespReply::WrongType();
  auto p = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p)) return InvalidInteger();
  auto& hv = GetOrCreateHash(ctx, args[1], lookup);
  auto r = hv.IncrementBy(args[2], std::get<ParsedInt>(p).value);
  if (std::holds_alternative<TypeError>(r))
    return TypeErrorToResp(std::get<TypeError>(r));
  return RespReply::Integer(std::get<int64_t>(r));
}

void RegisterHashCommands(CommandRegistry& registry) {
  auto ro = static_cast<uint32_t>(Flag::kReadOnly);
  auto wr = static_cast<uint32_t>(Flag::kWrite);
  registry.Register({"HSET", -4, wr, HSetCmd});
  registry.Register({"HGET", 3, ro, HGetCmd});
  registry.Register({"HDEL", -3, wr, HDelCmd});
  registry.Register({"HLEN", 2, ro, HLenCmd});
  registry.Register({"HEXISTS", 3, ro, HExistsCmd});
  registry.Register({"HKEYS", 2, ro, HKeysCmd});
  registry.Register({"HVALS", 2, ro, HValsCmd});
  registry.Register({"HGETALL", 2, ro, HGetAllCmd});
  registry.Register({"HINCRBY", 4, wr, HIncrByCmd});
}

}  // namespace miniredis
