#include <csignal>
#include <exec/async_scope.hpp>
#include <iostream>
#include <stdexec/execution.hpp>
#include <thread>

#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"
#include "io/client_session.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"
#include "io/socket_util.h"

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

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  std::cout << "MiniRedis v0.1.0" << std::endl;

  // 1. Load configuration.
  MiniRedisConfig config;

  // 2. Initialize server.
  Server& server = Server::Instance();
  if (!server.Init(config)) {
    std::cerr << "Failed to initialize server" << std::endl;
    return 1;
  }

  // 3. Create command registry.
  auto registry = CreateDefaultCommandRegistry();

  // 4. Create listening socket.
  int listen_fd =
      CreateListenSocket(config.bind, config.port, config.tcp_backlog);
  if (listen_fd < 0) {
    std::cerr << "Failed to create listening socket on " << config.bind << ":"
              << config.port << std::endl;
    return 1;
  }
  std::cout << "Listening on " << config.bind << ":" << config.port
            << std::endl;

  // 5. Block SIGINT/SIGTERM before starting worker threads.
  SetupSignalHandling();

  // 6. Create contexts.
  EpollContext io_ctx;
  CmdContext cmd_ctx;
  auto io_sched = io_ctx.get_scheduler();
  auto cmd_sched = cmd_ctx.get_scheduler();

  exec::async_scope scope;

  // 7. Start CMD thread.
  std::thread cmd_thread([&] { cmd_ctx.Run(); });

  // 8. Start IO thread with accept loop.
  std::thread io_thread([&] {
    scope.spawn(stdexec::starts_on(
        io_sched,
        accept_loop(scope, io_sched, cmd_sched, server, registry, listen_fd)));
    io_ctx.Run();
  });

  // 9. Wait for shutdown signal.
  WaitForShutdown();

  // 10. Stop accepting (IO context stops accept + pending I/O ops).
  io_ctx.Stop();

  // 11. Wait for all scoped client tasks to complete.
  //     Use sync_wait to block until scope is empty.
  stdexec::sync_wait(scope.on_empty());

  // 12. Stop CMD context — no more commands will be processed.
  cmd_ctx.Stop();

  // 13. Join worker threads.
  io_thread.join();
  cmd_thread.join();

  // 14. Clean up.
  ::close(listen_fd);
  server.Shutdown();

  std::cout << "MiniRedis stopped." << std::endl;
  return 0;
}
