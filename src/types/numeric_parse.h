#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "types/operation_result.h"

namespace miniredis {

struct ParsedInt {
  int64_t value;
};

TypeResult<ParsedInt> ParseCanonicalInt(std::string_view input);
TypeResult<int64_t> AddChecked(int64_t lhs, int64_t rhs);
TypeResult<double> ParseFiniteDouble(std::string_view input);
TypeResult<double> AddFiniteDouble(double lhs, double rhs);
std::string FormatDoubleForStorage(double value);

}  // namespace miniredis
