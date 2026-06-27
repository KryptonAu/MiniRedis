#include <gtest/gtest.h>

#include "ds/ds_common.h"
#include "ds/intset.h"

namespace miniredis::ds {
namespace {

// ===== Construction =====
TEST(IntsetTest, ConstructEmpty) {
  Intset is;
  EXPECT_EQ(is.Size(), 0);
  EXPECT_EQ(is.Encoding(), 2);  // starts as int16
}

// ===== Add and Contains =====
TEST(IntsetTest, AddAndContains) {
  Intset is;
  EXPECT_TRUE(is.Add(42));
  EXPECT_EQ(is.Size(), 1);
  EXPECT_TRUE(is.Contains(42));
  EXPECT_FALSE(is.Contains(99));
}

TEST(IntsetTest, AddDuplicate) {
  Intset is;
  EXPECT_TRUE(is.Add(10));
  EXPECT_FALSE(is.Add(10));  // duplicate
  EXPECT_EQ(is.Size(), 1);
}

TEST(IntsetTest, AddMultipleOrdered) {
  Intset is;
  EXPECT_TRUE(is.Add(3));
  EXPECT_TRUE(is.Add(1));
  EXPECT_TRUE(is.Add(2));
  EXPECT_EQ(is.Size(), 3);

  // Verify sorted order
  auto vals = is.Values();
  ASSERT_EQ(vals.size(), 3);
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 2);
  EXPECT_EQ(vals[2], 3);
}

TEST(IntsetTest, AddNegative) {
  Intset is;
  EXPECT_TRUE(is.Add(-5));
  EXPECT_TRUE(is.Add(-10));
  EXPECT_TRUE(is.Add(0));

  EXPECT_EQ(is.Size(), 3);
  auto vals = is.Values();
  EXPECT_EQ(vals[0], -10);
  EXPECT_EQ(vals[1], -5);
  EXPECT_EQ(vals[2], 0);
}

// ===== Get =====
TEST(IntsetTest, GetByIndex) {
  Intset is;
  is.Add(10);
  is.Add(20);
  is.Add(30);

  EXPECT_EQ(is.Get(0).value(), 10);
  EXPECT_EQ(is.Get(1).value(), 20);
  EXPECT_EQ(is.Get(2).value(), 30);
  EXPECT_FALSE(is.Get(3).has_value());
}

// ===== Find =====
TEST(IntsetTest, FindReturnsIndex) {
  Intset is;
  is.Add(100);
  is.Add(200);
  is.Add(300);

  auto idx = is.Find(200);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1);

  EXPECT_FALSE(is.Find(999).has_value());
}

// ===== Remove =====
TEST(IntsetTest, Remove) {
  Intset is;
  is.Add(1);
  is.Add(2);
  is.Add(3);

  EXPECT_TRUE(is.Remove(2));
  EXPECT_EQ(is.Size(), 2);
  EXPECT_FALSE(is.Contains(2));
  EXPECT_TRUE(is.Contains(1));
  EXPECT_TRUE(is.Contains(3));

  EXPECT_FALSE(is.Remove(99));  // not found
  EXPECT_EQ(is.Size(), 2);
}

TEST(IntsetTest, RemoveFromBeginning) {
  Intset is;
  is.Add(1);
  is.Add(2);
  is.Add(3);
  EXPECT_TRUE(is.Remove(1));
  auto vals = is.Values();
  EXPECT_EQ(vals[0], 2);
  EXPECT_EQ(vals[1], 3);
}

TEST(IntsetTest, RemoveFromEnd) {
  Intset is;
  is.Add(1);
  is.Add(2);
  is.Add(3);
  EXPECT_TRUE(is.Remove(3));
  auto vals = is.Values();
  EXPECT_EQ(vals[0], 1);
  EXPECT_EQ(vals[1], 2);
}

// ===== Encoding upgrade =====
TEST(IntsetTest, EncodingStartsInt16) {
  Intset is;
  EXPECT_EQ(is.Encoding(), 2);
  is.Add(0);
  EXPECT_EQ(is.Encoding(), 2);
}

TEST(IntsetTest, EncodingUpgradeToInt32) {
  Intset is;
  is.Add(1);
  is.Add(32768);  // exceeds int16 range
  EXPECT_EQ(is.Encoding(), 4);  // int32
  EXPECT_TRUE(is.Contains(1));
  EXPECT_TRUE(is.Contains(32768));
}

TEST(IntsetTest, EncodingUpgradeToInt64) {
  Intset is;
  is.Add(1);
  is.Add(INT64_MAX);
  EXPECT_EQ(is.Encoding(), 8);  // int64
  EXPECT_TRUE(is.Contains(1));
  EXPECT_TRUE(is.Contains(INT64_MAX));
}

