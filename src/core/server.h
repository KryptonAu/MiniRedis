#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "core/client.h"
#include "core/config.h"
#include "core/database.h"
#include "ds/dict.h"
#include "types/encoding_thresholds.h"

namespace miniredis {

class Server {
 public:
  static Server& Instance();

  bool Init(const MiniRedisConfig& config);

  Database* GetDb(int index);
  const Database* GetDb(int index) const;
  Database* GetDbFor(const Client& client);
  std::optional<size_t> DbSize(int index);
  bool FlushDb(int index);
  void FlushAll();
  int DbCount() const;

  Client* CreateClient(int fd);
  void RemoveClient(int fd);
  Client* FindClient(int fd);
  size_t ClientCount() const;

  const MiniRedisConfig& GetConfig() const;
  EncodingThresholds GetEncodingThresholds() const;

  bool IsRunning() const;
  void Shutdown();

 private:
  Server() = default;

  MiniRedisConfig config_;
  std::vector<Database> databases_;
  ds::Dict<int, std::unique_ptr<Client>> clients_;
  bool running_ = false;
};

}  // namespace miniredis
