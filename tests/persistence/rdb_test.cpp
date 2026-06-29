#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/server.h"
#include "ds/ds_common.h"
#include "persistence/crc64.h"
#include "persistence/rdb_deserialize.h"
#include "persistence/rdb_format.h"
#include "persistence/rdb_serialize.h"
#include "types/value.h"

namespace miniredis {
namespace {

std::filesystem::path TempPath(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("miniredis_" + std::string(name) + "_" +
          std::to_string(static_cast<long long>(::getpid())) + ".rdb");
}

int64_t NowMs() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void AppendU8(std::string& out, uint8_t value) {
  out.push_back(static_cast<char>(value));
}

void AppendU64(std::string& out, uint64_t value) {
  uint8_t bytes[8];
  ds::StoreLE64(bytes, value);
  out.append(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void AppendLen(std::string& out, size_t len) {
  if (len < 64) {
    AppendU8(out, static_cast<uint8_t>(len));
  } else if (len < 16384) {
    AppendU8(out, static_cast<uint8_t>(0x40 | (len >> 8)));
    AppendU8(out, static_cast<uint8_t>(len & 0xFF));
  } else {
    AppendU8(out, rdb::kLen32Bit);
    uint8_t bytes[4];
    ds::StoreLE32(bytes, static_cast<uint32_t>(len));
    out.append(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  }
}

void AppendString(std::string& out, std::string_view value) {
  AppendLen(out, value.size());
  out.append(value);
}

void FinalizeRdb(std::string& out) {
  AppendU8(out, rdb::kOpEof);
  uint64_t crc = Crc64(out.data() + 9, out.size() - 9, 0);
  AppendU64(out, crc);
}

void WriteFile(const std::filesystem::path& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

TEST(RdbTest, RoundTripShortAndLongStringKeys) {
  auto path = TempPath("roundtrip_keys");

  Server& server = Server::Instance();
  MiniRedisConfig cfg;
  cfg.databases = 2;
  ASSERT_TRUE(server.Init(cfg));
  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Set("a", StringValue("one")));
  std::string long_key(70, 'k');
  ASSERT_TRUE(db->Set(long_key, StringValue("two")));

  RdbSerializer serializer;
  ASSERT_TRUE(serializer.Save(path.string(), server));

  ASSERT_TRUE(server.Init(cfg));
  RdbDeserializer loader;
  ASSERT_TRUE(loader.Load(path.string(), server));

  db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  auto* short_value = db->Find("a");
  ASSERT_NE(short_value, nullptr);
  ASSERT_EQ(std::get_if<StringValue>(short_value)->ToString(), "one");
  auto* long_value = db->Find(long_key);
  ASSERT_NE(long_value, nullptr);
  ASSERT_EQ(std::get_if<StringValue>(long_value)->ToString(), "two");

  std::filesystem::remove(path);
}

TEST(RdbTest, ExpiredCompositeValueDoesNotCorruptFollowingKeys) {
  auto path = TempPath("expired_list_skip");

  std::string data;
  data.append(rdb::kRdbMagic, 5);
  data.append("0011", 4);

  AppendU8(data, rdb::kOpExpireTimeMs);
  AppendU64(data, static_cast<uint64_t>(NowMs() - 1000));
  AppendU8(data, rdb::kTypeList);
  AppendString(data, "old-list");
  AppendLen(data, 2);
  AppendString(data, "a");
  AppendString(data, "b");

  AppendU8(data, rdb::kTypeString);
  AppendString(data, "live");
  AppendString(data, "ok");

  FinalizeRdb(data);
  WriteFile(path, data);

  Server& server = Server::Instance();
  MiniRedisConfig cfg;
  ASSERT_TRUE(server.Init(cfg));
  RdbDeserializer loader;
  ASSERT_TRUE(loader.Load(path.string(), server));

  auto* db = server.GetDb(0);
  ASSERT_NE(db, nullptr);
  EXPECT_FALSE(db->Exists("old-list"));
  auto* live = db->Find("live");
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(std::get_if<StringValue>(live)->ToString(), "ok");

  std::filesystem::remove(path);
}

}  // namespace
}  // namespace miniredis
