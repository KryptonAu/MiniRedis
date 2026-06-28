#include "commands/dispatcher.h"

#include "commands/command_context.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"

namespace miniredis {

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           const std::vector<std::string>& args) {
  if (args.empty()) return RespReply::Error("ERR empty command");

  const CommandInfo* cmd = registry.Find(args[0]);
  if (!cmd) return RespReply::UnknownCommand(args[0]);

  // Arity check
  int argc = static_cast<int>(args.size());
  if (cmd->arity > 0) {
    if (argc != cmd->arity)
      return RespReply::Error("ERR wrong number of arguments for '" +
                              cmd->name + "' command");
  } else if (cmd->arity < 0) {
    if (argc < -cmd->arity)
      return RespReply::Error("ERR wrong number of arguments for '" +
                              cmd->name + "' command");
  }

  return cmd->func(context, args);
}

}  // namespace miniredis
