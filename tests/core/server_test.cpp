#include "core/server.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(ServerTest, InitAndDbCount) {
  Server& srv = Server::Instance();
  MiniRedisConfig cfg;
  cfg.databases = 8;
  EXPECT_TRUE(srv.Init(cfg));
  EXPECT_EQ(srv.DbCount(), 8);
}

TEST(ServerTest, CreateAndFindClient) {
  Server& srv = Server::Instance();
  MiniRedisConfig cfg;
  cfg.databases = 4;
  srv.Init(cfg);

  auto* c = srv.CreateClient(1);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(srv.FindClient(1), c);
  EXPECT_EQ(srv.ClientCount(), 1);

  // Duplicate fd rejected
  EXPECT_EQ(srv.CreateClient(1), nullptr);

  srv.RemoveClient(1);
  EXPECT_EQ(srv.FindClient(1), nullptr);
}

TEST(ServerTest, Shutdown) {
  Server& srv = Server::Instance();
  srv.Shutdown();
  EXPECT_FALSE(srv.IsRunning());
  // Re-init works
  MiniRedisConfig cfg;
  cfg.databases = 2;
  EXPECT_TRUE(srv.Init(cfg));
}

}  // namespace
}  // namespace miniredis
