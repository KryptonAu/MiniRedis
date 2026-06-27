#pragma once

#include <string_view>
#include <variant>

#include "types/hash_value.h"
#include "types/list_value.h"
#include "types/set_value.h"
#include "types/string_value.h"
#include "types/value_fwd.h"
#include "types/zset_value.h"

namespace miniredis {

using Value =
    std::variant<StringValue, ListValue, SetValue, HashValue, ZSetValue>;

ValueType GetType(const Value& v);
ValueEncoding GetEncoding(const Value& v);
std::string_view TypeName(ValueType type);

}  // namespace miniredis
