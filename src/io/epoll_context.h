#pragma once

#include <atomic>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

namespace miniredis {

// ---------------------------------------------------------------------------
// EpollOpBase — non-template base for the intrusive ready queue.
// ---------------------------------------------------------------------------
struct EpollOpBase {
  EpollOpBase* next_ = nullptr;
  virtual void Complete() noexcept = 0;

 protected:
  virtual ~EpollOpBase() = default;
};

// Forward-declared — defined in async_io.h.
struct EpollIoOpBase;
struct FdState;

// ===========================================================================
// Forward declarations
// ===========================================================================
class EpollContext;

// ===========================================================================
// EpollScheduleSender — schedule() sender
// ===========================================================================

template <class Rcvr>
struct EpollScheduleOpState;

class EpollScheduleSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_stopped_t()>;

  EpollContext* sched_ = nullptr;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> EpollScheduleOpState<Rcvr>;

  struct env;
  auto get_env() const noexcept -> env;
};

template <class Rcvr>
struct EpollScheduleOpState final : EpollOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  EpollContext* sched_;
  Rcvr rcvr_;

  EpollScheduleOpState(EpollContext* sched, Rcvr rcvr) noexcept
      : EpollOpBase{}, sched_(sched), rcvr_(std::move(rcvr)) {}

  EpollScheduleOpState(EpollScheduleOpState&&) = delete;

  void start() & noexcept;

  void Complete() noexcept override { stdexec::set_value(std::move(rcvr_)); }
};

// ===========================================================================
// EpollContext — owning epoll event loop + scheduler handle.
// ===========================================================================
class EpollContext {
 public:
  class scheduler {
   public:
    using scheduler_concept = stdexec::scheduler_tag;

    scheduler() = default;
    explicit scheduler(EpollContext* ctx) noexcept : ctx_(ctx) {}

    auto operator==(const scheduler&) const noexcept -> bool = default;

    auto schedule() const noexcept -> EpollScheduleSender {
      return EpollScheduleSender{ctx_};
    }

    auto query(stdexec::get_forward_progress_guarantee_t) const noexcept
        -> stdexec::forward_progress_guarantee {
      return stdexec::forward_progress_guarantee::parallel;
    }

    EpollContext* GetContext() const noexcept { return ctx_; }

   private:
    EpollContext* ctx_ = nullptr;
  };

  EpollContext();
  ~EpollContext();

  EpollContext(const EpollContext&) = delete;
  EpollContext& operator=(const EpollContext&) = delete;

  scheduler get_scheduler() noexcept { return scheduler{this}; }

  void Run();
  void Stop() noexcept;
  bool IsOnThread() const noexcept;
  bool IsStopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
  }

  bool Enqueue(EpollOpBase* op) noexcept;

  // -- I/O operation registration (must be called from IO thread) ----------
  // ArmIo() assumes the caller is on the IO thread; ScheduleArmIo()
  // is safe from any thread.
  void ArmIo(EpollIoOpBase* op);
  void ScheduleArmIo(EpollIoOpBase* op);
  void CancelFd(int fd) noexcept;

  int GetEpollFd() const { return epoll_fd_; }

 private:
  void ProcessReadyQueue();
  void DrainWakeFd() noexcept;
  void ProcessIoEvent(int fd, uint32_t events) noexcept;
  void RecomputeFdMask(EpollIoOpBase* op, bool add);
  void StopAllIoOps() noexcept;

  int epoll_fd_ = -1;
  int wake_fd_ = -1;

  std::atomic<EpollOpBase*> head_{nullptr};
  std::atomic<bool> stopping_{false};
  std::thread::id io_thread_id_{};

  // Per-fd state — accessed only from IO thread.
  std::unordered_map<int, FdState> fd_state_;
};

// ===========================================================================
// EpollScheduleSender::connect
// ===========================================================================
template <class Rcvr>
inline auto EpollScheduleSender::connect(Rcvr rcvr) const noexcept
    -> EpollScheduleOpState<Rcvr> {
  return EpollScheduleOpState<Rcvr>{sched_, std::move(rcvr)};
}

// ===========================================================================
// EpollScheduleSender::env
// ===========================================================================
struct EpollScheduleSender::env {
  EpollContext* sched_;

  auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>)
      const noexcept -> EpollContext::scheduler {
    return EpollContext::scheduler{sched_};
  }

  auto query(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>)
      const noexcept -> EpollContext::scheduler {
    return EpollContext::scheduler{sched_};
  }
};

inline auto EpollScheduleSender::get_env() const noexcept
    -> EpollScheduleSender::env {
  return EpollScheduleSender::env{sched_};
}

// ===========================================================================
// EpollScheduleOpState::start
// ===========================================================================
template <class Rcvr>
inline void EpollScheduleOpState<Rcvr>::start() & noexcept {
  if (!sched_->Enqueue(this)) {
    stdexec::set_stopped(std::move(rcvr_));
  }
}

}  // namespace miniredis
