#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types/encoding_thresholds.h"

namespace miniredis {

struct MiniRedisConfig {
  std::string bind = "127.0.0.1";
  uint16_t port = 6379;
  int tcp_backlog = 511;
  int databases = 16;
  size_t set_max_intset_entries = 512;
  size_t hash_max_listpack_entries = 512;
  size_t hash_max_listpack_value = 64;
  size_t zset_max_listpack_entries = 128;
  size_t zset_max_listpack_value = 64;
  bool save_enabled = true;
  std::string rdb_filename = "dump.rdb";
  std::string aof_filename = "appendonly.aof";
  std::string log_level = "notice";
  std::string log_file;

  // Persistence & Eviction (Phase 1+)
  size_t maxmemory = 0;
  std::string maxmemory_policy = "noeviction";
  int maxmemory_samples = 5;
  int hz = 10;
  bool appendonly = false;
  std::string appendfsync = "everysec";
  std::vector<std::pair<int, int>> save_params = {
      {3600, 1}, {300, 100}, {60, 10000}};
  int active_expire_effort = 1;
};

class ConfigManager {
 public:
  ConfigManager();
  bool LoadFromFile(const std::string& path);
  std::optional<std::string> Get(std::string_view key) const;
  bool Set(std::string_view key, std::string_view value);
  EncodingThresholds GetEncodingThresholds() const;
  const MiniRedisConfig& Config() const;

 private:
  MiniRedisConfig config_;
  std::unordered_map<std::string, std::string> extras_;
};

}  // namespace miniredis
