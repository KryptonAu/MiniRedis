#include <cmath>
#include <optional>
#include <utility>

#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "types/numeric_parse.h"
#include "types/value.h"
#include "types/zset_value.h"

namespace miniredis {

using Flag = CommandFlag;

static ZSetValue* GetZSet(CommandContext& ctx, std::string_view key) {
  auto* v = ctx.db.Find(key);
  return v ? std::get_if<ZSetValue>(v) : nullptr;
}

static bool IsZSetWrongType(CommandContext& ctx, std::string_view key) {
  auto* val = ctx.db.Find(key);
  return val && !std::holds_alternative<ZSetValue>(*val);
}

static ZSetValue& GetOrCreateZSet(CommandContext& ctx, std::string_view key) {
  auto* v = ctx.db.Find(key);
  if (!v) {
    ctx.db.Set(key, MakeZSetValue(ctx));
    return std::get<ZSetValue>(*ctx.db.Find(key));
  }
  return std::get<ZSetValue>(*v);
}

// ===== ZADD =====
static std::string ZAddCmd(CommandContext& ctx, CommandArgs args) {
  size_t i = 2;
  bool nx = false, xx = false, ch = false, incr = false;

  while (i < args.size() && !args[i].empty() &&
         (args[i][0] == 'N' || args[i][0] == 'X' || args[i][0] == 'C' ||
          args[i][0] == 'I')) {
    if (args[i] == "NX")
      nx = true;
    else if (args[i] == "XX")
      xx = true;
    else if (args[i] == "CH")
      ch = true;
    else if (args[i] == "INCR")
      incr = true;
    else
      break;
    i++;
  }
  if (nx && xx) return SyntaxError();

  if (incr) {
    if (args.size() - i != 2) return SyntaxError();
    auto sc_parsed = ParseFiniteDouble(args[i++]);
    if (std::holds_alternative<TypeError>(sc_parsed))
      return TypeErrorToResp(std::get<TypeError>(sc_parsed));
    double score = std::get<double>(sc_parsed);
    std::string_view member = args[i];
    auto* val = ctx.db.Find(args[1]);
    if (val && !std::holds_alternative<ZSetValue>(*val))
      return RespReply::WrongType();
    auto* existing_zs = val ? std::get_if<ZSetValue>(val) : nullptr;
    auto old = existing_zs ? existing_zs->Score(member) : std::nullopt;
    if (nx && old) return RespReply::Nil();
    if (xx && !old) return RespReply::Nil();
    auto& zs = existing_zs ? *existing_zs : GetOrCreateZSet(ctx, args[1]);
    auto r = zs.Update(member, score);
    if (std::holds_alternative<TypeError>(r))
      return TypeErrorToResp(std::get<TypeError>(r));
    return RespReply::BulkString(FormatDoubleForStorage(std::get<double>(r)));
  }

  size_t remaining = args.size() - i;
  if (remaining == 0 || remaining % 2 != 0) return WrongArity("ZADD");
  auto* val2 = ctx.db.Find(args[1]);
  if (val2 && !std::holds_alternative<ZSetValue>(*val2))
    return RespReply::WrongType();

  std::vector<std::pair<double, std::string>> entries;
  entries.reserve(remaining / 2);
  for (size_t entry = i; entry + 1 < args.size(); entry += 2) {
    auto sc_parsed = ParseFiniteDouble(args[entry]);
    if (std::holds_alternative<TypeError>(sc_parsed))
      return TypeErrorToResp(std::get<TypeError>(sc_parsed));
    entries.emplace_back(std::get<double>(sc_parsed), args[entry + 1]);
  }

  auto* existing_zs = val2 ? std::get_if<ZSetValue>(val2) : nullptr;
  if (!existing_zs && xx) return RespReply::Integer(0);
  auto& zs = existing_zs ? *existing_zs : GetOrCreateZSet(ctx, args[1]);

  int added = 0, changed = 0;
  for (const auto& [score, member] : entries) {
    auto old = zs.Score(member);
    if (nx && old) continue;
    if (xx && !old) continue;
    auto r = zs.Add(member, score);
    if (std::holds_alternative<TypeError>(r))
      return TypeErrorToResp(std::get<TypeError>(r));
    if (std::get<bool>(r))
      added++;
    else if (old != score)
      changed++;
  }
  return RespReply::Integer(ch ? (added + changed) : added);
}

// ===== ZREM =====
static std::string ZRemCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Integer(0);
  int removed = 0;
  for (size_t i = 2; i < args.size(); i++)
    if (zs->Remove(args[i])) removed++;
  if (zs->Count() == 0) ctx.db.Delete(args[1]);
  return RespReply::Integer(removed);
}

// ===== ZCARD =====
static std::string ZCardCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  return RespReply::Integer(zs ? static_cast<int64_t>(zs->Count()) : 0);
}

