#pragma once

#include <string>
#include <vector>

namespace miniredis {

struct CommandContext;
class CommandRegistry;
struct CommandInfo;

struct CommandResult {
  std::string reply;
  bool ok = false;       // handler succeeded (not a RESP error)
  bool mutated = false;  // actually changed database state
  std::vector<std::string> propagate_args;  // args to propagate (empty = use
                                            // original)
};

CommandResult ExecuteCommandDetailed(CommandRegistry& registry,
                                     CommandContext& context,
                                     const std::vector<std::string>& args,
                                     bool replay_mode = false);

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           const std::vector<std::string>& args);

}  // namespace miniredis
