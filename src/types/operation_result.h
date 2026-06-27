#pragma once

#include <cstdint>
#include <variant>

namespace miniredis {

enum class TypeError : uint8_t {
  kInvalidInteger,
  kIntegerOverflow,
  kInvalidFloat,
  kInvalidScore,
  kOutOfRange,
};

template <typename T>
using TypeResult = std::variant<T, TypeError>;

}  // namespace miniredis
