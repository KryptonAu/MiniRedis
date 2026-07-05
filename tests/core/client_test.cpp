#include "core/client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace miniredis {
namespace {

static void AppendToQueryBuffer(QueryBuffer& buffer, std::string_view data,
                                size_t min_writable = 0) {
  std::span<char> out = buffer.PrepareWrite(
      min_writable == 0 ? data.size() : std::max(min_writable, data.size()));
  ASSERT_GE(out.size(), data.size());
  std::memcpy(out.data(), data.data(), data.size());
  buffer.CommitWrite(data.size());
}

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

TEST(QueryBufferTest, CommitWriteAndConsumePrefix) {
  QueryBuffer buffer;
  AppendToQueryBuffer(buffer, "abcdef");

  EXPECT_EQ(buffer.Readable(), "abcdef");
  EXPECT_EQ(buffer.ReadableSize(), 6u);

  buffer.Consume(2);
  EXPECT_EQ(buffer.Readable(), "cdef");
  EXPECT_EQ(buffer.ReadableSize(), 4u);
}

TEST(QueryBufferTest, ConsumeAllClearsReadableWindow) {
  QueryBuffer buffer;
  AppendToQueryBuffer(buffer, "PING");

  buffer.Consume(4);
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.Readable(), "");

  AppendToQueryBuffer(buffer, "PONG");
  EXPECT_EQ(buffer.Readable(), "PONG");
}

TEST(QueryBufferTest, PartialTailCanAppendWithoutCompaction) {
  QueryBuffer buffer;
  AppendToQueryBuffer(buffer, "abcdef", 16);
  const char* before = buffer.Readable().data();

  buffer.Consume(2);
  AppendToQueryBuffer(buffer, "gh", 2);

  EXPECT_EQ(buffer.Readable(), "cdefgh");
  EXPECT_EQ(buffer.Readable().data(), before + 2);
}

TEST(QueryBufferTest, PrepareWriteCompactsOnlyWhenTailIsInsufficient) {
  QueryBuffer buffer;
  AppendToQueryBuffer(buffer, "abcdef", 8);
  buffer.Consume(3);
  ASSERT_EQ(buffer.Readable(), "def");

  std::span<char> out = buffer.PrepareWrite(8);
  ASSERT_GE(out.size(), 8u);
  EXPECT_EQ(buffer.Readable(), "def");
  EXPECT_EQ(buffer.Readable().data(), out.data() - buffer.ReadableSize());
}

}  // namespace
}  // namespace miniredis