// ===== ZCOUNT =====
static std::string ZCountCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Integer(0);
  auto p1 = ParseFiniteDouble(args[2]), p2 = ParseFiniteDouble(args[3]);
  if (std::holds_alternative<TypeError>(p1) ||
      std::holds_alternative<TypeError>(p2))
    return TypeErrorToResp(std::holds_alternative<TypeError>(p1)
                               ? std::get<TypeError>(p1)
                               : std::get<TypeError>(p2));
  return RespReply::Integer(static_cast<int64_t>(zs->CountByScore(
      std::get<double>(p1), std::get<double>(p2), false, false)));
}

// ===== ZSCORE =====
static std::string ZScoreCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Nil();
  auto s = zs->Score(args[2]);
  return s ? RespReply::BulkString(FormatDoubleForStorage(*s))
           : RespReply::Nil();
}

// ===== ZRANK / ZREVRANK =====
static std::string ZRankCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Nil();
  auto r = zs->Rank(args[2]);
  return r ? RespReply::Integer(static_cast<int64_t>(*r)) : RespReply::Nil();
}
static std::string ZRevRankCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Nil();
  auto r = zs->RevRank(args[2]);
  return r ? RespReply::Integer(static_cast<int64_t>(*r)) : RespReply::Nil();
}

// ===== ZINCRBY =====
static std::string ZIncrByCmd(CommandContext& ctx, CommandArgs args) {
  auto* val = ctx.db.Find(args[1]);
  if (val && !std::holds_alternative<ZSetValue>(*val))
    return RespReply::WrongType();
  auto sc_parsed = ParseFiniteDouble(args[2]);
  if (std::holds_alternative<TypeError>(sc_parsed))
    return TypeErrorToResp(std::get<TypeError>(sc_parsed));
  auto& zs = GetOrCreateZSet(ctx, args[1]);
  auto r = zs.Update(args[3], std::get<double>(sc_parsed));
  if (std::holds_alternative<TypeError>(r))
    return TypeErrorToResp(std::get<TypeError>(r));
  return RespReply::BulkString(FormatDoubleForStorage(std::get<double>(r)));
}

// ===== ZRANGE / ZREVRANGE =====
static std::string ZRangeCmd(CommandContext& ctx, CommandArgs args) {
  auto* val_zr = ctx.db.Find(args[1]);
  if (val_zr && !std::holds_alternative<ZSetValue>(*val_zr))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::EmptyArray();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  bool withscores = false;
  if (args.size() > 4) {
    if (args.size() == 5 && args[4] == "WITHSCORES")
      withscores = true;
    else
      return SyntaxError();
  }
  auto r =
      zs->Range(std::get<ParsedInt>(p1).value, std::get<ParsedInt>(p2).value);
  return ArrayOfZSetRange(r, withscores);
}

static std::string ZRevRangeCmd(CommandContext& ctx, CommandArgs args) {
  auto* val_zrr = ctx.db.Find(args[1]);
  if (val_zrr && !std::holds_alternative<ZSetValue>(*val_zrr))
    return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::EmptyArray();
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  bool withscores = false;
  if (args.size() > 4) {
    if (args.size() == 5 && args[4] == "WITHSCORES")
      withscores = true;
    else
      return SyntaxError();
  }
  auto r = zs->RevRange(std::get<ParsedInt>(p1).value,
                        std::get<ParsedInt>(p2).value);
  return ArrayOfZSetRange(r, withscores);
}

