#include "core/database.h"

#include <gtest/gtest.h>

namespace miniredis {
namespace {

TEST(DatabaseTest, SetAndFind) {
  Database db;
  EXPECT_TRUE(db.Set("key", StringValue("value")));
  auto* val = db.Find("key");
  ASSERT_NE(val, nullptr);
}

TEST(DatabaseTest, TTL) {
  Database db;
  db.Set("key", StringValue("val"));
  EXPECT_EQ(db.TTL("key"), -1);
  EXPECT_EQ(db.TTL("missing"), -2);
  db.SetExpire("key", 9999999999999LL);
  EXPECT_GT(db.TTL("key"), 0);
}

TEST(DatabaseTest, SetClearsTTL) {
  Database db;
  db.Set("key", StringValue("val"));
  db.SetExpire("key", 9999999999999LL);
  db.Set("key", StringValue("new"));
  EXPECT_EQ(db.ExpiresSize(), 0);
}

TEST(DatabaseTest, Rename) {
  Database db;
  db.Set("old", StringValue("val"));
  EXPECT_TRUE(db.Rename("old", "new"));
  EXPECT_FALSE(db.Exists("old"));
  EXPECT_TRUE(db.Exists("new"));
}

// Regression: Persist on expired key without prior Exists must not resurrect
TEST(DatabaseTest, PersistOnExpiredReturnsFalse) {
  Database db;
  db.Set("key", StringValue("val"));
  db.SetExpire("key", 0);  // past → expired
  // Direct Persist without prior access — must expire first and return false
  EXPECT_FALSE(db.Persist("key"));
  EXPECT_FALSE(db.Exists("key"));
}

// Regression: SetExpire on expired key without prior Exists must not resurrect
TEST(DatabaseTest, SetExpireOnExpiredReturnsFalse) {
  Database db;
  db.Set("key", StringValue("val"));
  db.SetExpire("key", 0);  // past → expired
  // Direct SetExpire without prior access — must expire first and return false
  EXPECT_FALSE(db.SetExpire("key", 9999999999999LL));
  EXPECT_FALSE(db.Exists("key"));
}

TEST(DatabaseTest, PurgeExpiredKeysUsesCurrentIteratorEntry) {
  Database db;
  db.Set("expired", StringValue("old"));
  db.SetExpire("expired", 100);
  db.Set("live", StringValue("new"));
  db.SetExpire("live", 9999999999999LL);

  EXPECT_EQ(db.PurgeExpiredKeys(500), 1u);
  EXPECT_FALSE(db.Exists("expired"));
  EXPECT_TRUE(db.Exists("live"));
  EXPECT_EQ(db.ExpiresSize(), 1u);
}

}  // namespace
}  // namespace miniredis
