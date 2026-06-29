#include "core/server.h"

#include <charconv>
#include <chrono>

namespace miniredis {

namespace {
constexpr uint32_t kLruClockMax = 0xFFFFFF;
constexpr int kLruClockResolutionMs = 1000;

template <typename T>
bool ParseInt(std::string_view s, T& out) {
  auto* start = s.data();
  auto* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(start, end, out);
  return ec == std::errc() && ptr == end;
}
}  // namespace

Server& Server::Instance() {
  static Server instance;
  return instance;
}

bool Server::Init(const MiniRedisConfig& config) {
  if (config.databases < 1) return false;
  config_ = config;
  databases_.clear();
  databases_.resize(static_cast<size_t>(config.databases));
  clients_.Clear();
  stats_ = ServerStats{};
  lru_clock_ = 0;
  child_pid_ = -1;
  last_save_ms_ = 0;
  running_ = true;
  return true;
}

Database* Server::GetDb(int index) {
  if (index < 0 || index >= static_cast<int>(databases_.size())) return nullptr;
  return &databases_[static_cast<size_t>(index)];
}

const Database* Server::GetDb(int index) const {
  if (index < 0 || index >= static_cast<int>(databases_.size())) return nullptr;
  return &databases_[static_cast<size_t>(index)];
}

Database* Server::GetDbFor(const Client& client) {
  return GetDb(client.CurrentDb());
}

std::optional<size_t> Server::DbSize(int index) {
  auto* db = GetDb(index);
  if (!db) return std::nullopt;
  return db->Size();
}

bool Server::FlushDb(int index) {
  auto* db = GetDb(index);
  if (!db) return false;
  db->Clear();
  return true;
}

void Server::FlushAll() {
  for (auto& db : databases_) db.Clear();
}

int Server::DbCount() const { return static_cast<int>(databases_.size()); }

Client* Server::CreateClient(int fd) {
  if (FindClient(fd)) return nullptr;
  auto client = std::make_unique<Client>(fd, 0);
  auto* raw = client.get();
  clients_.Set(fd, std::move(client));
  return raw;
}

void Server::RemoveClient(int fd) { clients_.Delete(fd); }

Client* Server::FindClient(int fd) {
  auto* ptr = clients_.Find(fd);
  return ptr ? ptr->get() : nullptr;
}

size_t Server::ClientCount() const { return clients_.Size(); }

const MiniRedisConfig& Server::GetConfig() const { return config_; }

EncodingThresholds Server::GetEncodingThresholds() const {
  EncodingThresholds t;
  t.set_max_intset_entries = config_.set_max_intset_entries;
  t.hash_max_listpack_entries = config_.hash_max_listpack_entries;
  t.hash_max_listpack_value = config_.hash_max_listpack_value;
  t.zset_max_listpack_entries = config_.zset_max_listpack_entries;
  t.zset_max_listpack_value = config_.zset_max_listpack_value;
  return t;
}

bool Server::ApplyConfig(std::string_view key, std::string_view value) {
  std::string k(key);
  std::string v(value);
  for (auto& ch : k) {
    if (ch == '-') ch = '_';
  }

  if (k == "maxmemory") {
    size_t val = 0;
    if (!ParseInt<size_t>(v, val)) return false;
    config_.maxmemory = val;
    return true;
  }
  if (k == "maxmemory_policy") {
    if (v != "noeviction" && v != "allkeys_lru" && v != "allkeys-lru" &&
        v != "volatile_lru" && v != "volatile-lru")
      return false;
    if (v == "allkeys-lru") v = "allkeys_lru";
    if (v == "volatile-lru") v = "volatile_lru";
    config_.maxmemory_policy = v;
    return true;
  }
  if (k == "maxmemory_samples") {
    int val = 0;
    if (!ParseInt<int>(v, val) || val < 1) return false;
    config_.maxmemory_samples = val;
    return true;
  }
  if (k == "hz") {
    int val = 0;
    if (!ParseInt<int>(v, val) || val < 1 || val > 500) return false;
    config_.hz = val;
    return true;
  }
  if (k == "appendonly") {
    if (v == "yes" || v == "true" || v == "1")
      config_.appendonly = true;
    else if (v == "no" || v == "false" || v == "0")
      config_.appendonly = false;
    else
      return false;
    return true;
  }
  if (k == "appendfsync") {
    if (v != "always" && v != "everysec" && v != "no") return false;
    config_.appendfsync = v;
    return true;
  }
  if (k == "dbfilename" || k == "rdb_filename") {
    config_.rdb_filename = v;
    return true;
  }
  if (k == "appendfilename" || k == "aof_filename") {
    config_.aof_filename = v;
    return true;
  }
  if (k == "active_expire_effort") {
    int val = 0;
    if (!ParseInt<int>(v, val) || val < 1 || val > 10) return false;
    config_.active_expire_effort = val;
    return true;
  }

  return false;
}

void Server::IncrementDirty(uint64_t delta) { stats_.dirty += delta; }

void Server::ResetDirty() { stats_.dirty = 0; }

void Server::UpdateLruClock(int64_t now_ms) {
  lru_clock_ = static_cast<uint32_t>(
      (static_cast<uint64_t>(now_ms) / kLruClockResolutionMs) & kLruClockMax);
  for (auto& db : databases_) {
    db.SetCurrentLruClock(lru_clock_);
  }
}

size_t Server::ApproxMemoryUsage() const {
  size_t total = 0;
  for (const auto& db : databases_) {
    total += db.ApproxMemoryUsage();
  }
  return total;
}

bool Server::IsRunning() const { return running_; }

void Server::Shutdown() {
  clients_.Clear();
  databases_.clear();
  running_ = false;
}

}  // namespace miniredis
