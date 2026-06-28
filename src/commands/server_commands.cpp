#include <chrono>

#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "core/server.h"
#include "types/numeric_parse.h"

namespace miniredis {

static std::string PingCmd(CommandContext&,
                           const std::vector<std::string>& args) {
  if (args.size() == 1) return RespReply::SimpleString("PONG");
  if (args.size() == 2) return RespReply::BulkString(args[1]);
  return WrongArity("PING");
}

static std::string EchoCmd(CommandContext&,
                           const std::vector<std::string>& args) {
  return RespReply::BulkString(args[1]);
}

static std::string SelectCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  int index = 0;
  auto parsed = ParseCanonicalInt(args[1]);
  if (!std::holds_alternative<ParsedInt>(parsed)) return InvalidInteger();
  index = static_cast<int>(std::get<ParsedInt>(parsed).value);
  if (!ctx.client.SelectDb(index, ctx.server.DbCount()))
    return InvalidDbIndex();
  return RespReply::Ok();
}

static std::string DbSizeCmd(CommandContext& ctx,
                             const std::vector<std::string>&) {
  return RespReply::Integer(static_cast<int64_t>(ctx.db.Size()));
}

static std::string FlushDbCmd(CommandContext& ctx,
                              const std::vector<std::string>&) {
  ctx.db.Clear();
  return RespReply::Ok();
}

static std::string FlushAllCmd(CommandContext& ctx,
                               const std::vector<std::string>&) {
  ctx.server.FlushAll();
  return RespReply::Ok();
}

static std::string TimeCmd(CommandContext&, const std::vector<std::string>&) {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  int64_t us =
      std::chrono::duration_cast<std::chrono::microseconds>(now).count() %
      1000000;
  std::vector<std::string> v = {RespReply::BulkString(std::to_string(sec)),
                                RespReply::BulkString(std::to_string(us))};
  return RespReply::ArrayOfEncoded(v);
}

static std::string CommandCmd(CommandContext&,
                              const std::vector<std::string>& args) {
  if (args.size() != 1) return Unsupported("COMMAND subcommand");
  auto registry = CreateDefaultCommandRegistry();
  return RespReply::ArrayOfBulkStrings(registry.CommandNames());
}

static std::string ConfigCmd(CommandContext& ctx,
                             const std::vector<std::string>& args) {
  if (args.size() < 3) return WrongArity("CONFIG");
  if (args[1] == "GET") {
    if (args.size() != 3) return WrongArity("CONFIG");
    auto& cfg = ctx.server.GetConfig();
    std::string key(args[2]);
    std::vector<std::string> result;
    if (key == "databases") {
      result = {key, std::to_string(cfg.databases)};
    } else if (key == "port") {
      result = {key, std::to_string(cfg.port)};
    } else if (key == "bind") {
      result = {key, cfg.bind};
    } else {
      return RespReply::EmptyArray();
    }
    return RespReply::ArrayOfBulkStrings(result);
  }
  if (args[1] == "SET") {
    if (args.size() != 4) return WrongArity("CONFIG");
    return NotImplemented("CONFIG SET");
  }
  return SyntaxError();
}

static std::string InfoCmd(CommandContext&,
                           const std::vector<std::string>& args) {
  if (args.size() > 2) return WrongArity("INFO");
  return RespReply::BulkString("# Server\r\nmini_redis_version:0.1.0\r\n");
}

void RegisterServerCommands(CommandRegistry& registry) {
  auto ro = static_cast<uint32_t>(CommandFlag::kReadOnly);
  auto wr = static_cast<uint32_t>(CommandFlag::kWrite);
  auto adm = static_cast<uint32_t>(CommandFlag::kAdmin);
  registry.Register({"PING", -1, ro, PingCmd});
  registry.Register({"ECHO", 2, ro, EchoCmd});
  registry.Register({"SELECT", 2, adm, SelectCmd});
  registry.Register({"DBSIZE", 1, ro, DbSizeCmd});
  registry.Register({"FLUSHDB", 1, wr | adm, FlushDbCmd});
  registry.Register({"FLUSHALL", 1, wr | adm, FlushAllCmd});
  registry.Register({"TIME", 1, ro, TimeCmd});
  registry.Register({"COMMAND", 1, ro, CommandCmd});
  registry.Register({"CONFIG", -3, adm, ConfigCmd});
  registry.Register({"INFO", -1, ro, InfoCmd});
}

}  // namespace miniredis
