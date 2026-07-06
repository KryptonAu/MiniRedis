#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexec/execution.hpp>
#include <thread>
#include <utility>
#include <vector>

#include "core/config.h"

namespace miniredis {

// ---------------------------------------------------------------------------
// CmdOpBase — non-template base for the CMD-task queue.
// ---------------------------------------------------------------------------
struct CmdOpBase {
  CmdOpBase* next_ = nullptr;
  virtual void Complete() noexcept = 0;
  virtual void CompleteStopped() noexcept = 0;

 protected:
  virtual ~CmdOpBase() = default;
};

class CmdContext;

// ===========================================================================
// CmdScheduleSender
// ===========================================================================

template <class Rcvr>
struct CmdScheduleOpState;

class CmdScheduleSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_stopped_t()>;

  CmdContext* sched_ = nullptr;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> CmdScheduleOpState<Rcvr>;

  struct env;
  auto get_env() const noexcept -> env;
};

template <class Rcvr>
struct CmdScheduleOpState final : CmdOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  CmdContext* sched_;
  Rcvr rcvr_;

  CmdScheduleOpState(CmdContext* sched, Rcvr rcvr) noexcept
      : CmdOpBase{}, sched_(sched), rcvr_(std::move(rcvr)) {}

  CmdScheduleOpState(CmdScheduleOpState&&) = delete;

  void start() & noexcept;

  void Complete() noexcept override { stdexec::set_value(std::move(rcvr_)); }
  void CompleteStopped() noexcept override {
    stdexec::set_stopped(std::move(rcvr_));
  }
};

template <class Rcvr>
inline auto CmdScheduleSender::connect(Rcvr rcvr) const noexcept
    -> CmdScheduleOpState<Rcvr> {
  return CmdScheduleOpState<Rcvr>{sched_, std::move(rcvr)};
}

// ===========================================================================
// CmdContext — owning single-threaded command execution context.
// The queue is SPSC: one producer thread schedules work for the CMD thread.
// ===========================================================================
class CmdContext {
 public:
  class scheduler {
   public:
    using scheduler_concept = stdexec::scheduler_tag;

    scheduler() = default;
    explicit scheduler(CmdContext* ctx) noexcept : ctx_(ctx) {}

    auto operator==(const scheduler&) const noexcept -> bool = default;

    auto schedule() const noexcept -> CmdScheduleSender {
      return CmdScheduleSender{ctx_};
    }

    auto query(stdexec::get_forward_progress_guarantee_t) const noexcept
        -> stdexec::forward_progress_guarantee {
      return stdexec::forward_progress_guarantee::parallel;
    }

   private:
    CmdContext* ctx_ = nullptr;
  };

  explicit CmdContext(
      size_t queue_capacity = MiniRedisConfig{}.command_queue_capacity);
  ~CmdContext() = default;

  CmdContext(const CmdContext&) = delete;
  CmdContext& operator=(const CmdContext&) = delete;

  scheduler get_scheduler() noexcept { return scheduler{this}; }

  void Run();
  void Stop();
  bool IsOnThread() const noexcept;

  // Returns false if the context is stopping or the bounded SPSC queue is full
  // (caller should deliver set_stopped() immediately). Single-producer only.
  bool Enqueue(CmdOpBase* op) noexcept;

  // Post a function to be executed on the CMD thread. Returns false if
  // stopping or the bounded SPSC queue is full. Single-producer only.
  bool PostFunction(std::function<void()> fn);

 private:
  CmdOpBase* TryDequeue() noexcept;
  void WakeConsumer() noexcept;
  void FinishEnqueue() noexcept;
  static size_t NormalizeQueueCapacity(size_t queue_capacity) noexcept;

  static constexpr size_t kCacheLineSize = 64;

  struct alignas(kCacheLineSize) PaddedAtomicSize {
    std::atomic<size_t> value{0};
  };

  struct alignas(kCacheLineSize) PaddedAtomicBool {
    std::atomic<bool> value{false};
  };

  std::vector<CmdOpBase*> queue_;
  size_t queue_mask_ = 0;
  PaddedAtomicSize head_;
  PaddedAtomicSize tail_;
  PaddedAtomicSize wake_sequence_;
  PaddedAtomicSize active_enqueues_;
  PaddedAtomicBool stopping_;
  std::thread::id cmd_thread_id_{};
};

// ===========================================================================
// CmdScheduleSender::env
// ===========================================================================
struct CmdScheduleSender::env {
  CmdContext* sched_;

  auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>)
      const noexcept -> CmdContext::scheduler {
    return CmdContext::scheduler{sched_};
  }

  auto query(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>)
      const noexcept -> CmdContext::scheduler {
    return CmdContext::scheduler{sched_};
  }
};

