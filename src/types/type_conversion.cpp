#include "types/type_conversion.h"

#include <string>
#include <string_view>

#include "types/numeric_parse.h"

namespace miniredis {
namespace {

std::string_view ListpackValueView(const ds::Listpack::Value& value,
                                   std::string& storage) {
  if (value.type == ds::Listpack::Value::Type::kString) {
    return value.string;
  }
  storage = std::to_string(value.integer);
  return storage;
}

}  // namespace

SetHashtable IntsetToSetHashtable(const ds::Intset& intset) {
  SetHashtable ht;
  for (size_t i = 0; i < intset.Size(); i++) {
    auto value = intset.Get(i);
    if (value) ht.Add(std::to_string(*value), std::monostate{});
  }
  return ht;
}

HashHashtable ListpackToHashDict(const ds::Listpack& lp) {
  HashHashtable ht;
  // Listpack layout: [field1, val1, field2, val2, ...]
  auto it = lp.begin();
  auto end = lp.end();
  while (it != end) {
    auto field = *it;
    ++it;
    if (it == end) break;
    auto value = *it;
    ++it;
    std::string field_storage;
    ht.SetView(ListpackValueView(field, field_storage), value.ToString());
  }
  return ht;
}

ZSetSkiplist ListpackToZSetSkiplist(ds::Listpack&& lp) {
  ZSetSkiplist zs;
  // Listpack layout: [ele1, score1_as_str, ele2, score2_as_str, ...]
  // Entries are already in (score, element) order
  auto it = lp.begin();
  auto end = lp.end();
  while (it != end) {
    auto ele = *it;
    ++it;
    if (it == end) break;
    auto score_str = *it;
    ++it;
    auto parsed = ParseFiniteDouble(score_str.ToString());
    double score = 0.0;
    if (std::holds_alternative<double>(parsed)) {
      score = std::get<double>(parsed);
    }
    std::string ele_storage;
    zs.skiplist.InsertView(score, ListpackValueView(ele, ele_storage));
  }
  zs.RebuildDict();
  return zs;
}

// ZSetSkiplist move and RebuildDict
ZSetSkiplist::ZSetSkiplist(ZSetSkiplist&& other) noexcept
    : skiplist(std::move(other.skiplist)), dict() {
  RebuildDict();
  other.dict.Clear();
}

ZSetSkiplist& ZSetSkiplist::operator=(ZSetSkiplist&& other) noexcept {
  if (this == &other) return *this;
  dict.Clear();
  skiplist = std::move(other.skiplist);
  RebuildDict();
  other.dict.Clear();
  return *this;
}

void ZSetSkiplist::RebuildDict() {
  dict.Clear();
  auto* node = skiplist.First();
  while (node) {
    dict.SetView(node->key, node);
    node = node->levels[0].forward;
  }
}

}  // namespace miniredis
