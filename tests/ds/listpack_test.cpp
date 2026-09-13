#include "ds/listpack.h"

#include <gtest/gtest.h>

namespace miniredis::ds {
namespace {

uint16_t HeaderCount(const Listpack& lp) {
  const uint8_t* data = lp.Data();
  return static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
}

void ExpectContentsAndRoundTrip(const Listpack& lp,
                                const std::vector<std::string>& expected,
                                uint16_t count_header) {
  EXPECT_EQ(lp.Size(), expected.size());
  EXPECT_EQ(HeaderCount(lp), count_header);
  EXPECT_EQ(lp.TotalBytes(), lp.DataSize());
  ASSERT_GE(lp.DataSize(), 7u);
  EXPECT_EQ(lp.Data()[lp.DataSize() - 1], 0xFF);

  auto restored = Listpack::FromBytes(
      std::vector<uint8_t>(lp.Data(), lp.Data() + lp.DataSize()));
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(HeaderCount(*restored), count_header);
  EXPECT_EQ(restored->Size(), expected.size());
  auto it = lp.begin();
  for (size_t i = 0; i < expected.size(); ++i) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ((*it).ToString(), expected[i]);
    ASSERT_TRUE(lp.Get(i).has_value());
    EXPECT_EQ(lp.Get(i)->ToString(), expected[i]);
    ASSERT_TRUE(restored->Get(i).has_value());
    EXPECT_EQ(restored->Get(i)->ToString(), expected[i]);
    ++it;
  }
  EXPECT_EQ(it, lp.end());
}

TEST(ListpackTest, PopBackEmptyAndReuse) {
  Listpack lp;
  EXPECT_FALSE(lp.PopBack().has_value());
  ExpectContentsAndRoundTrip(lp, {}, 0);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(lp.Append("tail"));
    EXPECT_EQ(lp.PopBack(), "tail");
    EXPECT_FALSE(lp.PopBack().has_value());
    ExpectContentsAndRoundTrip(lp, {}, 0);
  }
}

TEST(ListpackTest, PopBackMixedValuesOwnsReturnedStrings) {
  Listpack lp;
  std::vector<std::string> values = {"head", "", std::string("a\0b", 3),
                                     "-123", std::string(5000, 'x')};
  for (const auto& value : values) ASSERT_TRUE(lp.Append(value));
  std::vector<std::string> popped;
  while (!values.empty()) {
    auto value = lp.PopBack();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, values.back());
    popped.push_back(std::move(*value));
    values.pop_back();
    ExpectContentsAndRoundTrip(lp, values,
                               static_cast<uint16_t>(values.size()));
  }
  ASSERT_TRUE(lp.Append(std::string(10000, 'y')));
  EXPECT_EQ(popped, (std::vector<std::string>{std::string(5000, 'x'), "-123",
                                             std::string("a\0b", 3), "", "head"}));
}

TEST(ListpackTest, PopBackAllIntegerEncodings) {
  Listpack lp;
  const std::vector<int64_t> integers = {
      0, 127, 128, -1, -4096, 4095, -4097, 4096, -32768, 32767,
      -32769, 32768, -8388608, 8388607, -8388609, 8388608,
      INT32_MIN, INT32_MAX, int64_t{INT32_MIN} - 1,
      int64_t{INT32_MAX} + 1, INT64_MIN, INT64_MAX};
  std::vector<std::string> expected;
  for (int64_t value : integers) {
    ASSERT_TRUE(lp.Append(value));
    expected.push_back(std::to_string(value));
  }
  while (!expected.empty()) {
    EXPECT_EQ(lp.PopBack(), expected.back());
    expected.pop_back();
    ExpectContentsAndRoundTrip(lp, expected,
                               static_cast<uint16_t>(expected.size()));
  }
}

