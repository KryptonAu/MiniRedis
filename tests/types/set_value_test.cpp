#include "types/set_value.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace miniredis {
namespace {

TEST(SetValueTest, ConstructEmpty) {
  SetValue sv;
  EXPECT_EQ(sv.Size(), 0);
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntset);
}

TEST(SetValueTest, AddAndContains) {
  SetValue sv;
  EXPECT_TRUE(sv.Add("1"));
  EXPECT_TRUE(sv.Add("2"));
  EXPECT_EQ(sv.Size(), 2);
  EXPECT_TRUE(sv.Contains("1"));
  EXPECT_FALSE(sv.Contains("3"));
}

TEST(SetValueTest, AddDuplicate) {
  SetValue sv;
  EXPECT_TRUE(sv.Add("42"));
  EXPECT_FALSE(sv.Add("42"));
  EXPECT_EQ(sv.Size(), 1);
}

TEST(SetValueTest, Remove) {
  SetValue sv;
  sv.Add("1");
  sv.Add("2");
  EXPECT_TRUE(sv.Remove("1"));
  EXPECT_EQ(sv.Size(), 1);
  EXPECT_FALSE(sv.Contains("1"));
  EXPECT_FALSE(sv.Remove("99"));
}

TEST(SetValueTest, Members) {
  SetValue sv;
  sv.Add("b");
  sv.Add("a");
  auto m = sv.Members();
  EXPECT_EQ(m.size(), 2);
}

TEST(SetValueTest, NonCanonicalIntForcesHashtable) {
  EncodingThresholds thresh;
  thresh.set_max_intset_entries = 512;
  SetValue sv(thresh);
  sv.Add("1");
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kIntset);
  sv.Add("001");  // non-canonical → upgrade
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kHashtable);
  EXPECT_TRUE(sv.Contains("001"));
  EXPECT_TRUE(sv.Contains("1"));
}

TEST(SetValueTest, UpgradeOnThreshold) {
  EncodingThresholds thresh;
  thresh.set_max_intset_entries = 5;
  SetValue sv(thresh);
  for (int i = 0; i < 6; i++) {
    sv.Add(std::to_string(i));
  }
  EXPECT_EQ(sv.Encoding(), ValueEncoding::kHashtable);
  EXPECT_EQ(sv.Size(), 6);
}

TEST(SetValueTest, RandomMember) {
  SetValue sv;
  sv.Add("42");
  auto r = sv.RandomMember();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "42");
}

TEST(SetValueTest, Pop) {
  SetValue sv;
  sv.Add("42");
  sv.Add("99");
  auto popped = sv.Pop();
  ASSERT_TRUE(popped.has_value());
  EXPECT_EQ(sv.Size(), 1);
}

TEST(SetValueTest, HashtableEncodingAcceptsSubviewMembers) {
  EncodingThresholds thresh;
  thresh.set_max_intset_entries = 512;
  SetValue sv(thresh);
  EXPECT_TRUE(sv.Add("1"));

  std::string member_source = "__alpha__";
  std::string_view member(member_source.data() + 2, 5);
  EXPECT_TRUE(sv.Add(member));
  ASSERT_EQ(sv.Encoding(), ValueEncoding::kHashtable);
  member_source[2] = 'X';

  std::string lookup_source = "zzalphazz";
  std::string_view lookup(lookup_source.data() + 2, 5);
  EXPECT_TRUE(sv.Contains(lookup));
  EXPECT_TRUE(sv.Contains("1"));
  EXPECT_FALSE(sv.Add(lookup));

  std::string remove_source = "qqalphaqq";
  std::string_view remove_member(remove_source.data() + 2, 5);
  EXPECT_TRUE(sv.Remove(remove_member));
  EXPECT_FALSE(sv.Contains(lookup));
  EXPECT_TRUE(sv.Contains("1"));
}

}  // namespace
}  // namespace miniredis
