#include "types/zset_value.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

#include "types/numeric_parse.h"
#include "types/type_conversion.h"

namespace miniredis {
namespace {

constexpr size_t kInt64StringMax = 32;

std::string_view ListpackMemberView(
    const ds::Listpack::Value& value,
    std::array<char, kInt64StringMax>& scratch) {
  if (value.type == ds::Listpack::Value::Type::kString) return value.string;

  auto [ptr, ec] = std::to_chars(
      scratch.data(), scratch.data() + scratch.size(), value.integer);
  if (ec != std::errc()) return {};
  return std::string_view(scratch.data(),
                          static_cast<size_t>(ptr - scratch.data()));
}

bool ListpackMemberEquals(const ds::Listpack::Value& value,
                          std::string_view lookup) {
  std::array<char, kInt64StringMax> scratch{};
  return ListpackMemberView(value, scratch) == lookup;
}

bool LookupLessThanListpackMember(std::string_view lookup,
                                  const ds::Listpack::Value& value) {
  std::array<char, kInt64StringMax> scratch{};
  return lookup < ListpackMemberView(value, scratch);
}

bool ListpackMemberInLexRange(const ds::Listpack::Value& value,
                              std::string_view min, std::string_view max,
                              bool min_ex, bool max_ex) {
  std::array<char, kInt64StringMax> scratch{};
  std::string_view member = ListpackMemberView(value, scratch);
  if (min_ex ? member <= min : member < min) return false;
  if (max_ex ? member >= max : member > max) return false;
  return true;
}

}  // namespace

ZSetValue::ZSetValue(EncodingThresholds thresholds) : thresholds_(thresholds) {
  encoding_ = ds::Listpack{};
}

TypeResult<bool> ZSetValue::Add(std::string_view element, double score) {
  if (!std::isfinite(score)) return TypeError::kInvalidScore;

  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    // Listpack layout: [ele1, score_str1, ele2, score_str2, ...] sorted by
    // (score, ele) Find if element already exists, and find insertion position
    bool found = false;
    size_t insert_idx = lp->Size();  // default: append at end

    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      auto score_str = lp->Get(i + 1);
      if (!ele || !score_str) continue;

      if (ListpackMemberEquals(*ele, element)) {
        // Update existing
        double old_score = 0.0;
        auto parsed = ParseFiniteDouble(score_str->ToString());
        if (std::holds_alternative<double>(parsed)) {
          old_score = std::get<double>(parsed);
        }
        if (old_score == score) {
          return false;  // same score, no change
        }
        // Remove old pair
        lp->Delete(i + 1);
        lp->Delete(i);
        found = true;
        // Re-scan for correct insertion position after removal
        insert_idx = lp->Size();
        for (size_t j = 0; j < lp->Size(); j += 2) {
          auto e = lp->Get(j);
          auto ss = lp->Get(j + 1);
          if (!e || !ss) continue;
          double existing = 0.0;
          auto p = ParseFiniteDouble(ss->ToString());
          if (std::holds_alternative<double>(p)) existing = std::get<double>(p);
          if (score < existing || (score == existing &&
                                   LookupLessThanListpackMember(element, *e))) {
            insert_idx = j;
            break;
          }
        }
        break;
      }

      // Find insertion position for sorted order
      if (insert_idx == lp->Size()) {
        double existing_score = 0.0;
        auto parsed = ParseFiniteDouble(score_str->ToString());
        if (std::holds_alternative<double>(parsed)) {
          existing_score = std::get<double>(parsed);
        }
        if (score < existing_score ||
            (score == existing_score &&
             LookupLessThanListpackMember(element, *ele))) {
          insert_idx = i;
        }
      }
    }

    // Insert at correct position
    if (insert_idx == lp->Size()) {
      lp->Append(element);
      lp->Append(FormatDoubleForStorage(score));
    } else {
      lp->Insert(insert_idx, element);
      lp->Insert(insert_idx + 1, FormatDoubleForStorage(score));
    }

