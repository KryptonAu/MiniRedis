#include "types/string_value.h"

#include <algorithm>
#include <limits>

#include "types/numeric_parse.h"

namespace miniredis {

StringValue::StringValue() : value_(static_cast<int64_t>(0)) {}

StringValue::StringValue(std::string_view value) { Set(value); }

StringValue::StringValue(int64_t value) : value_(value) {}

std::string StringValue::ToString() const {
  if (auto* i = std::get_if<int64_t>(&value_)) {
    return std::to_string(*i);
  }
  return std::get<std::string>(value_);
}

std::optional<std::string_view> StringValue::AsString() const {
  if (auto* s = std::get_if<std::string>(&value_)) {
    return *s;
  }
  return std::nullopt;
}

std::optional<int64_t> StringValue::AsInt() const {
  if (auto* i = std::get_if<int64_t>(&value_)) {
    return *i;
  }
  return std::nullopt;
}

ValueEncoding StringValue::Encoding() const {
  if (std::holds_alternative<int64_t>(value_)) {
    return ValueEncoding::kIntEmbed;
  }
  return ValueEncoding::kRaw;
}

size_t StringValue::Length() const { return ToString().size(); }

void StringValue::Set(std::string_view value) {
  auto parsed = ParseCanonicalInt(value);
  if (std::holds_alternative<ParsedInt>(parsed)) {
    value_ = std::get<ParsedInt>(parsed).value;
  } else {
    value_ = std::string(value);
  }
}

void StringValue::Append(std::string_view suffix) {
  std::string s = ToString();
  s.append(suffix);
  value_ = std::move(s);
}

void StringValue::SetRange(size_t offset, std::string_view value) {
  std::string s = ToString();
  if (offset > s.size()) {
    s.resize(offset, '\0');
  }
  if (offset + value.size() > s.size()) {
    s.resize(offset + value.size(), '\0');
  }
  std::copy(value.begin(), value.end(), s.begin() + static_cast<long>(offset));
  Set(std::string_view(s));
}

std::string StringValue::GetRange(long long start, long long end) const {
  std::string s = ToString();
  long long len = static_cast<long long>(s.size());
  if (len == 0) return "";

  if (start < 0) start = std::max(start + len, 0LL);
  if (end < 0) end = std::max(end + len, 0LL);

  if (start > end || start >= len) return "";

  end = std::min(end, len - 1);
  return s.substr(static_cast<size_t>(start),
                  static_cast<size_t>(end - start + 1));
}

TypeResult<int64_t> StringValue::IncrementBy(int64_t delta) {
  int64_t current;
  if (auto* i = std::get_if<int64_t>(&value_)) {
    current = *i;
  } else {
    auto parsed = ParseCanonicalInt(ToString());
    if (std::holds_alternative<TypeError>(parsed)) {
      return std::get<TypeError>(parsed);
    }
    current = std::get<ParsedInt>(parsed).value;
  }

  auto result = AddChecked(current, delta);
  if (std::holds_alternative<TypeError>(result)) {
    return std::get<TypeError>(result);
  }

  int64_t new_val = std::get<int64_t>(result);
  value_ = new_val;
  return new_val;
}

TypeResult<double> StringValue::IncrementByFloat(double delta) {
  std::string s = ToString();
  auto parsed = ParseFiniteDouble(s);
  if (std::holds_alternative<TypeError>(parsed)) {
    return std::get<TypeError>(parsed);
  }
  double current = std::get<double>(parsed);

  auto result = AddFiniteDouble(current, delta);
  if (std::holds_alternative<TypeError>(result)) {
    return std::get<TypeError>(result);
  }

  double new_val = std::get<double>(result);
  value_ = FormatDoubleForStorage(new_val);
  return new_val;
}

bool StringValue::operator==(const StringValue& other) const {
  return ToString() == other.ToString();
}

}  // namespace miniredis
