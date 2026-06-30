#include "commands/dispatcher.h"

#include "commands/command_context.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "eviction/evict.h"

namespace miniredis {

namespace {

bool IsKnownZeroReplyNoMutation(std::string_view command_name) {
  return command_name == "DEL" || command_name == "EXPIRE" ||
         command_name == "PEXPIRE" || command_name == "EXPIREAT" ||
         command_name == "PEXPIREAT" || command_name == "PERSIST" ||
         command_name == "MSETNX" || command_name == "RENAMENX" ||
         command_name == "LPUSHX" || command_name == "RPUSHX" ||
         command_name == "HDEL" || command_name == "LREM" ||
         command_name == "ZREM" || command_name == "ZREMRANGEBYRANK" ||
         command_name == "ZREMRANGEBYSCORE";
}

bool IsKnownNilReplyNoMutation(std::string_view command_name) {
  return command_name == "SETNX" || command_name == "GETDEL" ||
         command_name == "LPOP" || command_name == "RPOP" ||
         command_name == "RPOPLPUSH" || command_name == "ZADD";
}

bool IsKnownEmptyArrayNoMutation(std::string_view command_name) {
  return command_name == "ZPOPMIN" || command_name == "ZPOPMAX";
}

bool IsWriteAllowedWhenOom(std::string_view command_name) {
  return command_name == "DEL" || command_name == "EXPIRE" ||
         command_name == "PEXPIRE" || command_name == "EXPIREAT" ||
         command_name == "PEXPIREAT" || command_name == "PERSIST" ||
         command_name == "FLUSHDB" || command_name == "FLUSHALL" ||
         command_name == "GETDEL" || command_name == "LPOP" ||
         command_name == "RPOP" || command_name == "LTRIM" ||
         command_name == "LREM" || command_name == "HDEL" ||
         command_name == "ZREM" || command_name == "ZPOPMIN" ||
         command_name == "ZPOPMAX" || command_name == "ZREMRANGEBYRANK" ||
         command_name == "ZREMRANGEBYSCORE";
}

}  // namespace

CommandResult ExecuteCommandDetailed(CommandRegistry& registry,
                                     CommandContext& context, CommandArgs args,
                                     bool replay_mode) {
  CommandResult result;

  if (args.empty()) {
    result.reply = RespReply::Error("ERR empty command");
    return result;
  }

  const CommandInfo* cmd = registry.Find(args[0]);
  if (!cmd) {
    result.reply = RespReply::UnknownCommand(args[0]);
    return result;
  }

  // Arity check
  int argc = static_cast<int>(args.size());
  if (cmd->arity > 0) {
    if (argc != cmd->arity) {
      result.reply = RespReply::Error("ERR wrong number of arguments for '" +
                                      cmd->name + "' command");
      return result;
    }
  } else if (cmd->arity < 0) {
    if (argc < -cmd->arity) {
      result.reply = RespReply::Error("ERR wrong number of arguments for '" +
                                      cmd->name + "' command");
      return result;
    }
  }

  bool is_write =
      (cmd->flags & static_cast<uint32_t>(CommandFlag::kWrite)) != 0;
  bool reject_for_oom = is_write && !IsWriteAllowedWhenOom(cmd->name);
  if (!replay_mode && ShouldRejectWriteForOom(context.server, reject_for_oom)) {
    result.reply = RespReply::Error(
        "OOM command not allowed when used memory > maxmemory");
    return result;
  }

  result.reply = cmd->func(context, args);

  // Determine ok / mutated.
  // ok = reply does NOT start with '-' (RESP error marker).
  result.ok = !result.reply.empty() && result.reply[0] != '-';

  if (result.ok) {
    // Conservative default for write commands: assume mutation.
    // Conditional commands (SETNX, MSETNX, EXPIRE, PERSIST, DEL, etc.) should
    // set propagate_args to empty when the condition was not met, so that
    // the caller can skip propagation. Until handlers report this directly,
    // only replies whose zero/nil result is known to mean "no mutation" are
    // filtered here.
    if (is_write) {
      result.mutated = true;
      if (result.reply == ":0\r\n" && IsKnownZeroReplyNoMutation(cmd->name)) {
        result.mutated = false;
      } else if (result.reply == RespReply::Nil() &&
                 IsKnownNilReplyNoMutation(cmd->name)) {
        result.mutated = false;
      } else if (result.reply == RespReply::EmptyArray() &&
                 IsKnownEmptyArrayNoMutation(cmd->name)) {
        result.mutated = false;
      }
    }
  }

  // Build propagate_args (only when not in replay mode and actually mutated).
  if (!replay_mode && result.ok && result.mutated) {
    if (result.propagate_args.empty()) {
      result.propagate_args = ToOwnedArgs(args);
    }
    context.server.IncrementDirty();
    if (context.propagate) {
      context.propagate(context.client.CurrentDb(), result.propagate_args);
    }
  }

  return result;
}

CommandResult ExecuteCommandDetailed(CommandRegistry& registry,
                                     CommandContext& context,
                                     const std::vector<std::string>& args,
                                     bool replay_mode) {
  auto views = ToArgViews(args);
  return ExecuteCommandDetailed(registry, context, CommandArgs{views},
                                replay_mode);
}

CommandResult ExecuteCommandDetailed(
    CommandRegistry& registry, CommandContext& context,
    std::initializer_list<std::string_view> args, bool replay_mode) {
  std::vector<std::string_view> views(args);
  return ExecuteCommandDetailed(registry, context, CommandArgs{views},
                                replay_mode);
}

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           CommandArgs args) {
  return ExecuteCommandDetailed(registry, context, args).reply;
}

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           const std::vector<std::string>& args) {
  auto views = ToArgViews(args);
  return ExecuteCommand(registry, context, CommandArgs{views});
}

std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           std::initializer_list<std::string_view> args) {
  std::vector<std::string_view> views(args);
  return ExecuteCommand(registry, context, CommandArgs{views});
}

}  // namespace miniredis
