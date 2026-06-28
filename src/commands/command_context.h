#pragma once

#include "core/client.h"
#include "core/database.h"
#include "core/server.h"

namespace miniredis {

struct CommandContext {
  Server& server;
  Client& client;
  Database& db;
};

}  // namespace miniredis