inline auto CmdScheduleSender::get_env() const noexcept
    -> CmdScheduleSender::env {
  return CmdScheduleSender::env{sched_};
}

// ===========================================================================
// CmdScheduleOpState::start
// ===========================================================================
template <class Rcvr>
inline void CmdScheduleOpState<Rcvr>::start() & noexcept {
  if (!sched_->Enqueue(this)) {
    stdexec::set_stopped(std::move(rcvr_));
  }
}

// ===========================================================================
// Inline implementations
// ===========================================================================
inline CmdContext::CmdContext(size_t queue_capacity)
    : queue_(NormalizeQueueCapacity(queue_capacity), nullptr),
      queue_mask_(queue_.size() - 1) {}

inline void CmdContext::Run() {
  cmd_thread_id_ = std::this_thread::get_id();

  while (true) {
    if (auto* op = TryDequeue(); op != nullptr) {
      op->Complete();
      continue;
    }

    if (stopping_.value.load(std::memory_order_acquire) &&
        active_enqueues_.value.load(std::memory_order_acquire) == 0) {
      break;
    }

    const size_t observed =
        wake_sequence_.value.load(std::memory_order_acquire);

    if (auto* op = TryDequeue(); op != nullptr) {
      op->Complete();
      continue;
    }

    if (stopping_.value.load(std::memory_order_acquire) &&
        active_enqueues_.value.load(std::memory_order_acquire) == 0) {
      break;
    }

    wake_sequence_.value.wait(observed, std::memory_order_acquire);
  }

  // Drain remaining queued operations — complete them with set_stopped.
  while (true) {
    auto* op = TryDequeue();
    if (op == nullptr) break;
    op->CompleteStopped();
  }
}

inline void CmdContext::Stop() {
  stopping_.value.store(true, std::memory_order_release);
  WakeConsumer();
}

inline bool CmdContext::IsOnThread() const noexcept {
  return std::this_thread::get_id() == cmd_thread_id_;
}

inline bool CmdContext::Enqueue(CmdOpBase* op) noexcept {
  // If stopping, reject so the caller can deliver set_stopped().
  if (stopping_.value.load(std::memory_order_acquire)) return false;

  active_enqueues_.value.fetch_add(1, std::memory_order_acq_rel);

  if (stopping_.value.load(std::memory_order_acquire)) {
    FinishEnqueue();
    return false;
  }

  const size_t tail = tail_.value.load(std::memory_order_relaxed);
  const size_t head = head_.value.load(std::memory_order_acquire);
  if (tail - head >= queue_.size()) {
    FinishEnqueue();
    return false;
  }

  queue_[tail & queue_mask_] = op;
  tail_.value.store(tail + 1, std::memory_order_release);
  FinishEnqueue();
  WakeConsumer();
  return true;
}

inline CmdOpBase* CmdContext::TryDequeue() noexcept {
  const size_t head = head_.value.load(std::memory_order_relaxed);
  const size_t tail = tail_.value.load(std::memory_order_acquire);
  if (head == tail) return nullptr;

  const size_t slot = head & queue_mask_;
  auto* op = queue_[slot];
  queue_[slot] = nullptr;
  head_.value.store(head + 1, std::memory_order_release);
  return op;
}

inline void CmdContext::WakeConsumer() noexcept {
  wake_sequence_.value.fetch_add(1, std::memory_order_release);
  wake_sequence_.value.notify_one();
}

inline void CmdContext::FinishEnqueue() noexcept {
  const size_t previous =
      active_enqueues_.value.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 1 && stopping_.value.load(std::memory_order_acquire)) {
    WakeConsumer();
  }
}

inline size_t CmdContext::NormalizeQueueCapacity(
    size_t queue_capacity) noexcept {
  if (queue_capacity <= 1) return 1;
  if (std::has_single_bit(queue_capacity)) return queue_capacity;

  constexpr size_t kMaxPowerOfTwo =
      size_t{1} << (std::numeric_limits<size_t>::digits - 1);
  if (queue_capacity > kMaxPowerOfTwo) return kMaxPowerOfTwo;
  return std::bit_ceil(queue_capacity);
}

inline bool CmdContext::PostFunction(std::function<void()> fn) {
  struct FuncOp : CmdOpBase {
    std::function<void()> func;
    explicit FuncOp(std::function<void()> f) : func(std::move(f)) {}
    void Complete() noexcept override {
      func();
      delete this;
    }
    void CompleteStopped() noexcept override { delete this; }
  };
  auto* op = new FuncOp(std::move(fn));
  if (!Enqueue(op)) {
    delete op;
    return false;
  }
  return true;
}

}  // namespace miniredis
