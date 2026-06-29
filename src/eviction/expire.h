#pragma once

#include <cstddef>
#include <cstdint>

namespace miniredis {

class Server;

struct ActiveExpireConfig {
  int keys_per_loop = 20;
  int slow_time_perc = 25;
  int fast_duration_us = 1000;
};

// Returns the number of keys expired during this cycle.
size_t ActiveExpireCycle(Server& server, int64_t now_ms,
                         const ActiveExpireConfig& config = {});

}  // namespace miniredis
