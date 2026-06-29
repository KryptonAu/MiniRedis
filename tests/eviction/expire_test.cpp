#include "eviction/expire.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(ExpireTest, Placeholder) {
  ActiveExpireConfig config;
  EXPECT_EQ(config.keys_per_loop, 20);
}

}  // namespace
}  // namespace miniredis
