#pragma once

#include <cstddef>
#include <cstdint>

namespace miniredis {

class Server;

// Returns the number of keys evicted.  kNoMemory means eviction failed to
// free enough memory.
enum class EvictionResult { kOk, kNoMemory };

EvictionResult PerformEvictions(Server& server);
bool ShouldRejectWriteForOom(const Server& server, bool is_write_command);

}  // namespace miniredis