    MaybeUpgrade(element.size());
    return !found;  // true if new element
  }

  // Skiplist encoding
  auto& zs = std::get<ZSetSkiplist>(encoding_);
  auto* existing = zs.dict.FindView(element);
  if (existing) {
    auto* node = *existing;
    if (node->score == score) return false;
    zs.dict.DeleteView(element);
    zs.skiplist.DeleteNode(node);
  }
  auto* node = zs.skiplist.InsertView(score, element);
  zs.dict.SetView(element, node);
  return existing == nullptr;
}

bool ZSetValue::Remove(std::string_view element) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      if (ele && ListpackMemberEquals(*ele, element)) {
        lp->Delete(i + 1);
        lp->Delete(i);
        return true;
      }
    }
    return false;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  auto* node_ptr = zs.dict.FindView(element);
  if (!node_ptr) return false;
  auto* node = *node_ptr;
  zs.dict.DeleteView(element);
  zs.skiplist.DeleteNode(node);
  return true;
}

TypeResult<double> ZSetValue::Update(std::string_view element, double delta) {
  if (!std::isfinite(delta)) return TypeError::kInvalidFloat;

  auto old = Score(element);
  double new_score = old.value_or(0.0) + delta;
  if (!std::isfinite(new_score)) return TypeError::kInvalidScore;

  if (old) {
    // Remove old, then add new (even in listpack)
    Remove(element);
  }
  Add(element, new_score);
  return new_score;
}

std::optional<double> ZSetValue::Score(std::string_view element) const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      if (ele && ListpackMemberEquals(*ele, element)) {
        auto score_str = lp->Get(i + 1);
        if (score_str) {
          auto parsed = ParseFiniteDouble(score_str->ToString());
          if (std::holds_alternative<double>(parsed))
            return std::get<double>(parsed);
        }
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  auto* node_ptr = zs.dict.FindView(element);
  if (!node_ptr) return std::nullopt;
  return (*node_ptr)->score;
}

std::optional<size_t> ZSetValue::Rank(std::string_view element) const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      if (ele && ListpackMemberEquals(*ele, element)) return i / 2;
    }
    return std::nullopt;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  auto* node_ptr = zs.dict.FindView(element);
  if (!node_ptr) return std::nullopt;
  auto rank = zs.skiplist.GetRankView((*node_ptr)->score, element);
  if (rank) return *rank - 1;  // 1-based → 0-based
  return std::nullopt;
}

std::optional<size_t> ZSetValue::RevRank(std::string_view element) const {
  auto r = Rank(element);
  if (!r) return std::nullopt;
  return Count() - *r - 1;
}

size_t ZSetValue::Count() const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) return lp->Size() / 2;
  return std::get<ZSetSkiplist>(encoding_).skiplist.Size();
}

size_t ZSetValue::CountByScore(double min, double max, bool min_ex,
                               bool max_ex) const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    size_t count = 0;
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto score_str = lp->Get(i + 1);
      if (!score_str) continue;
      auto parsed = ParseFiniteDouble(score_str->ToString());
      if (!std::holds_alternative<double>(parsed)) continue;
      double s = std::get<double>(parsed);
      bool in_range = true;
      if (min_ex ? s <= min : s < min) in_range = false;
      if (max_ex ? s >= max : s > max) in_range = false;
      if (in_range) count++;
    }
    return count;
  }
  auto& zs = std::get<ZSetSkiplist>(encoding_);
  typename ds::Skiplist<std::string, double>::RangeSpec spec{min, max, min_ex,
                                                             max_ex};
  if (!zs.skiplist.ScoreInRange(spec)) return 0;
  auto* node = zs.skiplist.FirstInRange(spec);
  size_t count = 0;
  while (node) {
    double s = node->score;
    if (max_ex ? s >= max : s > max) break;
    count++;
    node = node->levels[0].forward;
  }
  return count;
}

size_t ZSetValue::LexCount(std::string_view min, std::string_view max,
                           bool min_ex, bool max_ex) const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    size_t count = 0;
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      if (ele && ListpackMemberInLexRange(*ele, min, max, min_ex, max_ex)) {
        count++;
      }
    }
    return count;
  }

  // ZSet lex operations require all elements to have the same score
  auto all = Range(0, -1);
  size_t count = 0;
  for (const auto& r : all) {
    bool in_range = true;
    if (min_ex ? r.element <= min : r.element < min) in_range = false;
    if (max_ex ? r.element >= max : r.element > max) in_range = false;
    if (in_range) count++;
  }
  return count;
}

