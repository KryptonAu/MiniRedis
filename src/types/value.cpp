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

size_t ApproxMemoryUsage(const Value& v) {
  return std::visit(
      [](const auto& val) -> size_t {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, StringValue>) {
          // IntEmbed: just the variant storage (~16 bytes). Raw: string heap.
          size_t s = sizeof(StringValue);
          if (val.Encoding() == ValueEncoding::kRaw) {
            s += val.Length();
          }
          return s;
        }
        if constexpr (std::is_same_v<T, ListValue>) {
          // Quicklist: node overhead + listpack data.
          size_t s = sizeof(ListValue) + val.Size() * 16;
          return s;
        }
        if constexpr (std::is_same_v<T, SetValue>) {
          size_t s = sizeof(SetValue);
          if (val.Encoding() == ValueEncoding::kIntset) {
            // Intset has compact buf_ member.
            s += val.Size() * 8;
          } else {
            // Hashtable: dict entry overhead + key strings.
            s += val.Size() * 48;
          }
          return s;
        }
        if constexpr (std::is_same_v<T, HashValue>) {
          size_t s = sizeof(HashValue);
          if (val.Encoding() == ValueEncoding::kListpack) {
            s += val.Size() * 32;
          } else {
            s += val.Size() * 64;
          }
          return s;
        }
        if constexpr (std::is_same_v<T, ZSetValue>) {
          size_t s = sizeof(ZSetValue);
          if (val.Encoding() == ValueEncoding::kZSetListpack) {
            s += val.Count() * 32;
          } else {
            s += val.Count() * 80;
          }
          return s;
        }
        return sizeof(val);
      },
      v);
}

}  // namespace miniredis
