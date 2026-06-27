#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ds/dict.h"
#include "ds/intset.h"
#include "types/encoding_thresholds.h"
#include "types/value_fwd.h"

namespace miniredis {

class SetValue {
 public:
  using Hashtable = ds::Dict<std::string, std::monostate>;
  using Storage = std::variant<ds::Intset, Hashtable>;

  explicit SetValue(EncodingThresholds thresholds = {});

  SetValue(const SetValue&) = delete;
  SetValue& operator=(const SetValue&) = delete;
  SetValue(SetValue&&) noexcept = default;
  SetValue& operator=(SetValue&&) noexcept = default;

  bool Add(std::string_view member);
  bool Remove(std::string_view member);
  bool Contains(std::string_view member) const;
  std::optional<std::string> Pop();

  size_t Size() const;
  ValueEncoding Encoding() const;
  std::vector<std::string> Members() const;
  std::optional<std::string> RandomMember() const;

  SetValue Union(const SetValue& other) const;
  SetValue Intersect(const SetValue& other) const;
  SetValue Difference(const SetValue& other) const;

  static bool Move(SetValue& from, SetValue& to, std::string_view member);

  std::vector<std::string> Scan(size_t cursor, size_t count) const;

 private:
  Storage encoding_;
  EncodingThresholds thresholds_;

  void MaybeUpgrade();
};

}  // namespace miniredis
