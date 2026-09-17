#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

#include "commands/command_context.h"
#include "commands/dispatcher.h"
#include "commands/registry.h"
#include "core/client.h"
#include "core/database.h"
#include "core/resp_protocol.h"
#include "core/server.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"

namespace miniredis {

// ---------------------------------------------------------------------------
// CmdBatchContext — everything the CMD thread needs to run one batch.
// Owned by the client coroutine frame; it must stay alive until the batch has
// completed, which the co_await guarantees.
// ---------------------------------------------------------------------------
struct CmdBatchContext {
  Server* server = nullptr;
  Client* client = nullptr;
  CommandRegistry* registry = nullptr;
  const CommandContext::PropagateFn* propagate = nullptr;
  const CommandContext::ApplyConfigFn* apply_config = nullptr;
  // One entry per command, each entry holding that command's argument views.
  std::span<const std::span<const std::string_view>> args;
  size_t count = 0;
};

// Runs the batch in order and returns the concatenated RESP replies. This is a
// plain function: no sender, receiver or coroutine machinery is involved.
inline std::string RunCommandBatch(const CmdBatchContext& batch) {
  std::string out;
  for (size_t i = 0; i < batch.count; ++i) {
    // Resolved per command so that a SELECT inside the batch affects every
    // following command.
    Database* db = batch.server->GetDbFor(*batch.client);
    if (db == nullptr) {
      out += RespReply::Error("ERR invalid DB index");
      continue;
    }
    CommandContext ctx{*batch.server, *batch.client, *db};
    // The callbacks are owned by the coroutine frame and only used while this
    // batch runs. Keep a reference wrapper in CommandContext to avoid cloning
    // their type-erased targets for every request.
    if (*batch.propagate) ctx.propagate = std::cref(*batch.propagate);
    if (*batch.apply_config) ctx.apply_config = std::cref(*batch.apply_config);
    out += ExecuteCommand(*batch.registry, ctx, batch.args[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// CmdBatchSender
//
// A sender whose operation state *is* the CMD thread's SPSC queue node:
//   * start()  — a bare CmdContext::Enqueue, with no framework in between;
//   * Complete() (CMD thread) — runs the whole batch in one virtual call and
//     then posts an intrusive node back to the IO thread;
//   * the IO thread completes the receiver there.
//
// The completion behavior is declared `asynchronous_affine` so that exec::task's
// await_transform connects the receiver directly instead of wrapping the
// awaitable in `continues_on(get_start_scheduler(*__context_))`, i.e. the
// type-erased any_scheduler path. That declaration is only truthful because the
// completion is delivered on the IO thread, which is where start() runs.
// ---------------------------------------------------------------------------
class CmdBatchSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(std::string),
                                     stdexec::set_stopped_t()>;

  CmdBatchSender(CmdContext* cmd_ctx, EpollContext* io_ctx,
                 CmdBatchContext* batch) noexcept
      : cmd_ctx_(cmd_ctx), io_ctx_(io_ctx), batch_(batch) {}

  template <class Rcvr>
  struct OpState;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> OpState<Rcvr> {
    return OpState<Rcvr>{cmd_ctx_, io_ctx_, batch_, std::move(rcvr)};
  }

  auto get_env() const noexcept -> IoAffineEnv { return {}; }

 private:
  CmdContext* cmd_ctx_;
  EpollContext* io_ctx_;
  CmdBatchContext* batch_;
};

template <class Rcvr>
struct CmdBatchSender::OpState final : CmdOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  CmdContext* cmd_ctx_;
  EpollContext* io_ctx_;
  CmdBatchContext* batch_;
  Rcvr rcvr_;
  // Written on the CMD thread, read on the IO thread. The hand-off goes through
  // EpollContext::Enqueue (release) / ProcessReadyQueue (acquire).
  std::string result_;
  bool stopped_ = false;

  // Intrusive completion node for the return hop. It lives inside the operation
  // state, so returning a result allocates nothing. It carries its own `next_`
  // link, so it never aliases the CmdOpBase link used for the submission.
  struct Deliver final : EpollOpBase {
    OpState* self_;
    explicit Deliver(OpState* self) noexcept : self_(self) {}
    void Complete() noexcept override {
      if (self_->stopped_) {
        stdexec::set_stopped(std::move(self_->rcvr_));
      } else {
        stdexec::set_value(std::move(self_->rcvr_), std::move(self_->result_));
      }
    }
  } deliver_{this};

  OpState(CmdContext* cmd_ctx, EpollContext* io_ctx, CmdBatchContext* batch,
          Rcvr rcvr) noexcept
      : cmd_ctx_(cmd_ctx),
        io_ctx_(io_ctx),
        batch_(batch),
        rcvr_(std::move(rcvr)) {}

  OpState(OpState&&) = delete;

  // ---- IO thread: one bare enqueue, nothing else --------------------------
  void start() & noexcept {
    if (!cmd_ctx_->Enqueue(this)) {
      // The CMD context is stopping or its bounded queue is full. Completing
      // inline keeps the affine contract (we are still on the IO thread).
      stdexec::set_stopped(std::move(rcvr_));
    }
  }

  // ---- CMD thread: run the whole batch, then hand the result back ---------
  void Complete() noexcept override {
    result_ = RunCommandBatch(*batch_);
    DeliverToIo(/*stopped=*/false);
  }

  // Called by CmdContext's shutdown drain, on the CMD thread.
  void CompleteStopped() noexcept override { DeliverToIo(/*stopped=*/true); }

 private:
  void DeliverToIo(bool stopped) noexcept {
    stopped_ = stopped;
    if (io_ctx_->Enqueue(&deliver_)) return;

    // The IO context has stopped, so its ready queue will never run again.
    // Mirror the fallback the previous `continues_on` path had and complete in
    // place; this only happens while the server is shutting down.
    if (stopped) {
      stdexec::set_stopped(std::move(rcvr_));
    } else {
      stdexec::set_value(std::move(rcvr_), std::move(result_));
    }
  }
};

}  // namespace miniredis
