#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/client.h"
#include "core/config.h"
#include "core/database.h"
#include "ds/dict.h"
#include "types/encoding_thresholds.h"

namespace miniredis {

struct ServerStats {
  uint64_t dirty = 0;
  uint64_t keyspace_hits = 0;
  uint64_t keyspace_misses = 0;
  uint64_t expired_keys = 0;
  uint64_t evicted_keys = 0;
};

class Server {
 public:
  static Server& Instance();

  bool Init(const MiniRedisConfig& config);

  // Database access
  Database* GetDb(int index);
  const Database* GetDb(int index) const;
  Database* GetDbFor(const Client& client);
  std::optional<size_t> DbSize(int index);
  bool FlushDb(int index);
  void FlushAll();
  int DbCount() const;

  // Client management
  Client* CreateClient(int fd);
  void RemoveClient(int fd);
  Client* FindClient(int fd);
  size_t ClientCount() const;

  // Config
  const MiniRedisConfig& GetConfig() const;
  EncodingThresholds GetEncodingThresholds() const;
  bool ApplyConfig(std::string_view key, std::string_view value);

  // Stats
  const ServerStats& Stats() const { return stats_; }
  void IncrementDirty(uint64_t delta = 1);
  void ResetDirty();
  void IncrementHits() { ++stats_.keyspace_hits; }
  void IncrementMisses() { ++stats_.keyspace_misses; }
  void IncrementExpired(size_t n = 1) { stats_.expired_keys += n; }
  void IncrementEvicted(size_t n = 1) { stats_.evicted_keys += n; }

  // LRU clock (24-bit)
  uint32_t LruClock() const { return lru_clock_; }
  void UpdateLruClock(int64_t now_ms);

  // Persistence state
  bool IsSaveInProgress() const { return child_pid_ != -1; }
  int GetChildPid() const { return child_pid_; }
  void SetChildPid(int pid) { child_pid_ = pid; }
  int64_t GetLastSaveMs() const { return last_save_ms_; }
  void SetLastSaveMs(int64_t ms) { last_save_ms_ = ms; }

  // Approx memory
  size_t ApproxMemoryUsage() const;

  // Lifecycle
  bool IsRunning() const;
  void Shutdown();

 private:
  Server() = default;

  MiniRedisConfig config_;
  std::vector<Database> databases_;
  ds::Dict<int, std::unique_ptr<Client>> clients_;
  bool running_ = false;

  // Runtime state
  ServerStats stats_;
  uint32_t lru_clock_ = 0;
  int child_pid_ = -1;
  int64_t last_save_ms_ = 0;
};

}  // namespace miniredis
