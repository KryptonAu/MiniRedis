#include "types/value.h"

namespace miniredis {

ValueType GetType(const Value& v) {
  return std::visit(
      [](const auto& val) -> ValueType {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, StringValue>) return ValueType::kString;
        if constexpr (std::is_same_v<T, ListValue>) return ValueType::kList;
        if constexpr (std::is_same_v<T, SetValue>) return ValueType::kSet;
        if constexpr (std::is_same_v<T, HashValue>) return ValueType::kHash;
        if constexpr (std::is_same_v<T, ZSetValue>) return ValueType::kZSet;
      },
      v);
}

ValueEncoding GetEncoding(const Value& v) {
  return std::visit([](const auto& val) { return val.Encoding(); }, v);
}

std::string_view TypeName(ValueType type) {
  switch (type) {
    case ValueType::kString:
      return "string";
    case ValueType::kList:
      return "list";
    case ValueType::kSet:
      return "set";
    case ValueType::kHash:
      return "hash";
    case ValueType::kZSet:
      return "zset";
  }
  return "none";
}

}  // namespace miniredis
