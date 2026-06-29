#pragma once

#include <cstddef>
#include <cstdint>

namespace miniredis {

uint64_t Crc64(const void* data, size_t len, uint64_t init = 0);

}  // namespace miniredis