std::vector<ZSetValue::RangeResult> ZSetValue::Range(long long start,
                                                     long long stop) const {
  std::vector<RangeResult> result;
  long long sz = static_cast<long long>(Count());
  if (sz == 0) return result;

  if (start < 0) start = std::max(start + sz, 0LL);
  if (stop < 0) stop = std::max(stop + sz, 0LL);
  if (start > stop || start >= sz) return result;
  stop = std::min(stop, sz - 1);

  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (long long i = start; i <= stop; i++) {
      auto ele = lp->Get(static_cast<size_t>(i * 2));
      auto score_str = lp->Get(static_cast<size_t>(i * 2 + 1));
      if (ele && score_str) {
        double s = 0.0;
        auto parsed = ParseFiniteDouble(score_str->ToString());
        if (std::holds_alternative<double>(parsed))
          s = std::get<double>(parsed);
        result.push_back({ele->ToString(), s});
      }
    }
    return result;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  auto* node = zs.skiplist.GetByRank(static_cast<size_t>(start + 1));
  for (long long i = start; i <= stop && node; i++) {
    result.push_back({node->key, node->score});
    node = node->levels[0].forward;
  }
  return result;
}

std::vector<ZSetValue::RangeResult> ZSetValue::RevRange(long long start,
                                                        long long stop) const {
  // ZREVRANGE: elements from highest score to lowest.
  // Index 0 = highest score. Follows Redis genericZrangebyrankCommand
  // semantics.
  auto all = Range(0, -1);               // low to high
  std::reverse(all.begin(), all.end());  // now high to low

  long long sz = static_cast<long long>(all.size());
  if (sz == 0) return {};

  // Sanitize indices (same as Redis)
  if (start < 0) start += sz;
  if (stop < 0) stop += sz;
  if (start < 0) start = 0;
  if (stop >= sz) stop = sz - 1;
  if (start > stop || start >= sz) return {};

  return {all.begin() + static_cast<long>(start), all.begin() + stop + 1};
}

std::vector<ZSetValue::RangeResult> ZSetValue::RangeByScore(
    double min, double max, bool min_ex, bool max_ex, long long offset,
    long long count) const {
  std::vector<RangeResult> result;
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    // O(N) scan for listpack
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      auto score_str = lp->Get(i + 1);
      if (!ele || !score_str) continue;
      auto parsed = ParseFiniteDouble(score_str->ToString());
      if (!std::holds_alternative<double>(parsed)) continue;
      double s = std::get<double>(parsed);
      if (min_ex ? s <= min : s < min) continue;
      if (max_ex ? s >= max : s > max) break;  // sorted listpack
      if (offset > 0) {
        offset--;
        continue;
      }
      result.push_back({ele->ToString(), s});
      if (count >= 0 && static_cast<long long>(result.size()) >= count) break;
    }
    return result;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  typename ds::Skiplist<std::string, double>::RangeSpec spec{min, max, min_ex,
                                                             max_ex};
  if (!zs.skiplist.ScoreInRange(spec)) return result;
  auto* node = zs.skiplist.FirstInRange(spec);
  while (offset > 0 && node) {
    node = node->levels[0].forward;
    offset--;
    // Check if still in range
    if (node && (max_ex ? node->score >= max : node->score > max))
      node = nullptr;
  }
  while (node) {
    if (max_ex ? node->score >= max : node->score > max) break;
    result.push_back({node->key, node->score});
    if (count >= 0 && static_cast<long long>(result.size()) >= count) break;
    node = node->levels[0].forward;
  }
  return result;
}

