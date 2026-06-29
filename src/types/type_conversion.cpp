#include "types/type_conversion.h"

#include "types/numeric_parse.h"

namespace miniredis {

SetHashtable IntsetToSetHashtable(const ds::Intset& intset) {
  SetHashtable ht;
  for (auto v : intset.Values()) {
    ht.Add(std::to_string(v), std::monostate{});
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
    ht.Set(field.ToString(), value.ToString());
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
    zs.skiplist.Insert(score, ele.ToString());
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
    dict.Set(node->key, node);
    node = node->levels[0].forward;
  }
}

}  // namespace miniredis
