#include "types/numeric_parse.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

namespace miniredis {

TypeResult<ParsedInt> ParseCanonicalInt(std::string_view input) {
  if (input.empty()) return TypeError::kInvalidInteger;

  // Single digit "0"
  if (input == "0") return ParsedInt{0};

  // Reject non-canonical forms: leading zeros, leading plus, "-0"
  if (input[0] == '0' || input[0] == '+') return TypeError::kInvalidInteger;
  if (input.size() >= 2 && input[0] == '-' && input[1] == '0')
    return TypeError::kInvalidInteger;

  int64_t value = 0;
  bool negative = false;
  size_t pos = 0;

  if (input[pos] == '-') {
    negative = true;
    pos++;
    if (pos >= input.size()) return TypeError::kInvalidInteger;
  }

  // First digit must be 1-9 (already handled "0" case via size check above)
  if (input[pos] < '1' || input[pos] > '9') return TypeError::kInvalidInteger;

  while (pos < input.size()) {
    char c = input[pos];
    if (c < '0' || c > '9') return TypeError::kInvalidInteger;

    int64_t digit = c - '0';
    if (negative) {
      // Check for underflow
      if (value < (std::numeric_limits<int64_t>::min() + digit) / 10) {
        return TypeError::kIntegerOverflow;
      }
      value = value * 10 - digit;
    } else {
      // Check for overflow
      if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
        return TypeError::kIntegerOverflow;
      }
      value = value * 10 + digit;
    }
    pos++;
  }

  // "0" single digit has been handled above
  // If this is just "-" followed by nothing, it's invalid
  // Also, if value is 0 and input was negative, that means "-0" which was
  // already rejected

  return ParsedInt{value};
}

TypeResult<int64_t> AddChecked(int64_t lhs, int64_t rhs) {
  int64_t result;
  if (__builtin_add_overflow(lhs, rhs, &result)) {
    return TypeError::kIntegerOverflow;
  }
  return result;
}

TypeResult<double> ParseFiniteDouble(std::string_view input) {
  if (input.empty()) return TypeError::kInvalidFloat;

  // Manual parsing for exact control
  // Use std::from_chars when available, fall back to strtod
  double value = 0.0;
  auto [ptr, ec] =
      std::from_chars(input.data(), input.data() + input.size(), value);

  if (ec != std::errc() || ptr != input.data() + input.size()) {
    return TypeError::kInvalidFloat;
  }

  if (!std::isfinite(value)) {
    return TypeError::kInvalidFloat;
  }

  return value;
}

TypeResult<double> AddFiniteDouble(double lhs, double rhs) {
  double result = lhs + rhs;
  if (!std::isfinite(result)) {
    return TypeError::kInvalidFloat;
  }
  return result;
}

std::string FormatDoubleForStorage(double value) {
  // Use a stable format that preserves round-trip
  // For score storage, we use the %.17g format which is the precision
  // needed to uniquely identify any double
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "%.17g", value);
  if (len < 0) return std::to_string(value);
  // Ensure it looks like a finite number (no "nan", "inf")
  std::string result(buf, static_cast<size_t>(len));
  // If the result doesn't contain a decimal point and isn't scientific
  // notation, append ".0" to make it clearly a double (not strictly required
  // but helps)
  if (result.find('.') == std::string::npos &&
      result.find('e') == std::string::npos &&
      result.find('E') == std::string::npos) {
    result += ".0";
  }
  return result;
}

}  // namespace miniredis
