#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/client.h"
#include "core/database.h"
#include "core/server.h"

namespace miniredis {

struct CommandContext {
  using PropagateFn = std::function<bool(int, const std::vector<std::string>&)>;
  using ApplyConfigFn = std::function<bool(std::string_view, std::string_view)>;

  Server& server;
  Client& client;
  Database& db;
  PropagateFn propagate;
  ApplyConfigFn apply_config;

  CommandContext(Server& srv, Client& cli, Database& database)
      : server(srv), client(cli), db(database) {}
};

}  // namespace miniredis
