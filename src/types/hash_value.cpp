#include "types/hash_value.h"

#include "types/numeric_parse.h"
#include "types/type_conversion.h"

namespace miniredis {

HashValue::HashValue(EncodingThresholds thresholds) : thresholds_(thresholds) {
  encoding_ = ds::Listpack{};
}

bool HashValue::Set(std::string_view field, std::string_view value) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    // Scan even indices for existing field
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto f = lp->Get(i);
      if (f && f->ToString() == field) {
        // Update existing value — must check value length threshold
        if (value.size() > thresholds_.hash_max_listpack_value) {
          ConvertToHashtable();
          // Re-do the set with new encoding
          auto& ht = std::get<Hashtable>(encoding_);
          ht.SetView(field, std::string(value));
          return false;
        }
        lp->Replace(i + 1, value);
        return false;
      }
    }
    // New field — check upgrade before inserting
    MaybeUpgrade(field.size(), value.size());
    // After MaybeUpgrade, encoding may have changed to hashtable
    if (auto* still_lp = std::get_if<ds::Listpack>(&encoding_)) {
      still_lp->Append(field);
      still_lp->Append(value);
      return true;
    }
    // Fall through to Dict encoding add
  }

  // Dict encoding
  auto& ht = std::get<Hashtable>(encoding_);
  bool existed = ht.FindView(field) != nullptr;
  ht.SetView(field, std::string(value));
  return !existed;
}

bool HashValue::SetNX(std::string_view field, std::string_view value) {
  if (Exists(field)) return false;
  Set(field, value);
  return true;
}

std::optional<std::string> HashValue::Get(std::string_view field) const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto f = lp->Get(i);
      if (f && f->ToString() == field) {
        auto v = lp->Get(i + 1);
        if (v) return v->ToString();
        return std::nullopt;
      }
    }
    return std::nullopt;
  }
  const auto* val = std::get<Hashtable>(encoding_).FindView(field);
  if (val) return *val;
  return std::nullopt;
}

bool HashValue::Delete(std::string_view field) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto f = lp->Get(i);
      if (f && f->ToString() == field) {
        lp->Delete(i + 1);  // Delete value first (to avoid index shift)
        lp->Delete(i);      // Delete field
        return true;
      }
    }
    return false;
  }
  return std::get<Hashtable>(encoding_).DeleteView(field);
}

bool HashValue::Exists(std::string_view field) const {
  return Get(field).has_value();
}

std::vector<std::optional<std::string>> HashValue::MGet(
    const std::vector<std::string>& fields) const {
  std::vector<std::optional<std::string>> result;
  result.reserve(fields.size());
  for (const auto& f : fields) {
    result.push_back(Get(f));
  }
  return result;
}

size_t HashValue::Size() const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) return lp->Size() / 2;
  return std::get<Hashtable>(encoding_).Size();
}

size_t HashValue::FieldLength(std::string_view field) const {
  auto val = Get(field);
  return val ? val->size() : 0;
}

ValueEncoding HashValue::Encoding() const {
  if (std::holds_alternative<ds::Listpack>(encoding_))
    return ValueEncoding::kListpack;
  return ValueEncoding::kHashHT;
}

std::vector<std::string> HashValue::Keys() const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto f = lp->Get(i);
      if (f) keys.push_back(f->ToString());
    }
    return keys;
  }
  std::vector<std::string> keys;
  auto& ht = const_cast<Hashtable&>(std::get<Hashtable>(encoding_));
  for (auto it = ht.begin(); it != ht.end(); ++it) {
    keys.push_back(it->key);
  }
  return keys;
}

std::vector<std::string> HashValue::Values() const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    std::vector<std::string> values;
    for (size_t i = 1; i < lp->Size(); i += 2) {
      auto v = lp->Get(i);
      if (v) values.push_back(v->ToString());
    }
    return values;
  }
  std::vector<std::string> values;
  auto& ht = const_cast<Hashtable&>(std::get<Hashtable>(encoding_));
  for (auto it = ht.begin(); it != ht.end(); ++it) {
    values.push_back(it->value);
  }
  return values;
}

std::vector<std::pair<std::string, std::string>> HashValue::GetAll() const {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    std::vector<std::pair<std::string, std::string>> result;
    for (size_t i = 0; i < lp->Size(); i += 2) {
      auto f = lp->Get(i);
      auto v = lp->Get(i + 1);
      if (f && v) result.emplace_back(f->ToString(), v->ToString());
    }
    return result;
  }
  std::vector<std::pair<std::string, std::string>> result;
  auto& ht = const_cast<Hashtable&>(std::get<Hashtable>(encoding_));
  for (auto it = ht.begin(); it != ht.end(); ++it) {
    result.emplace_back(it->key, it->value);
  }
  return result;
}

TypeResult<int64_t> HashValue::IncrementBy(std::string_view field,
                                           int64_t delta) {
  auto val = Get(field);
  int64_t current = 0;
  if (val) {
    auto parsed = ParseCanonicalInt(*val);
    if (std::holds_alternative<TypeError>(parsed)) {
      return std::get<TypeError>(parsed);
    }
    current = std::get<ParsedInt>(parsed).value;
  }

  auto checked = AddChecked(current, delta);
  if (std::holds_alternative<TypeError>(checked)) {
    return std::get<TypeError>(checked);
  }

  int64_t new_val = std::get<int64_t>(checked);
  Set(field, std::to_string(new_val));
  return new_val;
}

TypeResult<double> HashValue::IncrementByFloat(std::string_view field,
                                               double delta) {
  auto val = Get(field);
  double current = 0.0;
  if (val) {
    auto parsed = ParseFiniteDouble(*val);
    if (std::holds_alternative<TypeError>(parsed)) {
      return std::get<TypeError>(parsed);
    }
    current = std::get<double>(parsed);
  }

  auto result = AddFiniteDouble(current, delta);
  if (std::holds_alternative<TypeError>(result)) {
    return std::get<TypeError>(result);
  }

  double new_val = std::get<double>(result);
  Set(field, FormatDoubleForStorage(new_val));
  return new_val;
}

std::optional<std::string> HashValue::RandomField() const {
  if (Size() == 0) return std::nullopt;
  auto keys = Keys();
  if (keys.empty()) return std::nullopt;
  return keys[0];  // simple, not truly random
}

std::vector<std::string> HashValue::Scan(size_t cursor, size_t count) const {
  auto all = Keys();
  if (cursor >= all.size()) return {};
  size_t end = std::min(cursor + count, all.size());
  return {all.begin() + static_cast<long>(cursor),
          all.begin() + static_cast<long>(end)};
}

void HashValue::MaybeUpgrade(size_t new_field_size, size_t new_value_size) {
  if (auto* lp = std::get_if<ds::Listpack>(&encoding_)) {
    bool over_entries =
        (lp->Size() / 2 + 1) > thresholds_.hash_max_listpack_entries;
    bool over_value = new_field_size > thresholds_.hash_max_listpack_value ||
                      new_value_size > thresholds_.hash_max_listpack_value;
    if (over_entries || over_value) {
      ConvertToHashtable();
    }
  }
}

void HashValue::ConvertToHashtable() {
  auto* lp = std::get_if<ds::Listpack>(&encoding_);
  if (!lp) return;
  encoding_ = ListpackToHashDict(*lp);
}

}  // namespace miniredis
