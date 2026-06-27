#include "core/config.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace miniredis {

namespace {

template <typename T>
bool ParseInt(std::string_view s, T& out) {
  auto* start = s.data();
  auto* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(start, end, out);
  return ec == std::errc() && ptr == end;
}

}  // namespace

ConfigManager::ConfigManager() = default;

bool ConfigManager::LoadFromFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return false;

  std::string line;
  bool any_failed = false;
  while (std::getline(file, line)) {
    // Skip comments and blank lines
    auto pos = line.find('#');
    if (pos != std::string::npos) line = line.substr(0, pos);
    // Trim
    size_t start = 0;
    while (start < line.size() && std::isspace(line[start])) start++;
    size_t end = line.size();
    while (end > start && std::isspace(line[end - 1])) end--;
    if (start >= end) continue;
    line = line.substr(start, end - start);

    // Split key value
    auto space_pos = line.find(' ');
    if (space_pos == std::string::npos) continue;
    std::string key = line.substr(0, space_pos);
    std::string value = line.substr(space_pos + 1);
    // Trim value
    start = 0;
    while (start < value.size() && std::isspace(value[start])) start++;
    end = value.size();
    while (end > start && std::isspace(value[end - 1])) end--;
    value = value.substr(start, end - start);

    if (!Set(key, value)) any_failed = true;
  }
  return !any_failed;
}

std::optional<std::string> ConfigManager::Get(std::string_view key) const {
  // Check known config keys
  std::string k(key);
  if (k == "bind") return config_.bind;
  if (k == "port") return std::to_string(config_.port);
  if (k == "tcp_backlog") return std::to_string(config_.tcp_backlog);
  if (k == "databases") return std::to_string(config_.databases);
  if (k == "set_max_intset_entries")
    return std::to_string(config_.set_max_intset_entries);
  if (k == "hash_max_listpack_entries")
    return std::to_string(config_.hash_max_listpack_entries);
  if (k == "hash_max_listpack_value")
    return std::to_string(config_.hash_max_listpack_value);
  if (k == "zset_max_listpack_entries")
    return std::to_string(config_.zset_max_listpack_entries);
  if (k == "zset_max_listpack_value")
    return std::to_string(config_.zset_max_listpack_value);
  if (k == "save_enabled") return config_.save_enabled ? "yes" : "no";
  if (k == "rdb_filename") return config_.rdb_filename;
  if (k == "aof_filename") return config_.aof_filename;
  if (k == "log_level") return config_.log_level;
  if (k == "log_file") return config_.log_file;
  auto it = extras_.find(k);
  if (it != extras_.end()) return it->second;
  return std::nullopt;
}

bool ConfigManager::Set(std::string_view key, std::string_view value) {
  std::string k(key);
  std::string v(value);
  if (k == "bind") {
    config_.bind = v;
    return true;
  }
  if (k == "port") {
    int p = 0;
    if (!ParseInt<int>(v, p) || p < 1 || p > 65535) return false;
    config_.port = static_cast<uint16_t>(p);
    return true;
  }
  if (k == "databases") {
    int d = 0;
    if (!ParseInt<int>(v, d) || d < 1) return false;
    config_.databases = d;
    return true;
  }
  if (k == "set_max_intset_entries") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.set_max_intset_entries = val;
    return true;
  }
  if (k == "hash_max_listpack_entries") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.hash_max_listpack_entries = val;
    return true;
  }
  if (k == "hash_max_listpack_value") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.hash_max_listpack_value = val;
    return true;
  }
  if (k == "zset_max_listpack_entries") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.zset_max_listpack_entries = val;
    return true;
  }
  if (k == "zset_max_listpack_value") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.zset_max_listpack_value = val;
    return true;
  }
  if (k == "tcp_backlog") {
    int val = 0;
    if (!ParseInt<int>(v, val) || val < 1) return false;
    config_.tcp_backlog = val;
    return true;
  }
  if (k == "save_enabled") {
    if (v == "yes" || v == "true" || v == "1")
      config_.save_enabled = true;
    else if (v == "no" || v == "false" || v == "0")
      config_.save_enabled = false;
    else
      return false;
    return true;
  }
  if (k == "rdb_filename") {
    config_.rdb_filename = v;
    return true;
  }
  if (k == "aof_filename") {
    config_.aof_filename = v;
    return true;
  }
  if (k == "log_level") {
    config_.log_level = v;
    return true;
  }
  if (k == "log_file") {
    config_.log_file = v;
    return true;
  }
  extras_[k] = v;
  return true;
}

EncodingThresholds ConfigManager::GetEncodingThresholds() const {
  EncodingThresholds t;
  t.set_max_intset_entries = config_.set_max_intset_entries;
  t.hash_max_listpack_entries = config_.hash_max_listpack_entries;
  t.hash_max_listpack_value = config_.hash_max_listpack_value;
  t.zset_max_listpack_entries = config_.zset_max_listpack_entries;
  t.zset_max_listpack_value = config_.zset_max_listpack_value;
  return t;
}

const MiniRedisConfig& ConfigManager::Config() const { return config_; }

}  // namespace miniredis
