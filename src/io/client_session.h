#pragma once

#include <unistd.h>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <functional>
#include <stdexec/execution.hpp>
#include <string>
#include <utility>
#include <vector>

#include "commands/command_context.h"
#include "commands/dispatcher.h"
#include "commands/registry.h"
#include "core/client.h"
#include "core/database.h"
#include "core/resp_protocol.h"
#include "core/server.h"
#include "io/async_io.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"

namespace miniredis {

template <typename F>
struct ScopeExit {
  F fn_;
  explicit ScopeExit(F fn) noexcept : fn_(std::move(fn)) {}
  ~ScopeExit() { fn_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
};

// ---------------------------------------------------------------------------
// handle_client
// ---------------------------------------------------------------------------
inline exec::task<void> handle_client(
    EpollContext::scheduler io_sched, CmdContext::scheduler cmd_sched,
    int client_fd, Server& server, CommandRegistry& registry,
    CommandContext::PropagateFn propagate = {},
    CommandContext::ApplyConfigFn apply_config = {}) {
  Client* client = server.CreateClient(client_fd);
  if (client == nullptr) {
    ::close(client_fd);
    co_return;
  }

  auto cleanup = ScopeExit([&] {
    ::close(client_fd);
    server.RemoveClient(client_fd);
  });

  try {
    auto& parser = client->Parser();
    auto& reply_buf = client->ReplyBuffer();

    while (true) {
      AsyncReadResult read =
          co_await AsyncReadSender{io_sched.GetContext(), client_fd};
      if (read.eof) co_return;

      ParseStatus status = parser.Feed(read.data);
      if (status == ParseStatus::kError) {
        std::string err_reply = RespReply::Error("ERR protocol error");
        AsyncWriteSender writer{io_sched.GetContext(), client_fd,
                                std::move(err_reply)};
        (void)co_await std::move(writer);
        co_return;
      }

      while (parser.HasCommand()) {
        std::vector<std::string> args = parser.TakeCommand();

        std::string reply = co_await stdexec::starts_on(
            cmd_sched,
            stdexec::just(std::move(args)) |
                stdexec::then([&](std::vector<std::string> cmd_args) {
                  Database* db = server.GetDbFor(*client);
                  if (db == nullptr) {
                    return RespReply::Error("ERR invalid DB index");
                  }
                  CommandContext ctx{server, *client, *db};
                  ctx.propagate = propagate;
                  ctx.apply_config = apply_config;
                  return ExecuteCommand(registry, ctx, cmd_args);
                }));
        // Transfer back to the IO thread before touching parser/reply_buf.
        co_await io_sched.schedule();
        reply_buf += reply;
      }

      if (!reply_buf.empty()) {
        std::string out = std::exchange(reply_buf, {});
        AsyncWriteSender writer{io_sched.GetContext(), client_fd,
                                std::move(out)};
        (void)co_await std::move(writer);
      }
    }
  } catch (...) {
    co_return;
  }
}

// ---------------------------------------------------------------------------
// accept_loop
// ---------------------------------------------------------------------------
inline exec::task<void> accept_loop(
    exec::async_scope& scope, EpollContext::scheduler io_sched,
    CmdContext::scheduler cmd_sched, Server& server, CommandRegistry& registry,
    int listen_fd, CommandContext::PropagateFn propagate = {},
    CommandContext::ApplyConfigFn apply_config = {}) {
  while (true) {
    try {
      int client_fd =
          co_await AsyncAcceptSender{io_sched.GetContext(), listen_fd};
      scope.spawn(stdexec::starts_on(
          io_sched, handle_client(io_sched, cmd_sched, client_fd, server,
                                  registry, propagate, apply_config)));
    } catch (const std::exception&) {
      co_return;
    }
  }
}

}  // namespace miniredis
