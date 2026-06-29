#pragma once

#include <cstdint>
#include <functional>

namespace miniredis {

class Server;

struct CronServices {
  std::function<void()> flush_aof_if_needed;
  std::function<void()> trigger_autosave;
};

void ServerCron(Server& server, CronServices services, int64_t now_ms);

}  // namespace miniredis