// ===== ZRANGEBYSCORE =====
static std::string ZRangeByScoreCmd(CommandContext& ctx, CommandArgs args) {
  if (IsZSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto min_p = ParseFiniteDouble(args[2]), max_p = ParseFiniteDouble(args[3]);
  if (std::holds_alternative<TypeError>(min_p) ||
      std::holds_alternative<TypeError>(max_p))
    return TypeErrorToResp(std::holds_alternative<TypeError>(min_p)
                               ? std::get<TypeError>(min_p)
                               : std::get<TypeError>(max_p));
  double min = std::get<double>(min_p), max = std::get<double>(max_p);
  bool min_ex = false, max_ex = false;
  bool withscores = false;
  long long offset = 0, count = -1;
  for (size_t i = 4; i < args.size();) {
    if (args[i] == "WITHSCORES") {
      withscores = true;
      i++;
    } else if (args[i] == "LIMIT") {
      if (i + 2 >= args.size()) return SyntaxError();
      auto o = ParseCanonicalInt(args[i + 1]);
      auto c = ParseCanonicalInt(args[i + 2]);
      if (!std::holds_alternative<ParsedInt>(o) ||
          !std::holds_alternative<ParsedInt>(c))
        return InvalidInteger();
      offset = std::get<ParsedInt>(o).value;
      count = std::get<ParsedInt>(c).value;
      i += 3;
    } else {
      return SyntaxError();
    }
  }
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::EmptyArray();
  auto r = zs->RangeByScore(min, max, min_ex, max_ex, offset, count);
  return ArrayOfZSetRange(r, withscores);
}

// ===== ZPOPMIN / ZPOPMAX =====
static std::string ZPopMinCmd(CommandContext& ctx, CommandArgs args) {
  if (args.size() > 3) return WrongArity("ZPOPMIN");
  if (IsZSetWrongType(ctx, args[1])) return RespReply::WrongType();
  int64_t count = 1;
  if (args.size() == 3) {
    auto parsed = ParseCanonicalInt(args[2]);
    if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
    count = std::get<ParsedInt>(parsed).value;
    if (count < 0) return InvalidInteger();
  }
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::EmptyArray();
  std::vector<ZSetValue::RangeResult> v;
  for (int64_t i = 0; i < count; i++) {
    auto r = zs->PopMin();
    if (!r) break;
    v.push_back(*r);
  }
  if (zs->Count() == 0) ctx.db.Delete(args[1]);
  return ArrayOfZSetRange(v, true);
}
static std::string ZPopMaxCmd(CommandContext& ctx, CommandArgs args) {
  if (args.size() > 3) return WrongArity("ZPOPMAX");
  if (IsZSetWrongType(ctx, args[1])) return RespReply::WrongType();
  int64_t count = 1;
  if (args.size() == 3) {
    auto parsed = ParseCanonicalInt(args[2]);
    if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
    count = std::get<ParsedInt>(parsed).value;
    if (count < 0) return InvalidInteger();
  }
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::EmptyArray();
  std::vector<ZSetValue::RangeResult> v;
  for (int64_t i = 0; i < count; i++) {
    auto r = zs->PopMax();
    if (!r) break;
    v.push_back(*r);
  }
  if (zs->Count() == 0) ctx.db.Delete(args[1]);
  return ArrayOfZSetRange(v, true);
}

// ===== ZREMRANGEBYRANK =====
static std::string ZRemRangeByRankCmd(CommandContext& ctx, CommandArgs args) {
  if (IsZSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Integer(0);
  auto p1 = ParseCanonicalInt(args[2]), p2 = ParseCanonicalInt(args[3]);
  if (!std::holds_alternative<ParsedInt>(p1) ||
      !std::holds_alternative<ParsedInt>(p2))
    return InvalidInteger();
  auto r = zs->RemoveRangeByRank(std::get<ParsedInt>(p1).value,
                                 std::get<ParsedInt>(p2).value);
  if (zs->Count() == 0) ctx.db.Delete(args[1]);
  return RespReply::Integer(static_cast<int64_t>(r));
}

// ===== ZREMRANGEBYSCORE =====
static std::string ZRemRangeByScoreCmd(CommandContext& ctx, CommandArgs args) {
  if (IsZSetWrongType(ctx, args[1])) return RespReply::WrongType();
  auto* zs = GetZSet(ctx, args[1]);
  if (!zs) return RespReply::Integer(0);
  auto min_p = ParseFiniteDouble(args[2]), max_p = ParseFiniteDouble(args[3]);
  if (std::holds_alternative<TypeError>(min_p) ||
      std::holds_alternative<TypeError>(max_p))
    return InvalidInteger();
  auto r = zs->RemoveRangeByScore(std::get<double>(min_p),
                                  std::get<double>(max_p), false, false);
  if (zs->Count() == 0) ctx.db.Delete(args[1]);
  return RespReply::Integer(static_cast<int64_t>(r));
}

void RegisterZSetCommands(CommandRegistry& registry) {
  auto ro = static_cast<uint32_t>(Flag::kReadOnly);
  auto wr = static_cast<uint32_t>(Flag::kWrite);
  registry.Register({"ZADD", -4, wr, ZAddCmd});
  registry.Register({"ZREM", -3, wr, ZRemCmd});
  registry.Register({"ZCARD", 2, ro, ZCardCmd});
  registry.Register({"ZCOUNT", 4, ro, ZCountCmd});
  registry.Register({"ZSCORE", 3, ro, ZScoreCmd});
  registry.Register({"ZRANK", 3, ro, ZRankCmd});
  registry.Register({"ZREVRANK", 3, ro, ZRevRankCmd});
  registry.Register({"ZINCRBY", 4, wr, ZIncrByCmd});
  registry.Register({"ZRANGE", -4, ro, ZRangeCmd});
  registry.Register({"ZREVRANGE", -4, ro, ZRevRangeCmd});
  registry.Register({"ZRANGEBYSCORE", -4, ro, ZRangeByScoreCmd});
  registry.Register({"ZPOPMIN", -2, wr, ZPopMinCmd});
  registry.Register({"ZPOPMAX", -2, wr, ZPopMaxCmd});
  registry.Register({"ZREMRANGEBYRANK", 4, wr, ZRemRangeByRankCmd});
  registry.Register({"ZREMRANGEBYSCORE", 4, wr, ZRemRangeByScoreCmd});
}

}  // namespace miniredis
