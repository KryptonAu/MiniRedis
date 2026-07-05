#include "types/set_value.h"

#include "types/numeric_parse.h"
#include "types/type_conversion.h"

namespace miniredis {

SetValue::SetValue(EncodingThresholds thresholds) : thresholds_(thresholds) {
  encoding_ = ds::Intset{};
}

bool SetValue::Add(std::string_view member) {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    auto parsed = ParseCanonicalInt(member);
    if (std::holds_alternative<ParsedInt>(parsed)) {
      int64_t v = std::get<ParsedInt>(parsed).value;
      if (!is->Add(v)) return false;
    } else {
      Hashtable ht = IntsetToSetHashtable(*is);
      ht.AddView(member, {});
      encoding_ = std::move(ht);
      return true;
    }
    MaybeUpgrade();
    return true;
  }
  auto& ht = std::get<Hashtable>(encoding_);
  return ht.AddView(member, {});
}

bool SetValue::Remove(std::string_view member) {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    auto parsed = ParseCanonicalInt(member);
    if (std::holds_alternative<ParsedInt>(parsed)) {
      return is->Remove(std::get<ParsedInt>(parsed).value);
    }
    return false;
  }
  return std::get<Hashtable>(encoding_).DeleteView(member);
}

bool SetValue::Contains(std::string_view member) const {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    auto parsed = ParseCanonicalInt(member);
    if (std::holds_alternative<ParsedInt>(parsed)) {
      return is->Contains(std::get<ParsedInt>(parsed).value);
    }
    return false;
  }
  return std::get<Hashtable>(encoding_).FindView(member) != nullptr;
}

std::optional<std::string> SetValue::Pop() {
  auto member = RandomMember();
  if (member) Remove(*member);
  return member;
}

size_t SetValue::Size() const {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) return is->Size();
  return std::get<Hashtable>(encoding_).Size();
}

ValueEncoding SetValue::Encoding() const {
  if (std::holds_alternative<ds::Intset>(encoding_))
    return ValueEncoding::kIntset;
  return ValueEncoding::kHashtable;
}

std::vector<std::string> SetValue::Members() const {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    std::vector<std::string> result;
    for (auto v : is->Values()) result.push_back(std::to_string(v));
    return result;
  }
  std::vector<std::string> result;
  auto& ht = const_cast<Hashtable&>(std::get<Hashtable>(encoding_));
  for (auto it = ht.begin(); it != ht.end(); ++it) {
    result.push_back(it->key);
  }
  return result;
}

std::optional<std::string> SetValue::RandomMember() const {
  if (Size() == 0) return std::nullopt;
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    auto v = is->Get(0);
    if (v) return std::to_string(*v);
    return std::nullopt;
  }
  auto& ht = const_cast<Hashtable&>(std::get<Hashtable>(encoding_));
  auto key = ht.RandomKey();
  if (key) return *key;
  return std::nullopt;
}

SetValue SetValue::Union(const SetValue& other) const {
  SetValue result(thresholds_);
  for (const auto& m : Members()) result.Add(m);
  for (const auto& m : other.Members()) result.Add(m);
  return result;
}

SetValue SetValue::Intersect(const SetValue& other) const {
  SetValue result(thresholds_);
  for (const auto& m : Members()) {
    if (other.Contains(m)) result.Add(m);
  }
  return result;
}

SetValue SetValue::Difference(const SetValue& other) const {
  SetValue result(thresholds_);
  for (const auto& m : Members()) {
    if (!other.Contains(m)) result.Add(m);
  }
  return result;
}

bool SetValue::Move(SetValue& from, SetValue& to, std::string_view member) {
  if (!from.Contains(member)) return false;
  from.Remove(member);
  to.Add(member);
  return true;
}

std::vector<std::string> SetValue::Scan(size_t cursor, size_t count) const {
  auto all = Members();
  if (cursor >= all.size()) return {};
  size_t end = std::min(cursor + count, all.size());
  return {all.begin() + static_cast<long>(cursor),
          all.begin() + static_cast<long>(end)};
}

void SetValue::MaybeUpgrade() {
  if (auto* is = std::get_if<ds::Intset>(&encoding_)) {
    if (is->Size() > thresholds_.set_max_intset_entries) {
      Hashtable ht = IntsetToSetHashtable(*is);
      encoding_ = std::move(ht);
    }
  }
}

}  // namespace miniredis
