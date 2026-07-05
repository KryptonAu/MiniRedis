#include "types/hash_value.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace miniredis {
namespace {

TEST(HashValueTest, ConstructEmpty) {
  HashValue hv;
  EXPECT_EQ(hv.Size(), 0);
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kListpack);
}

TEST(HashValueTest, SetAndGet) {
  HashValue hv;
  EXPECT_TRUE(hv.Set("key1", "val1"));
  EXPECT_EQ(hv.Size(), 1);
  EXPECT_EQ(hv.Get("key1").value(), "val1");
  EXPECT_FALSE(hv.Get("missing").has_value());
}

TEST(HashValueTest, SetNX) {
  HashValue hv;
  EXPECT_TRUE(hv.SetNX("key", "first"));
  EXPECT_FALSE(hv.SetNX("key", "second"));
  EXPECT_EQ(hv.Get("key").value(), "first");
}

TEST(HashValueTest, Delete) {
  HashValue hv;
  hv.Set("a", "1");
  hv.Set("b", "2");
  EXPECT_TRUE(hv.Delete("a"));
  EXPECT_EQ(hv.Size(), 1);
  EXPECT_FALSE(hv.Exists("a"));
}

TEST(HashValueTest, UpgradeOnThreshold) {
  EncodingThresholds t;
  t.hash_max_listpack_entries = 5;
  HashValue hv(t);
  for (int i = 0; i < 6; i++) {
    hv.Set(std::to_string(i), "v");
  }
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kHashHT);
  EXPECT_EQ(hv.Size(), 6);
  EXPECT_EQ(hv.Get("3").value(), "v");
}

TEST(HashValueTest, KeysAndValues) {
  HashValue hv;
  hv.Set("a", "1");
  hv.Set("b", "2");
  EXPECT_EQ(hv.Keys().size(), 2);
  EXPECT_EQ(hv.Values().size(), 2);
}

TEST(HashValueTest, IncrementBy) {
  HashValue hv;
  hv.Set("counter", "10");
  auto r = hv.IncrementBy("counter", 5);
  ASSERT_TRUE(std::holds_alternative<int64_t>(r));
  EXPECT_EQ(std::get<int64_t>(r), 15);
  EXPECT_EQ(hv.Get("counter").value(), "15");
}

TEST(HashValueTest, IncrementByNewField) {
  HashValue hv;
  auto r = hv.IncrementBy("new", 5);
  ASSERT_TRUE(std::holds_alternative<int64_t>(r));
  EXPECT_EQ(std::get<int64_t>(r), 5);
}

TEST(HashValueTest, IncrementByNonInteger) {
  HashValue hv;
  hv.Set("x", "abc");
  auto r = hv.IncrementBy("x", 1);
  EXPECT_TRUE(std::holds_alternative<TypeError>(r));
}

// Update existing field with value exceeding threshold must trigger upgrade
TEST(HashValueTest, UpdateFieldWithLongValueTriggersUpgrade) {
  EncodingThresholds t;
  t.hash_max_listpack_value = 10;
  HashValue hv(t);
  hv.Set("key", "short");
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kListpack);
  hv.Set("key", std::string(20, 'x'));
  EXPECT_EQ(hv.Encoding(), ValueEncoding::kHashHT);
  EXPECT_EQ(hv.Get("key").value(), std::string(20, 'x'));
}

TEST(HashValueTest, HashtableEncodingAcceptsSubviewFields) {
  EncodingThresholds t;
  t.hash_max_listpack_entries = 1;
  HashValue hv(t);
  hv.Set("seed", "0");

  std::string field_source = "__alpha__";
  std::string_view field(field_source.data() + 2, 5);
  EXPECT_TRUE(hv.Set(field, "1"));
  ASSERT_EQ(hv.Encoding(), ValueEncoding::kHashHT);
  field_source[2] = 'X';

  std::string lookup_source = "zzalphazz";
  std::string_view lookup(lookup_source.data() + 2, 5);
  EXPECT_TRUE(hv.Exists(lookup));
  ASSERT_TRUE(hv.Get(lookup).has_value());
  EXPECT_EQ(hv.Get(lookup).value(), "1");

  EXPECT_FALSE(hv.Set(lookup, "2"));
  ASSERT_TRUE(hv.Get(lookup).has_value());
  EXPECT_EQ(hv.Get(lookup).value(), "2");

  std::string delete_source = "qqalphaqq";
  std::string_view delete_field(delete_source.data() + 2, 5);
  EXPECT_TRUE(hv.Delete(delete_field));
  EXPECT_FALSE(hv.Exists(lookup));
}

}  // namespace
}  // namespace miniredis
