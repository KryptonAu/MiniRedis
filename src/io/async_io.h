#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exec/completion_behavior.hpp>
#include <span>
#include <stdexec/execution.hpp>
#include <string>
#include <system_error>
#include <utility>

#include "io/epoll_context.h"

namespace miniredis {

// ===========================================================================
// EpollIoOpBase — typed base for I/O operation states.
// All I/O operations must be started on the context's IO thread.
// ===========================================================================
struct EpollIoOpBase {
  int fd = -1;

  virtual void OnReady(uint32_t events) noexcept = 0;
  virtual void OnStopped() noexcept = 0;
  virtual uint32_t Events() const noexcept = 0;

 protected:
  ~EpollIoOpBase() = default;
};

// ===========================================================================
// AsyncReadResult
// ===========================================================================
struct AsyncReadResult {
  size_t bytes_read = 0;
  bool eof = false;
};

// ===========================================================================
// AsyncReadSender
// ===========================================================================
class AsyncReadSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(AsyncReadResult),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  EpollContext* sched_ = nullptr;
  int fd_ = -1;
  std::span<char> buffer_;

  template <class Rcvr>
  struct OpState;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> OpState<Rcvr>;

  auto get_env() const noexcept -> IoAffineEnv { return {}; }
};

template <class Rcvr>
struct AsyncReadSender::OpState final : EpollIoOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  EpollContext* sched_;
  std::span<char> buffer_;
  Rcvr rcvr_;

  OpState(EpollContext* sched, int client_fd, std::span<char> buffer,
          Rcvr rcvr) noexcept
      : EpollIoOpBase{},
        sched_(sched),
        buffer_(buffer),
        rcvr_(std::move(rcvr)) {
    this->fd = client_fd;
  }

  OpState(OpState&&) = delete;

  void start() & noexcept;
  void OnReady(uint32_t events) noexcept override;
  void OnStopped() noexcept override;
  uint32_t Events() const noexcept override {
    return EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
  }
};

template <class Rcvr>
inline auto AsyncReadSender::connect(Rcvr rcvr) const noexcept
    -> AsyncReadSender::OpState<Rcvr> {
  return AsyncReadSender::OpState<Rcvr>{sched_, fd_, buffer_, std::move(rcvr)};
}

// ===========================================================================
// AsyncWriteSender
// ===========================================================================
class AsyncWriteSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(size_t),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  EpollContext* sched_ = nullptr;
  int fd_ = -1;
  std::string data_;

  template <class Rcvr>
  struct OpState;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> OpState<Rcvr>;

  auto get_env() const noexcept -> IoAffineEnv { return {}; }
};

template <class Rcvr>
struct AsyncWriteSender::OpState final : EpollIoOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  EpollContext* sched_;
  std::string data_;
  size_t offset_ = 0;
  Rcvr rcvr_;

  OpState(EpollContext* sched, int client_fd, std::string data,
          Rcvr rcvr) noexcept
      : EpollIoOpBase{},
        sched_(sched),
        data_(std::move(data)),
        rcvr_(std::move(rcvr)) {
    this->fd = client_fd;
  }

  OpState(OpState&&) = delete;

  void start() & noexcept;
  void OnReady(uint32_t events) noexcept override;
  void OnStopped() noexcept override;
  uint32_t Events() const noexcept override {
    return EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
  }
};

template <class Rcvr>
inline auto AsyncWriteSender::connect(Rcvr rcvr) const noexcept
    -> AsyncWriteSender::OpState<Rcvr> {
  return AsyncWriteSender::OpState<Rcvr>{sched_, fd_, data_, std::move(rcvr)};
}

// ===========================================================================
// AsyncAcceptSender
// ===========================================================================
class AsyncAcceptSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(int),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  EpollContext* sched_ = nullptr;
  int listen_fd_ = -1;

  template <class Rcvr>
  struct OpState;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> OpState<Rcvr>;

  auto get_env() const noexcept -> IoAffineEnv { return {}; }
};

template <class Rcvr>
struct AsyncAcceptSender::OpState final : EpollIoOpBase {
  using operation_state_concept = stdexec::operation_state_tag;

  EpollContext* sched_;
  Rcvr rcvr_;

  OpState(EpollContext* sched, int in_listen_fd, Rcvr rcvr) noexcept
      : EpollIoOpBase{}, sched_(sched), rcvr_(std::move(rcvr)) {
    this->fd = in_listen_fd;
  }

  OpState(OpState&&) = delete;

  void start() & noexcept;
  void OnReady(uint32_t events) noexcept override;
  void OnStopped() noexcept override;
  uint32_t Events() const noexcept override {
    return EPOLLIN | EPOLLERR | EPOLLHUP;
  }
};

template <class Rcvr>
inline auto AsyncAcceptSender::connect(Rcvr rcvr) const noexcept
    -> AsyncAcceptSender::OpState<Rcvr> {
  return AsyncAcceptSender::OpState<Rcvr>{sched_, listen_fd_, std::move(rcvr)};
}

// ===========================================================================
// Out-of-line implementations
// ===========================================================================

// -- AsyncReadSender::OpState -----------------------------------------------

