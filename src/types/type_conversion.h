#pragma once

#include <string>

#include "ds/dict.h"
#include "ds/intset.h"
#include "ds/listpack.h"
#include "types/zset_value.h"

namespace miniredis {

using SetHashtable = ds::Dict<std::string, std::monostate>;
using HashHashtable = ds::Dict<std::string, std::string>;

SetHashtable IntsetToSetHashtable(const ds::Intset& intset);
HashHashtable ListpackToHashDict(const ds::Listpack& lp);
ZSetSkiplist ListpackToZSetSkiplist(ds::Listpack&& lp);

}  // namespace miniredis
