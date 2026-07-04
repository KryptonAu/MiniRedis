#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "commands/command_context.h"
#include "types/operation_result.h"
#include "types/value.h"
#include "types/zset_value.h"

namespace miniredis {

struct CommandContext;

// Error responses
std::string WrongArity(std::string_view command);
std::string SyntaxError();
std::string InvalidInteger();
std::string InvalidExpireTime();
std::string InvalidDbIndex();
std::string NotImplemented(std::string_view command);
std::string Unsupported(std::string_view feature);
std::string TypeErrorToResp(TypeError error);

// Type extraction
template <typename T>
struct TypedKeyLookup {
  Value* raw = nullptr;
  T* value = nullptr;

  bool Missing() const { return raw == nullptr; }
  bool WrongType() const { return raw != nullptr && value == nullptr; }
};

template <typename T>
TypedKeyLookup<T> LookupKeyAs(Database& db, std::string_view key) {
  auto* val = db.Find(key);
  return {val, val ? std::get_if<T>(val) : nullptr};
}

template <typename T, typename Factory>
T& GetOrCreateValueAs(Database& db, std::string_view key,
                      TypedKeyLookup<T>& lookup, Factory&& factory) {
  assert(!lookup.WrongType());
  if (lookup.value) return *lookup.value;

  db.Set(key, std::forward<Factory>(factory)());
  lookup = LookupKeyAs<T>(db, key);
  assert(lookup.value != nullptr);
  return *lookup.value;
}

// Create typed values with server thresholds
SetValue MakeSetValue(CommandContext& ctx);
HashValue MakeHashValue(CommandContext& ctx);
ZSetValue MakeZSetValue(CommandContext& ctx);

// RESP array helpers
std::string ArrayOfZSetRange(const std::vector<ZSetValue::RangeResult>& values,
                             bool with_scores);
std::string ScanReply(size_t next_cursor,
                      const std::vector<std::string>& elements);

// TTL helpers
int64_t MsToSec(int64_t ms);

}  // namespace miniredis
