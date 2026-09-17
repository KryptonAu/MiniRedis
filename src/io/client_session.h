#pragma once

#include <unistd.h>

#include <array>
#include <cassert>
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
#include "io/cmd_batch.h"
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
    EpollContext::scheduler io_sched, CmdContext& cmd_ctx, int client_fd,
    Server& server, CommandRegistry& registry,
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
    CmdBatchContext batch_ctx{.server = &server,
                              .client = client,
                              .registry = &registry,
                              .propagate = &propagate,
                              .apply_config = &apply_config,
                              .args = {},
                              .count = 0};

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
          // One sender operation for the whole batch. Its operation state is
          // the CMD context's queue node: start() is a bare Enqueue, the CMD
          // thread runs the batch in a single virtual call, and only the
          // completion goes back through the framework to the IO thread.
          batch_ctx.args =
              std::span<const std::span<const std::string_view>>(
                  batch_args.data(), count);
          batch_ctx.count = count;
          std::string replies = co_await CmdBatchSender{
              &cmd_ctx, io_sched.GetContext(), &batch_ctx};
          // CmdBatchSender declares itself affine to the IO thread and delivers
          // its completion there, so this coroutine is back on the IO thread
          // and may touch parser/reply_buf without any further hand-off. The
          // only exception is when the IO context has already stopped, in which
          // case the completion is delivered in place (server shutdown).
          assert(io_sched.GetContext()->IsStopping() ||
                 io_sched.GetContext()->IsOnThread());
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
    CmdContext& cmd_ctx, Server& server, CommandRegistry& registry,
    int listen_fd, CommandContext::PropagateFn propagate = {},
    CommandContext::ApplyConfigFn apply_config = {}) {
  while (true) {
    try {
      int client_fd =
          co_await AsyncAcceptSender{io_sched.GetContext(), listen_fd};
      scope.spawn(stdexec::starts_on(
          io_sched, handle_client(io_sched, cmd_ctx, client_fd, server,
                                  registry, propagate, apply_config)));
    } catch (const std::exception&) {
      co_return;
    }
  }
}

}  // namespace miniredis
