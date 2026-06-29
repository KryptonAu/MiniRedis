#include "persistence/crc64.h"

#include <gtest/gtest.h>

#include <string>

namespace miniredis {
namespace {

TEST(Crc64Test, SameInputSameOutput) {
  std::string data = "hello";
  uint64_t a = Crc64(data.data(), data.size(), 0);
  uint64_t b = Crc64(data.data(), data.size(), 0);
  EXPECT_EQ(a, b);
}

}  // namespace
}  // namespace miniredis