TEST(ListpackTest, PopBackStringEncodingAndBacklenBoundaries) {
  // String header transitions (1/2/5 bytes), followed by payload lengths
  // around the 1/2/3/4-byte backlen transitions, including the header.
  const std::vector<size_t> lengths = {
      0, 63, 64, 4095, 4096, 124, 125, 126,
      16377, 16378, 16379, 2097145, 2097146, 2097147};
  for (size_t length : lengths) {
    SCOPED_TRACE(length);
    Listpack lp;
    ASSERT_TRUE(lp.Append("prefix"));
    const size_t prefix_bytes = lp.DataSize();
    std::string value(length, 'x');
    const size_t payload = length + (length < 64 ? 1u : length < 4096 ? 2u : 5u);
    const size_t backlen = payload <= 127 ? 1u : payload < 16383 ? 2u
                                                  : payload < 2097151 ? 3u : 4u;
    ASSERT_EQ(Listpack::EncodedEntrySize(value), payload + backlen);
    ASSERT_TRUE(lp.Append(value));
    EXPECT_EQ(lp.DataSize(), prefix_bytes + payload + backlen);
    ExpectContentsAndRoundTrip(lp, {"prefix", value}, 2);
    EXPECT_EQ(lp.PopBack(), value);
    EXPECT_EQ(lp.DataSize(), prefix_bytes);
    ExpectContentsAndRoundTrip(lp, {"prefix"}, 1);
  }
}

TEST(ListpackTest, PopBackKeepsUnknownCountHeaderThroughEmptyAndReuse) {
  Listpack source;
  std::vector<std::string> expected = {"first", "42", "", "last"};
  for (const auto& value : expected) ASSERT_TRUE(source.Append(value));
  std::vector<uint8_t> bytes(source.Data(), source.Data() + source.DataSize());
  bytes[4] = bytes[5] = 0xFF;
  auto lp = Listpack::FromBytes(std::move(bytes));
  ASSERT_TRUE(lp.has_value());
  while (!expected.empty()) {
    EXPECT_EQ(lp->PopBack(), expected.back());
    expected.pop_back();
    ExpectContentsAndRoundTrip(*lp, expected, UINT16_MAX);
  }
  EXPECT_FALSE(lp->PopBack().has_value());
  ASSERT_TRUE(lp->Append("again"));
  ExpectContentsAndRoundTrip(*lp, {"again"}, UINT16_MAX);
  EXPECT_EQ(lp->PopBack(), "again");
  ExpectContentsAndRoundTrip(*lp, {}, UINT16_MAX);
}

// ===== Construction =====
TEST(ListpackTest, ConstructEmpty) {
  Listpack lp;
  EXPECT_EQ(lp.Size(), 0);
  EXPECT_EQ(lp.TotalBytes(), 7);  // 6 header + 1 EOF
  EXPECT_GT(lp.DataSize(), 0);
}

// ===== Integer encoding tests =====
TEST(ListpackTest, AppendSmallUnsignedInteger) {
  Listpack lp;
  lp.Append(42);
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsInteger(0));
  EXPECT_EQ(lp.GetInteger(0).value(), 42);
}

TEST(ListpackTest, AppendNegativeInteger) {
  Listpack lp;
  lp.Append(-100);
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsInteger(0));
  EXPECT_EQ(lp.GetInteger(0).value(), -100);
}

TEST(ListpackTest, AppendLargeInteger) {
  Listpack lp;
  int64_t val = 1000000;
  lp.Append(val);
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsInteger(0));
  EXPECT_EQ(lp.GetInteger(0).value(), val);
}

TEST(ListpackTest, AppendMinMaxIntegers) {
  Listpack lp;
  lp.Append(INT64_MAX);
  lp.Append(INT64_MIN);

  EXPECT_EQ(lp.Size(), 2);
  EXPECT_EQ(lp.GetInteger(0).value(), INT64_MAX);
  EXPECT_EQ(lp.GetInteger(1).value(), INT64_MIN);
}

// Regression: 32-bit integer encoding must sign-extend correctly
TEST(ListpackTest, Int32RangeNegative) {
  Listpack lp;
  // -100000 is in 32-bit range, triggers LP_ENCODING_32BIT_INT
  lp.Append(-100000);
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsInteger(0));
  EXPECT_EQ(lp.GetInteger(0).value(), -100000);
}

