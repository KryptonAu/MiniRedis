#pragma once

#include <string>

namespace miniredis {

// Create a non-blocking listening socket bound to the given address/port.
// Returns the fd on success, -1 on failure.
// When port == 0 the kernel selects a free port; the actual port can be
// retrieved via getsockname().
int CreateListenSocket(const std::string& bind_addr, int port,
                       int backlog = 128);

}  // namespace miniredis