template <class Rcvr>
inline void AsyncReadSender::OpState<Rcvr>::start() & noexcept {
  assert(sched_->IsOnThread());
  if (buffer_.empty()) {
    stdexec::set_value(std::move(rcvr_), AsyncReadResult{});
    return;
  }
  sched_->ArmIo(this);
}

template <class Rcvr>
inline void AsyncReadSender::OpState<Rcvr>::OnReady(uint32_t events) noexcept {
  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    ssize_t n = ::read(this->fd, buffer_.data(), buffer_.size());
    if (n > 0) {
      AsyncReadResult r;
      r.bytes_read = static_cast<size_t>(n);
      r.eof = false;
      stdexec::set_value(std::move(rcvr_), std::move(r));
      return;
    }
    if (n == 0) {
      AsyncReadResult r;
      r.eof = true;
      stdexec::set_value(std::move(rcvr_), std::move(r));
      return;
    }
    if (n < 0 && (errno == ECONNRESET)) {
      AsyncReadResult r;
      r.eof = true;
      stdexec::set_value(std::move(rcvr_), std::move(r));
      return;
    }
    stdexec::set_error(std::move(rcvr_),
                       std::make_exception_ptr(
                           std::system_error(errno, std::generic_category())));
    return;
  }

  // EPOLLIN: read available data.
  ssize_t n = ::read(this->fd, buffer_.data(), buffer_.size());
  if (n > 0) {
    AsyncReadResult r;
    r.bytes_read = static_cast<size_t>(n);
    r.eof = false;
    stdexec::set_value(std::move(rcvr_), std::move(r));
    return;
  }
  if (n == 0) {
    AsyncReadResult r;
    r.eof = true;
    stdexec::set_value(std::move(rcvr_), std::move(r));
    return;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    sched_->ArmIo(this);
    return;
  }
  stdexec::set_error(std::move(rcvr_),
                     std::make_exception_ptr(
                         std::system_error(errno, std::generic_category())));
}

template <class Rcvr>
inline void AsyncReadSender::OpState<Rcvr>::OnStopped() noexcept {
  stdexec::set_stopped(std::move(rcvr_));
}

// -- AsyncWriteSender::OpState ----------------------------------------------

template <class Rcvr>
inline void AsyncWriteSender::OpState<Rcvr>::start() & noexcept {
  assert(sched_->IsOnThread());
  if (data_.empty()) {
    stdexec::set_value(std::move(rcvr_), size_t{0});
    return;
  }
  if (sched_->IsStopping()) {
    stdexec::set_stopped(std::move(rcvr_));
    return;
  }

  ssize_t n = ::send(this->fd, data_.data() + offset_, data_.size() - offset_,
                     MSG_NOSIGNAL);
  if (n > 0) {
    offset_ += static_cast<size_t>(n);
    if (offset_ == data_.size()) {
      stdexec::set_value(std::move(rcvr_), data_.size());
      return;
    }
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    stdexec::set_error(std::move(rcvr_),
                       std::make_exception_ptr(
                           std::system_error(errno, std::generic_category())));
    return;
  }
  sched_->ArmIo(this);
}

template <class Rcvr>
inline void AsyncWriteSender::OpState<Rcvr>::OnReady(uint32_t events) noexcept {
  if (events & (EPOLLERR | EPOLLHUP)) {
    stdexec::set_error(std::move(rcvr_),
                       std::make_exception_ptr(
                           std::system_error(EPIPE, std::generic_category())));
    return;
  }

  while (offset_ < data_.size()) {
    ssize_t n = ::send(this->fd, data_.data() + offset_, data_.size() - offset_,
                       MSG_NOSIGNAL);
    if (n > 0) {
      offset_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      sched_->ArmIo(this);
      return;
    }
    if (n < 0) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::system_error(
                             errno, std::generic_category())));
      return;
    }
  }
  stdexec::set_value(std::move(rcvr_), data_.size());
}

template <class Rcvr>
inline void AsyncWriteSender::OpState<Rcvr>::OnStopped() noexcept {
  stdexec::set_stopped(std::move(rcvr_));
}

// -- AsyncAcceptSender::OpState ---------------------------------------------

template <class Rcvr>
inline void AsyncAcceptSender::OpState<Rcvr>::start() & noexcept {
  assert(sched_->IsOnThread());
  sched_->ArmIo(this);
}

template <class Rcvr>
inline void AsyncAcceptSender::OpState<Rcvr>::OnReady(
    uint32_t events) noexcept {
  if (events & (EPOLLERR | EPOLLHUP)) {
    stdexec::set_error(std::move(rcvr_),
                       std::make_exception_ptr(
                           std::system_error(EIO, std::generic_category())));
    return;
  }

  int client_fd =
      ::accept4(this->fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (client_fd >= 0) {
    int one = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    stdexec::set_value(std::move(rcvr_), client_fd);
    return;
  }

  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    sched_->ArmIo(this);
    return;
  }

  stdexec::set_error(std::move(rcvr_),
                     std::make_exception_ptr(
                         std::system_error(errno, std::generic_category())));
}

template <class Rcvr>
inline void AsyncAcceptSender::OpState<Rcvr>::OnStopped() noexcept {
  stdexec::set_stopped(std::move(rcvr_));
}

}  // namespace miniredis