TEST(ListpackTest, Int32RangeBoundaries) {
  Listpack lp;
  // -8388609 is the first value that requires 32-bit encoding
  lp.Append(-8388609);
  EXPECT_EQ(lp.GetInteger(0).value(), -8388609);

  lp.Append(-2147483648LL);  // INT32_MIN
  EXPECT_EQ(lp.GetInteger(1).value(), -2147483648LL);

  lp.Append(2147483647);  // INT32_MAX
  EXPECT_EQ(lp.GetInteger(2).value(), 2147483647);
}

// ===== String encoding tests =====
TEST(ListpackTest, AppendShortString) {
  Listpack lp;
  lp.Append(std::string_view("hello"));
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsString(0));
  EXPECT_EQ(lp.GetString(0).value(), "hello");
}

TEST(ListpackTest, AppendEmptyString) {
  Listpack lp;
  lp.Append(std::string_view(""));
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsString(0));
  EXPECT_EQ(lp.GetString(0).value(), "");
}

TEST(ListpackTest, AppendLongString) {
  Listpack lp;
  std::string long_str(5000, 'x');
  lp.Append(std::string_view(long_str));
  EXPECT_EQ(lp.Size(), 1);
  EXPECT_TRUE(lp.IsString(0));
  EXPECT_EQ(lp.GetString(0).value(), long_str);
}

// ===== Mixed types =====
TEST(ListpackTest, MixedIntegersAndStrings) {
  Listpack lp;
  lp.Append(1);
  lp.Append(std::string_view("two"));
  lp.Append(3);
  lp.Append(std::string_view("four"));

  EXPECT_EQ(lp.Size(), 4);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
  EXPECT_EQ(lp.GetString(1).value(), "two");
  EXPECT_EQ(lp.GetInteger(2).value(), 3);
  EXPECT_EQ(lp.GetString(3).value(), "four");
}

// ===== Value struct =====
TEST(ListpackTest, ValueToStringOnInteger) {
  Listpack lp;
  lp.Append(42);
  auto val = lp.Get(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val->type, Listpack::Value::Type::kInteger);
  EXPECT_EQ(val->ToString(), "42");
}

TEST(ListpackTest, ValueToStringOnString) {
  Listpack lp;
  lp.Append(std::string_view("abc"));
  auto val = lp.Get(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val->type, Listpack::Value::Type::kString);
  EXPECT_EQ(val->ToString(), "abc");
}

// ===== Insert operations =====
TEST(ListpackTest, InsertAtBeginning) {
  Listpack lp;
  lp.Append(2);
  lp.Append(3);
  lp.Insert(0, 1);

  EXPECT_EQ(lp.Size(), 3);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
  EXPECT_EQ(lp.GetInteger(1).value(), 2);
  EXPECT_EQ(lp.GetInteger(2).value(), 3);
}

TEST(ListpackTest, InsertInMiddle) {
  Listpack lp;
  lp.Append(1);
  lp.Append(3);
  lp.Insert(1, 2);

  EXPECT_EQ(lp.Size(), 3);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
  EXPECT_EQ(lp.GetInteger(1).value(), 2);
  EXPECT_EQ(lp.GetInteger(2).value(), 3);
}

TEST(ListpackTest, InsertAtEnd) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);
  lp.Insert(2, 3);

  EXPECT_EQ(lp.Size(), 3);
  EXPECT_EQ(lp.GetInteger(2).value(), 3);
}

// ===== Delete operations =====
TEST(ListpackTest, DeleteFromBeginning) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);
  lp.Append(3);
  lp.Delete(0);

  EXPECT_EQ(lp.Size(), 2);
  EXPECT_EQ(lp.GetInteger(0).value(), 2);
}

TEST(ListpackTest, DeleteFromMiddle) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);
  lp.Append(3);
  lp.Delete(1);

  EXPECT_EQ(lp.Size(), 2);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
  EXPECT_EQ(lp.GetInteger(1).value(), 3);
}

TEST(ListpackTest, DeleteFromEnd) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);
  lp.Append(3);
  lp.Delete(2);

  EXPECT_EQ(lp.Size(), 2);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
  EXPECT_EQ(lp.GetInteger(1).value(), 2);
}