std::vector<ZSetValue::RangeResult> ZSetValue::RangeByLex(
    std::string_view min, std::string_view max, bool min_ex, bool max_ex,
    long long offset, long long count) const {
  std::vector<RangeResult> result;
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto ele = lp->Get(i);
      auto score_str = lp->Get(i + 1);
      if (!ele || !score_str ||
          !ListpackMemberInLexRange(*ele, min, max, min_ex, max_ex)) {
        continue;
      }
      if (offset > 0) {
        offset--;
        continue;
      }
      auto parsed = ParseFiniteDouble(score_str->ToString());
      if (!std::holds_alternative<double>(parsed)) continue;
      result.push_back({ele->ToString(), std::get<double>(parsed)});
      if (count >= 0 && static_cast<long long>(result.size()) >= count) break;
    }
    return result;
  }

  // Lex operations scan all elements (require same score for correct semantics)
  auto all = Range(0, -1);
  long long skipped = 0;
  for (const auto& r : all) {
    bool in_range = true;
    if (min_ex ? r.element <= min : r.element < min) in_range = false;
    if (max_ex ? r.element >= max : r.element > max) in_range = false;
    if (in_range) {
      if (skipped < offset) {
        skipped++;
        continue;
      }
      result.push_back(r);
      if (count >= 0 && static_cast<long long>(result.size()) >= count) break;
    }
  }
  return result;
}

std::optional<ZSetValue::RangeResult> ZSetValue::PopMin() {
  if (Count() == 0) return std::nullopt;
  auto r = Range(0, 0);
  if (r.empty()) return std::nullopt;
  auto result = r[0];
  Remove(result.element);
  return result;
}

std::optional<ZSetValue::RangeResult> ZSetValue::PopMax() {
  if (Count() == 0) return std::nullopt;
  auto r = Range(-1, -1);
  if (r.empty()) return std::nullopt;
  auto result = r[0];
  Remove(result.element);
  return result;
}

size_t ZSetValue::RemoveRangeByRank(long long start, long long stop) {
  long long sz = static_cast<long long>(Count());
  if (start < 0) start = std::max(start + sz, 0LL);
  if (stop < 0) stop = std::max(stop + sz, 0LL);
  if (start > stop || start >= sz) return 0;
  stop = std::min(stop, sz - 1);
  size_t count = static_cast<size_t>(stop - start + 1);

  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    size_t removed = 0;
    for (size_t i = 0; i < count; i++) {
      lp->Delete(static_cast<size_t>(start * 2 + 1));  // score
      lp->Delete(static_cast<size_t>(start * 2));      // element
      removed++;
    }
    return removed;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  return zs.skiplist.DeleteRangeByRank(
      static_cast<size_t>(start + 1), static_cast<size_t>(stop + 1),
      [&](const std::string& key, double) { zs.dict.Delete(key); });
}

size_t ZSetValue::RemoveRangeByScore(double min, double max, bool min_ex,
                                     bool max_ex) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    size_t removed = 0;
    size_t i = 0;
    while (i < lp->Size()) {
      auto score_str = lp->Get(i + 1);
      if (!score_str) {
        i += 2;
        continue;
      }
      auto parsed = ParseFiniteDouble(score_str->ToString());
      if (!std::holds_alternative<double>(parsed)) {
        i += 2;
        continue;
      }
      double s = std::get<double>(parsed);
      bool in_range = true;
      if (min_ex ? s <= min : s < min) in_range = false;
      if (max_ex ? s >= max : s > max) {
        in_range = false; /* sorted, can break */
        if (removed > 0 || i == 0) break;
      }
      if (in_range) {
        lp->Delete(i + 1);
        lp->Delete(i);
        removed++;
      } else {
        i += 2;
      }
    }
    return removed;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  typename ds::Skiplist<std::string, double>::RangeSpec spec{min, max, min_ex,
                                                             max_ex};
  return zs.skiplist.DeleteRangeByScore(
      spec, [&](const std::string& key, double) { zs.dict.Delete(key); });
}

