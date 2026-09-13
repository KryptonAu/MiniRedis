#pragma once

#include <unistd.h>

#include <array>
#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <functional>
#include <span>
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
    auto& query_buf = client->QueryBuffer();
    auto& reply_buf = client->ReplyBuffer();

    // Pipelined requests are parsed and executed as a batch, so a batch of N
    // commands costs one CMD-thread hand-off instead of N. Each slot owns the
    // argument views of one command in the batch; the bulk bytes stay in
    // QueryBuffer.
    constexpr size_t kMaxBatch = 16;
    std::array<CommandArgStorage, kMaxBatch> batch_storage;
    std::array<std::span<const std::string_view>, kMaxBatch> batch_args;

    while (true) {
      std::span<char> write_buf = query_buf.PrepareWrite();
      AsyncReadResult read =
          co_await AsyncReadSender{io_sched.GetContext(), client_fd, write_buf};
      if (read.eof) co_return;
      query_buf.CommitWrite(read.bytes_read);

      while (true) {
        // Parse every complete command currently buffered, up to kMaxBatch.
        const std::string_view readable = query_buf.Readable();
        size_t scanned = 0;
        size_t count = 0;
        bool protocol_error = false;
        while (count < kMaxBatch) {
          RespParseResult parsed =
              parser.ParseNext(readable.substr(scanned), batch_storage[count]);
          if (parsed.status == ParseStatus::kIncomplete) break;
          if (parsed.status == ParseStatus::kError) {
            protocol_error = true;
            break;
          }
          batch_args[count] = parsed.command.Args();
          scanned += parsed.consumed;
          ++count;
        }

        if (count > 0) {
          // One sender operation for the whole batch: the CMD thread runs the
          // loop and returns one aggregated reply blob. Composing the
          // scheduler sender directly with `then` (rather than
          // `starts_on(cmd_sched, just() | then(...))`) drops an extra value
          // sender and the `schedule_from` sequencing op state.
          std::string replies =
              co_await (cmd_sched.schedule() | stdexec::then([&]() {
                          std::string out;
                          for (size_t i = 0; i < count; ++i) {
                            // Resolved per command so a SELECT inside the
                            // batch affects every following command.
                            Database* db = server.GetDbFor(*client);
                            if (db == nullptr) {
                              out += RespReply::Error("ERR invalid DB index");
                              continue;
                            }
                            CommandContext ctx{server, *client, *db};
                            // The callbacks are owned by this coroutine frame
                            // and only used while executing this awaited
                            // batch. Keep a reference wrapper in
                            // CommandContext to avoid cloning their
                            // type-erased targets for every request.
                            if (propagate) ctx.propagate = std::cref(propagate);
                            if (apply_config) {
                              ctx.apply_config = std::cref(apply_config);
                            }
                            out += ExecuteCommand(registry, ctx, batch_args[i]);
                          }
                          return out;
                        }));
          // `exec::task`'s sticky scheduler already resumed this coroutine on
          // the IO thread, so touching parser/reply_buf here is safe without an
          // extra hand-off. An explicit `co_await io_sched.schedule()` at this
          // point only added an eventfd wakeup plus a ready-queue round trip
          // per command (measured: ~57-68% of pipelined throughput).
          query_buf.Consume(scanned);
          for (size_t i = 0; i < count; ++i) {
            batch_storage[i].ReleaseOversizedHeap();
          }
          reply_buf += replies;
        }

        if (protocol_error) {
          // Flush the replies of the commands that were already executed
          // before the malformed bytes, like Redis does, then report the
          // protocol error and drop the connection.
          reply_buf += RespReply::Error("ERR protocol error");
          std::string out = std::exchange(reply_buf, {});
          AsyncWriteSender writer{io_sched.GetContext(), client_fd,
                                  std::move(out)};
          (void)co_await std::move(writer);
          co_return;
        }

        // A short batch means parsing stopped on a partial command, so go read
        // more; a full batch may still have complete commands behind it.
        if (count < kMaxBatch) break;
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
