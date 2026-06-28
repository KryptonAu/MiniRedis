#include "commands/command_helpers.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(CommandHelpersTest, MsToSecRoundsUpPositiveTtl) {
  EXPECT_EQ(MsToSec(1), 1);
  EXPECT_EQ(MsToSec(999), 1);
  EXPECT_EQ(MsToSec(1000), 1);
  EXPECT_EQ(MsToSec(1001), 2);
}

TEST(CommandHelpersTest, ZSetRangeUsesStableScoreFormatting) {
  std::vector<ZSetValue::RangeResult> values = {{"member", 1.0}};

  EXPECT_EQ(ArrayOfZSetRange(values, true),
            "*2\r\n$6\r\nmember\r\n$3\r\n1.0\r\n");
}

}  // namespace
}  // namespace miniredis
