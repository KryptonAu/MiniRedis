#include "io/epoll_context.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "io/async_io.h"

namespace miniredis {

EpollContext::EpollContext() {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "epoll_create1 failed");
  }

  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
    throw std::system_error(errno, std::generic_category(), "eventfd failed");
  }

  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLERR;
  ev.data.fd = wake_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
    ::close(wake_fd_);
    ::close(epoll_fd_);
    wake_fd_ = -1;
    epoll_fd_ = -1;
    throw std::system_error(errno, std::generic_category(),
                            "epoll_ctl(wake_fd) failed");
  }

  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ >= 0) {
    epoll_event tev{};
    tev.events = EPOLLIN | EPOLLERR;
    tev.data.fd = timer_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &tev) < 0) {
      ::close(timer_fd_);
      timer_fd_ = -1;
    }
  }
}

EpollContext::~EpollContext() {
  if (timer_fd_ >= 0) {
    ::close(timer_fd_);
    timer_fd_ = -1;
  }
  if (wake_fd_ >= 0) {
    ::close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

void EpollContext::Run() {
  io_thread_id_ = std::this_thread::get_id();

  constexpr int kMaxEvents = 256;
  epoll_event events[kMaxEvents];

  while (!stopping_.load(std::memory_order_relaxed)) {
    ProcessReadyQueue();

    if (stopping_.load(std::memory_order_relaxed)) break;

    int nfds = ::epoll_wait(epoll_fd_, events, kMaxEvents, -1);
    if (nfds < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      int fd = events[i].data.fd;
      uint32_t ev = events[i].events;

      if (fd == wake_fd_) {
        DrainWakeFd();
        continue;
      }
      if (fd == timer_fd_) {
        uint64_t expirations = 0;
        while (::read(timer_fd_, &expirations, sizeof(expirations)) > 0) {
        }
        if (timer_callback_) {
          timer_callback_(expirations);
        }
        continue;
      }

      ProcessIoEvent(fd, ev);
    }
  }

  // Drain remaining schedule ops.
  ProcessReadyQueue();

  // Complete all pending I/O ops with set_stopped().
  StopAllIoOps();
}

void EpollContext::Stop() noexcept {
  stopping_.store(true, std::memory_order_release);

  uint64_t one = 1;
  while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

bool EpollContext::IsOnThread() const noexcept {
  return std::this_thread::get_id() == io_thread_id_;
}

bool EpollContext::Enqueue(EpollOpBase* op) noexcept {
  if (stopping_.load(std::memory_order_acquire)) return false;

  op->next_ = head_.load(std::memory_order_relaxed);
  while (!head_.compare_exchange_weak(op->next_, op, std::memory_order_release,
                                      std::memory_order_relaxed)) {
  }

  uint64_t one = 1;
  while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
  return true;
}

// -- I/O management ----------------------------------------------------------

FdState& EpollContext::EnsureFdState(int fd) {
  const size_t index = static_cast<size_t>(fd);
  if (index >= fd_state_.size()) {
    fd_state_.resize(index + 1);
  }
  return fd_state_[index];
}

FdState* EpollContext::FindFdState(int fd) noexcept {
  if (fd < 0) return nullptr;

  const size_t index = static_cast<size_t>(fd);
  if (index >= fd_state_.size()) return nullptr;

  auto& st = fd_state_[index];
  return st.fd == fd ? &st : nullptr;
}

void EpollContext::ScheduleArmIo(EpollIoOpBase* op) {
  if (IsStopping()) {
    op->OnStopped();
    return;
  }

  if (IsOnThread()) {
    ArmIo(op);
    return;
  }

  // Enqueue a thunk that will call ArmIo() when processed on the IO thread.
  // The thunk is heap-allocated and self-deleting after execution.
  struct ArmIoThunk : EpollOpBase {
    EpollContext* sched;
    EpollIoOpBase* io_op;
    explicit ArmIoThunk(EpollContext* s, EpollIoOpBase* op)
        : EpollOpBase{}, sched(s), io_op(op) {}
    void Complete() noexcept override {
      if (sched->IsStopping()) {
        io_op->OnStopped();
      } else {
        sched->ArmIo(io_op);
      }
      delete this;
    }
  };
  auto* thunk = new ArmIoThunk(this, op);
  if (!Enqueue(thunk)) {
    delete thunk;
    op->OnStopped();
  }
}

void EpollContext::ArmIo(EpollIoOpBase* op) {
  if (op->fd < 0) {
    op->OnReady(EPOLLERR);
    return;
  }

  auto* st = &EnsureFdState(op->fd);
  st->fd = op->fd;

  uint32_t ev_mask = op->Events();

  // Check for duplicate pending op (programmer error).
  if ((ev_mask & EPOLLIN) && st->read_op != nullptr) {
    auto* pending_op = st->read_op;
    st->read_op = nullptr;
    pending_op->OnReady(EPOLLERR);
    st = &EnsureFdState(op->fd);
  }
  if ((ev_mask & EPOLLOUT) && st->write_op != nullptr) {
    auto* pending_op = st->write_op;
    st->write_op = nullptr;
    pending_op->OnReady(EPOLLERR);
    st = &EnsureFdState(op->fd);
  }

  // Assign the op to the appropriate slot.
  if (ev_mask & EPOLLIN) {
    st->read_op = op;
  }
  if (ev_mask & EPOLLOUT) {
    st->write_op = op;
  }

  // Compute the new mask and update epoll.
  uint32_t new_mask = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
  if (st->read_op != nullptr) new_mask |= EPOLLIN;
  if (st->write_op != nullptr) new_mask |= EPOLLOUT;
  if (st->accept_op != nullptr) new_mask |= EPOLLIN;

  int op_code = (st->armed_events == 0) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  epoll_event ev{};
  ev.events = new_mask;
  ev.data.fd = op->fd;

  if (::epoll_ctl(epoll_fd_, op_code, op->fd, &ev) < 0) {
    // If ADD fails because it's already registered, try MOD.
    if (op_code == EPOLL_CTL_ADD && errno == EEXIST) {
      ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, op->fd, &ev);
    }
  }
  st->armed_events = new_mask;
}

void EpollContext::CancelFd(int fd) noexcept {
  if (fd < 0) return;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

  auto* st = FindFdState(fd);
  if (st == nullptr) return;

  if (st->read_op != nullptr) {
    auto* op = st->read_op;
    st->read_op = nullptr;
    op->OnStopped();
    st = FindFdState(fd);
  }
  if (st != nullptr && st->write_op != nullptr) {
    auto* op = st->write_op;
    st->write_op = nullptr;
    op->OnStopped();
    st = FindFdState(fd);
  }
  if (st != nullptr && st->accept_op != nullptr) {
    auto* op = st->accept_op;
    st->accept_op = nullptr;
    op->OnStopped();
    st = FindFdState(fd);
  }

  if (st != nullptr) *st = FdState{};
}

// -- private -----------------------------------------------------------------

void EpollContext::ProcessReadyQueue() {
  auto* ops = head_.exchange(nullptr, std::memory_order_acquire);

  EpollOpBase* prev = nullptr;
  while (ops != nullptr) {
    auto* next = ops->next_;
    ops->next_ = prev;
    prev = ops;
    ops = next;
  }

  while (prev != nullptr) {
    auto* next = prev->next_;
    prev->Complete();
    prev = next;
  }
}

void EpollContext::DrainWakeFd() noexcept {
  uint64_t dummy;
  while (::read(wake_fd_, &dummy, sizeof(dummy)) > 0) {
  }
}

void EpollContext::ProcessIoEvent(int fd, uint32_t events) noexcept {
  auto* st = FindFdState(fd);
  if (st == nullptr) return;

  // Read/accept readiness
  if ((events & EPOLLIN) && st->read_op != nullptr) {
    auto* op = st->read_op;
    st->read_op = nullptr;
    st->armed_events &= ~EPOLLIN;
    RecomputeFdMask(op, /*add=*/false);
    op->OnReady(events);
    return;
  }

  // Write readiness
  if ((events & EPOLLOUT) && st->write_op != nullptr) {
    auto* op = st->write_op;
    st->write_op = nullptr;
    st->armed_events &= ~EPOLLOUT;
    RecomputeFdMask(op, /*add=*/false);
    op->OnReady(events);
    return;
  }

  // Error / hangup — deliver to read op if present, else write op.
  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    if (st->read_op != nullptr) {
      auto* op = st->read_op;
      st->read_op = nullptr;
      st->armed_events = 0;
      RecomputeFdMask(op, /*add=*/false);
      op->OnReady(events);
      return;
    }
    if (st->write_op != nullptr) {
      auto* op = st->write_op;
      st->write_op = nullptr;
      st->armed_events = 0;
      RecomputeFdMask(op, /*add=*/false);
      op->OnReady(events);
      return;
    }
  }
}

void EpollContext::RecomputeFdMask(EpollIoOpBase* op, bool add) {
  // Recompute the epoll mask from FdState and update the kernel.
  int fd = op->fd;
  auto* st = FindFdState(fd);
  if (st == nullptr) return;

  uint32_t new_mask = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
  if (st->read_op != nullptr) new_mask |= EPOLLIN;
  if (st->write_op != nullptr) new_mask |= EPOLLOUT;
  if (st->accept_op != nullptr) new_mask |= EPOLLIN;

  if (new_mask == (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    // No more ops on this fd — remove from epoll.
    st->armed_events = 0;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    *st = FdState{};
  } else {
    // Update the interest mask.
    int op_code = (st->armed_events != 0) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event ev{};
    ev.events = new_mask;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, op_code, fd, &ev);
    st->armed_events = new_mask;
  }

  (void)add;  // Reserved for future edge-triggered mode.
}

bool EpollContext::ArmPeriodicTimer(uint64_t interval_ms,
                                    TimerCallback callback) {
  if (timer_fd_ < 0) return false;
  timer_callback_ = std::move(callback);

  struct itimerspec its {};
  its.it_interval.tv_sec = static_cast<time_t>(interval_ms / 1000);
  its.it_interval.tv_nsec = static_cast<long>((interval_ms % 1000) * 1000000);
  its.it_value = its.it_interval;

  if (::timerfd_settime(timer_fd_, 0, &its, nullptr) < 0) {
    return false;
  }
  return true;
}

void EpollContext::DisarmTimer() noexcept {
  if (timer_fd_ < 0) return;
  timer_callback_ = nullptr;
  struct itimerspec its {};
  ::timerfd_settime(timer_fd_, 0, &its, nullptr);
}

void EpollContext::StopAllIoOps() noexcept {
  DisarmTimer();
  for (auto& st : fd_state_) {
    if (st.fd < 0) continue;

    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, st.fd, nullptr);
    if (st.read_op != nullptr) {
      st.read_op->OnStopped();
      st.read_op = nullptr;
    }
    if (st.write_op != nullptr) {
      st.write_op->OnStopped();
      st.write_op = nullptr;
    }
    if (st.accept_op != nullptr) {
      st.accept_op->OnStopped();
      st.accept_op = nullptr;
    }
  }
  fd_state_.clear();
}

}  // namespace miniredis
