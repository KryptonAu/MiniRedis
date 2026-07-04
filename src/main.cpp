#include <chrono>
#include <csignal>
#include <exec/async_scope.hpp>
#include <iostream>
#include <stdexec/execution.hpp>
#include <string_view>
#include <thread>
#include <vector>

#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"
#include "eviction/server_cron.h"
#include "io/client_session.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"
#include "io/socket_util.h"
#include "persistence/aof.h"
#include "persistence/rdb_deserialize.h"
#include "persistence/rdb_serialize.h"

using namespace miniredis;

namespace {

std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int) { g_shutdown_requested.store(true); }

void SetupSignalHandling() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  struct sigaction sa {};
  sa.sa_handler = SignalHandler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

void WaitForShutdown() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);

  int sig = 0;
  sigwait(&set, &sig);
  std::cout << "\nReceived signal " << sig << ", shutting down..." << std::endl;
  g_shutdown_requested.store(true);
}

int64_t NowMs() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  std::cout << "MiniRedis v0.2.0" << std::endl;

  MiniRedisConfig config;
  Server& server = Server::Instance();
  if (!server.Init(config)) {
    std::cerr << "Failed to initialize server" << std::endl;
    return 1;
  }

  auto registry = CreateDefaultCommandRegistry();

  // Load persistence data before starting threads.
  int aof_selected_db = 0;
  if (config.appendonly) {
    AofReader reader;
    std::vector<std::vector<std::string>> commands;
    if (reader.ReadCommands(config.aof_filename, commands)) {
      std::cout << "Replaying AOF: " << commands.size() << " commands"
                << std::endl;
      // Create a pseudo client for replay.
      Client replay_client(-1, 0);
      for (auto& args : commands) {
        if (args.empty()) continue;
        // Handle SELECT to switch DB.
        if (args[0] == "SELECT" && args.size() >= 2) {
          int idx = std::stoi(args[1]);
          if (replay_client.SelectDb(idx, server.DbCount())) {
            aof_selected_db = replay_client.CurrentDb();
          }
          continue;
        }
        Database* db = server.GetDbFor(replay_client);
        if (!db) continue;
        CommandContext ctx{server, replay_client, *db};
        ExecuteCommandDetailed(registry, ctx, args, /*replay_mode=*/true);
      }
      aof_selected_db = replay_client.CurrentDb();
    }
  } else {
    RdbDeserializer loader;
    if (loader.Load(config.rdb_filename, server)) {
      std::cout << "RDB loaded from " << config.rdb_filename << std::endl;
    }
  }

  int listen_fd =
      CreateListenSocket(config.bind, config.port, config.tcp_backlog);
  if (listen_fd < 0) {
    std::cerr << "Failed to create listening socket on " << config.bind << ":"
              << config.port << std::endl;
    return 1;
  }
  std::cout << "Listening on " << config.bind << ":" << config.port
            << std::endl;

  SetupSignalHandling();

  EpollContext io_ctx;
  CmdContext cmd_ctx(config.command_queue_capacity);
  auto io_sched = io_ctx.get_scheduler();
  auto cmd_sched = cmd_ctx.get_scheduler();

  exec::async_scope scope;

  // AOF writer (created on CMD thread after threads start).
  AofWriter aof_writer;
  bool aof_ready = false;

  auto reopen_aof = [&]() -> bool {
    const auto& cfg = server.GetConfig();
    if (aof_ready) {
      aof_writer.Close();
      aof_ready = false;
    }
    if (!cfg.appendonly) return true;
    aof_ready = aof_writer.Open(cfg.aof_filename, cfg.appendfsync);
    return aof_ready;
  };

  CommandContext::PropagateFn propagate_command =
      [&](int db_index, const std::vector<std::string>& args) {
        if (!aof_ready) return true;
        return aof_writer.AppendCommandForDb(db_index, args, aof_selected_db);
      };

  CommandContext::ApplyConfigFn apply_runtime_config =
      [&](std::string_view key, std::string_view value) {
        MiniRedisConfig old_cfg = server.GetConfig();
        if (!server.ApplyConfig(key, value)) return false;

        std::string normalized(key);
        for (auto& ch : normalized) {
          if (ch == '-') ch = '_';
        }
        bool aof_related =
            normalized == "appendonly" || normalized == "appendfsync" ||
            normalized == "appendfilename" || normalized == "aof_filename";
        if (!aof_related) return true;

        if (reopen_aof()) return true;

        server.ApplyConfig("appendonly", old_cfg.appendonly ? "yes" : "no");
        server.ApplyConfig("appendfilename", old_cfg.aof_filename);
        server.ApplyConfig("appendfsync", old_cfg.appendfsync);
        (void)reopen_aof();
        return false;
      };

  std::thread cmd_thread([&] {
    // Open AOF if enabled.
    (void)reopen_aof();
    cmd_ctx.Run();
  });

  std::thread io_thread([&] {
    scope.spawn(stdexec::starts_on(
        io_sched,
        accept_loop(scope, io_sched, cmd_sched, server, registry, listen_fd,
                    propagate_command, apply_runtime_config)));

    // Arm periodic timer for serverCron.
    uint64_t cron_interval_ms =
        static_cast<uint64_t>(1000 / (config.hz > 0 ? config.hz : 10));
    io_ctx.ArmPeriodicTimer(cron_interval_ms, [&](uint64_t /*exp*/) {
      cmd_ctx.PostFunction([&] {
        CronServices services;
        services.flush_aof_if_needed = [&] {
          if (aof_ready) {
            aof_writer.FlushIfNeeded(NowMs());
          }
        };
        services.trigger_autosave = [&] {
          const auto& cfg = server.GetConfig();
          if (cfg.save_enabled && server.Stats().dirty > 0) {
            // Check save params.
            for (auto& [seconds, changes] : cfg.save_params) {
              if (server.Stats().dirty >= static_cast<uint64_t>(changes)) {
                int64_t last = server.GetLastSaveMs();
                if (last == 0 || (NowMs() - last) >= seconds * 1000) {
                  RdbSerializer serializer;
                  if (serializer.Save(cfg.rdb_filename, server)) {
                    server.ResetDirty();
                    server.SetLastSaveMs(NowMs());
                  }
                  break;
                }
              }
            }
          }
        };
        ServerCron(server, services, NowMs());
      });
    });
    io_ctx.Run();
  });

  WaitForShutdown();

  io_ctx.Stop();
  stdexec::sync_wait(scope.on_empty());
  cmd_ctx.Stop();

  io_thread.join();
  cmd_thread.join();

  // Flush and close AOF before cleanup.
  if (aof_ready) {
    aof_writer.Close();
  }

  ::close(listen_fd);
  server.Shutdown();

  std::cout << "MiniRedis stopped." << std::endl;
  return 0;
}
