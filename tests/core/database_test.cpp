#include "core/database.h"

#include <gtest/gtest.h>

#include <string_view>

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
  EXPECT_EQ(db.ExpiresSize(), 1u);
  db.Set("key", StringValue("new"));
  EXPECT_EQ(db.ExpiresSize(), 0u);
  EXPECT_EQ(db.TTL("key"), -1);
}

TEST(DatabaseTest, Rename) {
  Database db;
  db.Set("old", StringValue("val"));
  EXPECT_TRUE(db.Rename("old", "new"));
  EXPECT_FALSE(db.Exists("old"));
  EXPECT_TRUE(db.Exists("new"));
}

TEST(DatabaseTest, RenamePreservesTtlAndLruAndAccountsForTargetTtl) {
  Database db;
  constexpr int64_t kOldExpire = 9999999999999LL;

  db.SetCurrentLruClock(10);
  db.Set("old", StringValue("old"));
  db.SetExpire("old", kOldExpire);

  db.SetCurrentLruClock(20);
  db.Set("new", StringValue("new"));
  db.SetExpire("new", 8888888888888LL);
  ASSERT_EQ(db.ExpiresSize(), 2u);

  EXPECT_TRUE(db.Rename("old", "new"));

  EXPECT_FALSE(db.Exists("old"));
  EXPECT_TRUE(db.Exists("new"));
  EXPECT_EQ(db.ExpiresSize(), 1u);
  EXPECT_EQ(db.ExpireAt("new"), kOldExpire);
  EXPECT_EQ(db.LruOf("new"), 10u);
}

TEST(DatabaseTest, DeletePersistAndLazyExpireMaintainExpiresSize) {
  Database db;
  db.Set("delete", StringValue("value"));
  db.SetExpire("delete", 9999999999999LL);
  db.Set("persist", StringValue("value"));
  db.SetExpire("persist", 9999999999999LL);
  db.Set("expired", StringValue("value"));
  db.SetExpire("expired", 0);
  ASSERT_EQ(db.ExpiresSize(), 3u);

  EXPECT_TRUE(db.Delete("delete"));
  EXPECT_EQ(db.ExpiresSize(), 2u);

  EXPECT_TRUE(db.Persist("persist"));
  EXPECT_EQ(db.ExpiresSize(), 1u);

  EXPECT_EQ(db.Find("expired"), nullptr);
  EXPECT_EQ(db.ExpiresSize(), 0u);
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

TEST(DatabaseTest, StringViewKeyOperationsUseSubviewContents) {
  Database db;
  std::string source = "xxalphayy";
  std::string_view key(source.data() + 2, 5);

  EXPECT_TRUE(db.Set(key, StringValue("value")));
  EXPECT_NE(db.Find(key), nullptr);

  EXPECT_TRUE(db.SetExpire(key, 9999999999999LL));
  EXPECT_GT(db.TTL(key), 0);
  EXPECT_TRUE(db.Persist(key));
  EXPECT_EQ(db.TTL(key), -1);

  EXPECT_TRUE(db.SetExpire(key, 0));
  EXPECT_TRUE(db.IsExpired(key));
  EXPECT_EQ(db.Find(key), nullptr);

  EXPECT_TRUE(db.Set(key, StringValue("value")));
  EXPECT_TRUE(db.Delete(key));
  EXPECT_FALSE(db.Exists(key));
}

TEST(DatabaseTest, RenameNXUsesStringViewTargetLookup) {
  Database db;
  std::string old_source = "__old__";
  std::string new_source = "xxnewyy";
  std::string_view old_key(old_source.data() + 2, 3);
  std::string_view new_key(new_source.data() + 2, 3);

  EXPECT_TRUE(db.Set(old_key, StringValue("old")));
  EXPECT_TRUE(db.Set(new_key, StringValue("new")));
  EXPECT_FALSE(db.RenameNX(old_key, new_key));
  EXPECT_TRUE(db.Exists(old_key));
  EXPECT_TRUE(db.Exists(new_key));

  EXPECT_TRUE(db.Delete(new_key));
  EXPECT_TRUE(db.RenameNX(old_key, new_key));
  EXPECT_FALSE(db.Exists(old_key));
  EXPECT_TRUE(db.Exists(new_key));
}

}  // namespace
}  // namespace miniredis
