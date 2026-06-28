#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "commands/command_context.h"
#include "commands/dispatcher.h"
#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"

namespace miniredis {

struct CommandTestHarness {
  Server& server = Server::Instance();
  Client client{1};

  CommandTestHarness() {
    MiniRedisConfig cfg;
    cfg.databases = 4;
    server.Init(cfg);
  }

  CommandContext Context() {
    return {server, client, *server.GetDbFor(client)};
  }

  std::string Call(CommandRegistry& registry,
                   const std::vector<std::string>& args) {
    auto ctx = Context();
    return ExecuteCommand(registry, ctx, args);
  }
};

inline std::vector<std::string> ParseBulkArray(std::string_view resp) {
  // Simple parser for RESP array of bulk strings (for test assertions)
  std::vector<std::string> result;
  if (resp.empty() || resp[0] != '*') return result;
  size_t pos = 1;
  while (pos < resp.size() && resp[pos] != '\r') pos++;
  pos += 2;
  while (pos < resp.size()) {
    if (resp[pos] == '*') return result;  // nested array, stop
    if (resp[pos] != '$') return result;
    pos++;
    size_t len = 0;
    while (pos < resp.size() && resp[pos] != '\r') {
      len = len * 10 + static_cast<size_t>(resp[pos] - '0');
      pos++;
    }
    pos += 2;
    result.emplace_back(resp.substr(pos, len));
    pos += len + 2;
  }
  return result;
}

inline bool IsErr(std::string_view resp) { return resp.starts_with("-ERR"); }

inline bool IsWrongType(std::string_view resp) {
  return resp.starts_with("-WRONGTYPE");
}

inline bool ContainsBulk(std::string_view resp, std::string_view value) {
  auto elements = ParseBulkArray(resp);
  return std::find(elements.begin(), elements.end(), value) != elements.end();
}

}  // namespace miniredis
