#pragma once

#include <string>
#include <vector>

namespace miniredis {

struct CommandContext;
class CommandRegistry;

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           const std::vector<std::string>& args);

}  // namespace miniredis
