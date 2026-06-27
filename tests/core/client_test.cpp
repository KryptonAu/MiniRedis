#include "core/client.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(ClientTest, Create) {
  Client c(5);
  EXPECT_EQ(c.Fd(), 5);
  EXPECT_GT(c.Id(), 0);
  EXPECT_EQ(c.CurrentDb(), 0);
}

TEST(ClientTest, SelectDb) {
  Client c(1, 2);
  EXPECT_EQ(c.CurrentDb(), 2);
  EXPECT_TRUE(c.SelectDb(5, 10));
  EXPECT_EQ(c.CurrentDb(), 5);
  EXPECT_FALSE(c.SelectDb(-1, 10));
  EXPECT_FALSE(c.SelectDb(10, 10));
}

}  // namespace
}  // namespace miniredis
