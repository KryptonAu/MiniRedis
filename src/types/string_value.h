#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "types/operation_result.h"
#include "types/value_fwd.h"

namespace miniredis {

class StringValue {
 public:
  using Storage = std::variant<int64_t, std::string>;

  StringValue();
  explicit StringValue(std::string_view value);
  explicit StringValue(int64_t value);

  // Move-only
  StringValue(const StringValue&) = delete;
  StringValue& operator=(const StringValue&) = delete;
  StringValue(StringValue&&) noexcept = default;
  StringValue& operator=(StringValue&&) noexcept = default;

  std::string ToString() const;
  std::optional<std::string_view> AsString() const;
  std::optional<int64_t> AsInt() const;
  ValueEncoding Encoding() const;
  size_t Length() const;

  void Set(std::string_view value);
  void Append(std::string_view suffix);
  void SetRange(size_t offset, std::string_view value);
  std::string GetRange(long long start, long long end) const;

  TypeResult<int64_t> IncrementBy(int64_t delta);
  TypeResult<double> IncrementByFloat(double delta);

  bool operator==(const StringValue& other) const;

 private:
  Storage value_;
};

}  // namespace miniredis