// ===== Replace operations =====
TEST(ListpackTest, ReplaceSameType) {
  Listpack lp;
  lp.Append(1);
  lp.Replace(0, 99);

  EXPECT_EQ(lp.Size(), 1);
  EXPECT_EQ(lp.GetInteger(0).value(), 99);
}

TEST(ListpackTest, ReplaceStringWithStringSameSize) {
  Listpack lp;
  lp.Append(std::string_view("abc"));
  lp.Replace(0, std::string_view("xyz"));

  EXPECT_EQ(lp.Size(), 1);
  EXPECT_EQ(lp.GetString(0).value(), "xyz");
}

// ===== Find operations =====
TEST(ListpackTest, FindString) {
  Listpack lp;
  lp.Append(std::string_view("alpha"));
  lp.Append(std::string_view("beta"));
  lp.Append(std::string_view("gamma"));

  auto idx = lp.Find(std::string_view("beta"));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1);

  auto missing = lp.Find(std::string_view("delta"));
  EXPECT_FALSE(missing.has_value());
}

TEST(ListpackTest, FindInteger) {
  Listpack lp;
  lp.Append(10);
  lp.Append(20);
  lp.Append(30);

  auto idx = lp.Find(20);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1);

  auto missing = lp.Find(99);
  EXPECT_FALSE(missing.has_value());
}

// ===== Prepend =====
TEST(ListpackTest, Prepend) {
  Listpack lp;
  lp.Append(2);
  lp.Prepend(1);

  EXPECT_EQ(lp.Size(), 2);
  EXPECT_EQ(lp.GetInteger(0).value(), 1);
}

// ===== Edge cases: integer zero =====
TEST(ListpackTest, ZeroIsNotString) {
  Listpack lp;
  lp.Append(0);
  EXPECT_FALSE(lp.IsString(0));
  EXPECT_TRUE(lp.IsInteger(0));
  EXPECT_EQ(lp.GetInteger(0).value(), 0);
}

// ===== Edge cases: out of bounds =====
TEST(ListpackTest, GetOutOfBounds) {
  Listpack lp;
  EXPECT_FALSE(lp.Get(0).has_value());
  lp.Append(1);
  EXPECT_FALSE(lp.Get(1).has_value());
}

TEST(ListpackTest, DeleteOutOfBounds) {
  Listpack lp;
  size_t empty_total_bytes = lp.TotalBytes();
  uint16_t empty_count = HeaderCount(lp);
  EXPECT_FALSE(lp.Delete(0));
  EXPECT_EQ(lp.TotalBytes(), empty_total_bytes);
  EXPECT_EQ(HeaderCount(lp), empty_count);

  lp.Append(1);
  size_t single_total_bytes = lp.TotalBytes();
  uint16_t single_count = HeaderCount(lp);
  EXPECT_FALSE(lp.Delete(1));
  EXPECT_EQ(lp.TotalBytes(), single_total_bytes);
  EXPECT_EQ(HeaderCount(lp), single_count);
  EXPECT_EQ(lp.Size(), 1);
}

// ===== Serialization round-trip =====
TEST(ListpackTest, FromBytesValidates) {
  Listpack lp;
  lp.Append(1);
  lp.Append(std::string_view("hello"));

  auto lp2 = Listpack::FromBytes(
      std::vector<uint8_t>(lp.Data(), lp.Data() + lp.DataSize()));
  ASSERT_TRUE(lp2.has_value());
  EXPECT_EQ(lp2->Size(), 2);
  EXPECT_EQ(lp2->GetInteger(0).value(), 1);
  EXPECT_EQ(lp2->GetString(1).value(), "hello");
}

TEST(ListpackTest, FromBytesRejectsInvalid) {
  // Totally invalid data
  std::vector<uint8_t> bad = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(Listpack::FromBytes(bad).has_value());

  // Too short
  std::vector<uint8_t> too_short = {0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(Listpack::FromBytes(too_short).has_value());
}

TEST(ListpackTest, FromBytesRejectsWrongNumele) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);
  lp.Append(3);

  // Create a copy with corrupted numele
  std::vector<uint8_t> blob(lp.Data(), lp.Data() + lp.DataSize());
  // numele is at offset 4-5 (little-endian uint16_t)
  // Set it to a wrong value: 1 instead of 3
  blob[4] = 0x01;
  blob[5] = 0x00;

  EXPECT_FALSE(Listpack::FromBytes(std::move(blob)).has_value());
}