size_t ZSetValue::RemoveRangeByLex(std::string_view min, std::string_view max,
                                   bool min_ex, bool max_ex) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    size_t removed = 0;
    size_t i = 0;
    while (i < lp->Size()) {
      auto ele = lp->Get(i);
      if (!ele) {
        i += 2;
        continue;
      }
      if (ListpackMemberInLexRange(*ele, min, max, min_ex, max_ex)) {
        lp->Delete(i + 1);
        lp->Delete(i);
        removed++;
      } else {
        i += 2;
      }
    }
    return removed;
  }

  auto& zs = std::get<ZSetSkiplist>(encoding_);
  size_t removed = 0;
  auto* node = zs.skiplist.First();
  while (node) {
    auto* next = node->levels[0].forward;
    bool in_range = true;
    if (min_ex ? node->key <= min : node->key < min) in_range = false;
    if (max_ex ? node->key >= max : node->key > max) in_range = false;
    if (in_range) {
      zs.dict.Delete(node->key);
      zs.skiplist.DeleteNode(node);
      removed++;
    }
    node = next;
  }
  return removed;
}

void ZSetValue::Union(const ZSetValue& other, std::vector<double> weights,
                      const std::string& aggregate) {
  // Collect all elements with their weighted scores
  std::vector<std::pair<std::string, double>> all;
  // Insert our elements with weight
  double w_self = weights.empty() ? 1.0 : weights[0];
  auto self_range = Range(0, -1);
  for (const auto& r : self_range) {
    all.emplace_back(r.element, r.score * w_self);
  }
  // Insert other's elements with weight
  double w_other = weights.size() > 1 ? weights[1] : 1.0;
  auto other_range = other.Range(0, -1);
  for (const auto& r : other_range) {
    all.emplace_back(r.element, r.score * w_other);
  }

  // Aggregate
  std::sort(all.begin(), all.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  // Clear and rebuild (preserve thresholds)
  *this = ZSetValue(thresholds_);
  for (size_t i = 0; i < all.size();) {
    std::string elem = all[i].first;
    double score = all[i].second;
    size_t j = i + 1;
    while (j < all.size() && all[j].first == elem) {
      double s = all[j].second;
      if (aggregate == "SUM" || aggregate.empty())
        score += s;
      else if (aggregate == "MIN")
        score = std::min(score, s);
      else if (aggregate == "MAX")
        score = std::max(score, s);
      j++;
    }
    if (std::isfinite(score)) Add(elem, score);
    i = j;
  }
}

void ZSetValue::Intersect(const ZSetValue& other, std::vector<double> weights,
                          const std::string& aggregate) {
  // Only keep elements present in both sets
  std::vector<std::pair<std::string, double>> to_add;
  auto self_range = Range(0, -1);
  double w_self = weights.empty() ? 1.0 : weights[0];
  double w_other = weights.size() > 1 ? weights[1] : 1.0;

  for (const auto& r : self_range) {
    auto other_score = other.Score(r.element);
    if (!other_score) continue;  // not in other set

    double s_self = r.score * w_self;
    double s_other = *other_score * w_other;
    double score;
    if (aggregate == "SUM" || aggregate.empty())
      score = s_self + s_other;
    else if (aggregate == "MIN")
      score = std::min(s_self, s_other);
    else if (aggregate == "MAX")
      score = std::max(s_self, s_other);
    else
      score = s_self + s_other;

    if (std::isfinite(score)) to_add.emplace_back(r.element, score);
  }

  *this = ZSetValue(thresholds_);
  for (const auto& p : to_add) Add(p.first, p.second);
}

ValueEncoding ZSetValue::Encoding() const {
  if (std::holds_alternative<ds::Listpack>(encoding_))
    return ValueEncoding::kZSetListpack;
  return ValueEncoding::kSkiplist;
}

std::vector<ZSetValue::RangeResult> ZSetValue::Scan(size_t cursor,
                                                    size_t count) const {
  return Range(static_cast<long long>(cursor),
               static_cast<long long>(cursor + count - 1));
}

void ZSetValue::MaybeUpgrade(size_t new_element_size) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    bool over_entries =
        (lp->Size() / 2) > thresholds_.zset_max_listpack_entries;
    bool over_value = new_element_size > thresholds_.zset_max_listpack_value;
    if (over_entries || over_value) {
      ConvertToSkiplist();
    }
  }
}

void ZSetValue::ConvertToSkiplist() {
  auto* lp = std::get_if<ds::Listpack>(&encoding_);
  if (!lp) return;
  ds::Listpack lp_copy = std::move(*lp);
  encoding_ = ListpackToZSetSkiplist(std::move(lp_copy));
}

}  // namespace miniredis
