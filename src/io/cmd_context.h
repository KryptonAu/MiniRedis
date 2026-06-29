#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>

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

  CmdContext() = default;
  ~CmdContext() = default;

  CmdContext(const CmdContext&) = delete;
  CmdContext& operator=(const CmdContext&) = delete;

  scheduler get_scheduler() noexcept { return scheduler{this}; }

  void Run();
  void Stop();
  bool IsOnThread() const noexcept;

  // Returns false if the context is stopping (caller should deliver
  // set_stopped() immediately). Thread-safe.
  bool Enqueue(CmdOpBase* op) noexcept;

  // Post a function to be executed on the CMD thread. Returns false if
  // stopping. Thread-safe.
  bool PostFunction(std::function<void()> fn);

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<CmdOpBase*> queue_;
  std::atomic<bool> stopping_{false};
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
inline void CmdContext::Run() {
  cmd_thread_id_ = std::this_thread::get_id();

  while (true) {
    CmdOpBase* op = nullptr;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return !queue_.empty() || stopping_.load(std::memory_order_acquire);
      });

      if (queue_.empty() && stopping_.load(std::memory_order_acquire)) break;

      if (!queue_.empty()) {
        op = queue_.front();
        queue_.pop_front();
      }
    }

    if (op != nullptr) {
      op->Complete();
    }
  }

  // Drain remaining queued operations — complete them with set_stopped.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      auto* op = queue_.front();
      queue_.pop_front();
      lock.unlock();
      op->CompleteStopped();
      lock.lock();
    }
  }
}

inline void CmdContext::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_.store(true, std::memory_order_release);
  }
  cv_.notify_all();
}

inline bool CmdContext::IsOnThread() const noexcept {
  return std::this_thread::get_id() == cmd_thread_id_;
}

inline bool CmdContext::Enqueue(CmdOpBase* op) noexcept {
  // If stopping, reject so the caller can deliver set_stopped().
  if (stopping_.load(std::memory_order_acquire)) return false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Double-check under the lock.
    if (stopping_.load(std::memory_order_relaxed)) return false;
    queue_.push_back(op);
  }
  cv_.notify_one();
  return true;
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