TEST(ListpackTest, FromBytesAcceptsUint16MaxNumele) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);

  // Create a copy with UINT16_MAX numele (means "unknown")
  std::vector<uint8_t> blob(lp.Data(), lp.Data() + lp.DataSize());
  blob[4] = 0xFF;
  blob[5] = 0xFF;

  auto result = Listpack::FromBytes(std::move(blob));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->Size(), 2);  // actual count from scanning
}

TEST(ListpackTest, MutatingUnknownNumeleKeepsUnknownCountHeader) {
  Listpack lp;
  lp.Append(1);
  lp.Append(2);

  std::vector<uint8_t> blob(lp.Data(), lp.Data() + lp.DataSize());
  blob[4] = 0xFF;
  blob[5] = 0xFF;

  auto result = Listpack::FromBytes(std::move(blob));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(HeaderCount(*result), UINT16_MAX);

  EXPECT_TRUE(result->Append(3));
  EXPECT_EQ(HeaderCount(*result), UINT16_MAX);
  EXPECT_EQ(result->Size(), 3);
  EXPECT_EQ(result->TotalBytes(), result->DataSize());

  EXPECT_TRUE(result->Delete(1));
  EXPECT_EQ(HeaderCount(*result), UINT16_MAX);
  EXPECT_EQ(result->Size(), 2);
  EXPECT_EQ(result->GetInteger(0).value(), 1);
  EXPECT_EQ(result->GetInteger(1).value(), 3);

  auto round_trip = Listpack::FromBytes(std::vector<uint8_t>(
      result->Data(), result->Data() + result->DataSize()));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(round_trip->Size(), 2);
}

// ===== Large data performance =====
TEST(ListpackTest, LargeNumberOfEntries) {
  Listpack lp;
  for (int i = 0; i < 1000; i++) {
    lp.Append(i);
  }
  EXPECT_EQ(lp.Size(), 1000);
  EXPECT_EQ(lp.GetInteger(0).value(), 0);
  EXPECT_EQ(lp.GetInteger(999).value(), 999);
}

TEST(ListpackTest, LargeMutationsMaintainHeaderIncrementally) {
  Listpack lp;
  for (int i = 0; i < 2000; i++) {
    ASSERT_TRUE(lp.Append(i));
    ASSERT_EQ(HeaderCount(lp), i + 1);
    ASSERT_EQ(lp.TotalBytes(), lp.DataSize());
  }

  for (int i = 0; i < 500; i++) {
    ASSERT_TRUE(lp.Delete(0));
    ASSERT_EQ(HeaderCount(lp), 1999 - i);
    ASSERT_EQ(lp.TotalBytes(), lp.DataSize());
  }

  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(lp.Replace(static_cast<size_t>(i),
                           std::string_view("replacement-value")));
    ASSERT_EQ(HeaderCount(lp), 1500);
    ASSERT_EQ(lp.TotalBytes(), lp.DataSize());
  }

  ASSERT_EQ(lp.Size(), 1500);
  EXPECT_EQ(lp.GetString(0).value(), "replacement-value");
  EXPECT_EQ(lp.GetString(99).value(), "replacement-value");
  EXPECT_EQ(lp.GetInteger(100).value(), 600);
  EXPECT_EQ(lp.GetInteger(1499).value(), 1999);

  auto round_trip = Listpack::FromBytes(
      std::vector<uint8_t>(lp.Data(), lp.Data() + lp.DataSize()));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(round_trip->Size(), 1500);
  EXPECT_EQ(round_trip->TotalBytes(), round_trip->DataSize());
  EXPECT_EQ(round_trip->GetString(0).value(), "replacement-value");
  EXPECT_EQ(round_trip->GetInteger(1499).value(), 1999);
}

}  // namespace
}  // namespace miniredis::ds
