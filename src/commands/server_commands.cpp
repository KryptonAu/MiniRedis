#include <chrono>
#include <sstream>

#include "commands/command_context.h"
#include "commands/command_helpers.h"
#include "commands/registry.h"
#include "core/resp_protocol.h"
#include "core/server.h"
#include "persistence/rdb_serialize.h"
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

static std::string SaveCmd(CommandContext& ctx,
                           const std::vector<std::string>&) {
  RdbSerializer serializer;
  auto& cfg = ctx.server.GetConfig();
  if (!serializer.Save(cfg.rdb_filename, ctx.server)) {
    return RespReply::Error("ERR failed to save RDB");
  }
  ctx.server.ResetDirty();
  auto now = std::chrono::system_clock::now().time_since_epoch();
  ctx.server.SetLastSaveMs(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  return RespReply::Ok();
}

static std::string LastSaveCmd(CommandContext& ctx,
                               const std::vector<std::string>&) {
  int64_t ms = ctx.server.GetLastSaveMs();
  if (ms == 0) return RespReply::Integer(0);
  return RespReply::Integer(ms / 1000);
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
    } else if (key == "maxmemory") {
      result = {key, std::to_string(cfg.maxmemory)};
    } else if (key == "maxmemory-policy" || key == "maxmemory_policy") {
      result = {key, cfg.maxmemory_policy};
    } else if (key == "maxmemory-samples" || key == "maxmemory_samples") {
      result = {key, std::to_string(cfg.maxmemory_samples)};
    } else if (key == "appendonly") {
      result = {key, cfg.appendonly ? "yes" : "no"};
    } else if (key == "appendfsync") {
      result = {key, cfg.appendfsync};
    } else if (key == "dbfilename") {
      result = {key, cfg.rdb_filename};
    } else if (key == "appendfilename") {
      result = {key, cfg.aof_filename};
    } else if (key == "hz") {
      result = {key, std::to_string(cfg.hz)};
    } else if (key == "*") {
      // Return all config
      result = {"databases",
                std::to_string(cfg.databases),
                "port",
                std::to_string(cfg.port),
                "bind",
                cfg.bind,
                "maxmemory",
                std::to_string(cfg.maxmemory),
                "maxmemory-policy",
                cfg.maxmemory_policy,
                "maxmemory-samples",
                std::to_string(cfg.maxmemory_samples),
                "appendonly",
                cfg.appendonly ? "yes" : "no",
                "appendfsync",
                cfg.appendfsync,
                "dbfilename",
                cfg.rdb_filename,
                "appendfilename",
                cfg.aof_filename,
                "hz",
                std::to_string(cfg.hz)};
    } else {
      return RespReply::EmptyArray();
    }
    return RespReply::ArrayOfBulkStrings(result);
  }
  if (args[1] == "SET") {
    if (args.size() != 4) return WrongArity("CONFIG");
    bool ok = ctx.apply_config ? ctx.apply_config(args[2], args[3])
                               : ctx.server.ApplyConfig(args[2], args[3]);
    if (!ok) {
      return RespReply::Error(
          "ERR Unsupported CONFIG parameter or invalid value");
    }
    return RespReply::Ok();
  }
  return SyntaxError();
}

static std::string InfoCmd(CommandContext& ctx,
                           const std::vector<std::string>& args) {
  if (args.size() > 2) return WrongArity("INFO");
  std::ostringstream oss;

  // Server
  oss << "# Server\r\n";
  oss << "mini_redis_version:0.1.0\r\n";

  // Stats
  const auto& stats = ctx.server.Stats();
  oss << "# Stats\r\n";
  oss << "keyspace_hits:" << stats.keyspace_hits << "\r\n";
  oss << "keyspace_misses:" << stats.keyspace_misses << "\r\n";
  oss << "expired_keys:" << stats.expired_keys << "\r\n";
  oss << "evicted_keys:" << stats.evicted_keys << "\r\n";

  // Persistence
  oss << "# Persistence\r\n";
  oss << "rdb_last_save_time:" << (ctx.server.GetLastSaveMs() / 1000) << "\r\n";

  // Memory
  oss << "# Memory\r\n";
  oss << "used_memory:" << ctx.server.ApproxMemoryUsage() << "\r\n";
  oss << "maxmemory:" << ctx.server.GetConfig().maxmemory << "\r\n";
  oss << "maxmemory_policy:" << ctx.server.GetConfig().maxmemory_policy
      << "\r\n";

  return RespReply::BulkString(oss.str());
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
  registry.Register({"SAVE", 1, adm, SaveCmd});
  registry.Register({"LASTSAVE", 1, ro, LastSaveCmd});
}

}  // namespace miniredis