TEST(IntsetTest, EncodingUpgradeWithNegative) {
  Intset is;
  is.Add(1);
  is.Add(INT64_MIN);
  EXPECT_EQ(is.Encoding(), 8);
  EXPECT_TRUE(is.Contains(INT64_MIN));
  EXPECT_TRUE(is.Contains(1));
}

TEST(IntsetTest, EncodingUpgradePreservesOrder) {
  Intset is;
  is.Add(100);
  is.Add(200);
  is.Add(300);
  EXPECT_EQ(is.Encoding(), 2);

  is.Add(100000);  // triggers int32 upgrade
  EXPECT_EQ(is.Encoding(), 4);

  auto vals = is.Values();
  ASSERT_EQ(vals.size(), 4);
  EXPECT_EQ(vals[0], 100);
  EXPECT_EQ(vals[1], 200);
  EXPECT_EQ(vals[2], 300);
  EXPECT_EQ(vals[3], 100000);
}

// ===== Large data =====
TEST(IntsetTest, LargeNumberOfEntries) {
  Intset is;
  for (int i = 0; i < 1000; i++) {
    EXPECT_TRUE(is.Add(i * 2));  // even numbers to test binary search
  }
  EXPECT_EQ(is.Size(), 1000);
  EXPECT_TRUE(is.Contains(0));
  EXPECT_TRUE(is.Contains(1998));
  EXPECT_FALSE(is.Contains(1));   // odd, not present
  EXPECT_FALSE(is.Contains(1999));
}

// ===== Serialization =====
TEST(IntsetTest, DataConsistency) {
  Intset is;
  is.Add(1);
  is.Add(2);
  is.Add(3);

  const uint8_t* data = is.Data();
  size_t data_size = is.DataSize();

  // Header: encoding(4) + length(4) + contents
  EXPECT_EQ(data_size, 8 + 3 * 2);  // 8 header + 3*2 (int16)

  uint32_t encoding = LoadLE32(data);
  uint32_t length = LoadLE32(data + 4);
  EXPECT_EQ(encoding, 2);
  EXPECT_EQ(length, 3);
}

// ===== FromBytes =====
TEST(IntsetTest, FromBytesRoundTrip) {
  Intset is;
  is.Add(1);
  is.Add(2);
  is.Add(3);

  std::vector<uint8_t> blob(is.Data(), is.Data() + is.DataSize());
  auto is2 = Intset::FromBytes(std::move(blob));
  ASSERT_TRUE(is2.has_value());
  EXPECT_EQ(is2->Size(), 3);
  EXPECT_EQ(is2->Encoding(), 2);
  EXPECT_TRUE(is2->Contains(1));
  EXPECT_TRUE(is2->Contains(2));
  EXPECT_TRUE(is2->Contains(3));
}

TEST(IntsetTest, FromBytesRejectsInvalid) {
  // Empty
  std::vector<uint8_t> empty;
  EXPECT_FALSE(Intset::FromBytes(empty).has_value());

  // Too short (less than 8 bytes header)
  std::vector<uint8_t> too_short = {0x02, 0x00, 0x00, 0x00};
  EXPECT_FALSE(Intset::FromBytes(too_short).has_value());

  // Invalid encoding
  std::vector<uint8_t> bad_encoding = {
      0x03, 0x00, 0x00, 0x00,  // encoding = 3 (invalid)
      0x00, 0x00, 0x00, 0x00   // length = 0
  };
  EXPECT_FALSE(Intset::FromBytes(bad_encoding).has_value());

  // Wrong size
  std::vector<uint8_t> wrong_size = {
      0x02, 0x00, 0x00, 0x00,  // encoding = 2
      0x02, 0x00, 0x00, 0x00,  // length = 2
      0x01, 0x00                // only 1 element worth of data
  };
  EXPECT_FALSE(Intset::FromBytes(wrong_size).has_value());
}

// ===== Edge cases =====
TEST(IntsetTest, RemoveLastElement) {
  Intset is;
  is.Add(42);
  EXPECT_TRUE(is.Remove(42));
  EXPECT_EQ(is.Size(), 0);
  EXPECT_FALSE(is.Contains(42));
}

TEST(IntsetTest, FindOnEmpty) {
  Intset is;
  EXPECT_FALSE(is.Find(0).has_value());
}

TEST(IntsetTest, GetOnEmpty) {
  Intset is;
  EXPECT_FALSE(is.Get(0).has_value());
}

}  // namespace
}  // namespace miniredis::ds
