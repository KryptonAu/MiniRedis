#include "io/async_io.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>

#include "io/epoll_context.h"
#include "io/socket_util.h"

namespace miniredis {
namespace {

// -- Helpers ---------------------------------------------------------------

static std::pair<int, int> MakeSocketPair() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
    throw std::system_error(errno, std::generic_category(), "socketpair");
  }
  return {fds[0], fds[1]};
}

// RAII thread joiner.
struct ThreadGuard {
  std::thread& t_;
  explicit ThreadGuard(std::thread& t) : t_(t) {}
  ~ThreadGuard() {
    if (t_.joinable()) t_.join();
  }
};

// Wraps an EpollContext + IO thread, guaranteeing Stop()+join on destruction.
struct IoLoop {
  EpollContext ctx;
  std::thread io_thread;
  ThreadGuard guard{io_thread};

  explicit IoLoop() {
    io_thread = std::thread([this] { ctx.Run(); });
  }

  ~IoLoop() {
    ctx.Stop();
    // guard destructor joins io_thread
  }
};

// =========================================================================
// async_read
// =========================================================================

TEST(AsyncIoTest, AsyncReadReceivesData) {
  auto [rfd, wfd] = MakeSocketPair();

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  const std::string msg = "hello";
  ASSERT_EQ(::write(wfd, msg.data(), msg.size()),
            static_cast<ssize_t>(msg.size()));

  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&io.ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         EXPECT_FALSE(r.eof);
                                         EXPECT_EQ(r.data, msg);
                                       }));
  stdexec::sync_wait(std::move(sender));

  close(rfd);
  close(wfd);
}

TEST(AsyncIoTest, AsyncReadDetectsEof) {
  auto [rfd, wfd] = MakeSocketPair();

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  ::close(wfd);

  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&io.ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         EXPECT_TRUE(r.eof);
                                         EXPECT_TRUE(r.data.empty());
                                       }));
  stdexec::sync_wait(std::move(sender));

  close(rfd);
}

TEST(AsyncIoTest, AsyncReadHandlesPartialData) {
  auto [rfd, wfd] = MakeSocketPair();

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  std::string big_msg(65536, 'x');
  ASSERT_EQ(::write(wfd, big_msg.data(), big_msg.size()),
            static_cast<ssize_t>(big_msg.size()));

  std::string collected;
  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&io.ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         collected = std::move(r.data);
                                         EXPECT_FALSE(r.eof);
                                         EXPECT_GT(collected.size(), 0u);
                                       }));
  stdexec::sync_wait(std::move(sender));

  close(rfd);
  close(wfd);
}

// =========================================================================
// async_write
// =========================================================================

TEST(AsyncIoTest, AsyncWriteSendsData) {
  auto [rfd, wfd] = MakeSocketPair();

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  const std::string msg = "world";
  auto sender =
      stdexec::starts_on(io_sched, AsyncWriteSender{&io.ctx, rfd, msg} |
                                       stdexec::then([&](size_t written) {
                                         EXPECT_EQ(written, msg.size());
                                       }));
  stdexec::sync_wait(std::move(sender));

  // Read back from the other side.
  std::array<char, 256> buf{};
  ssize_t n = ::read(wfd, buf.data(), buf.size());
  EXPECT_EQ(n, static_cast<ssize_t>(msg.size()));
  EXPECT_EQ(std::string(buf.data(), static_cast<size_t>(n)), msg);

  close(rfd);
  close(wfd);
}

TEST(AsyncIoTest, AsyncWriteHandlesLargePayload) {
  auto [rfd, wfd] = MakeSocketPair();

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  std::string big_msg(128 * 1024, 'y');
  std::string collected;
  std::atomic<bool> writer_done{false};
  std::thread reader([&] {
    std::array<char, 8192> buf{};
    while (collected.size() < big_msg.size()) {
      pollfd pfd{};
      pfd.fd = wfd;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      int ready = ::poll(&pfd, 1, 50);
      if (ready == 0) {
        if (writer_done.load()) break;
        continue;
      }
      if (ready < 0) break;
      ssize_t n = ::read(wfd, buf.data(), buf.size());
      if (n <= 0) break;
      collected.append(buf.data(), static_cast<size_t>(n));
    }
  });

  auto sender =
      stdexec::starts_on(io_sched, AsyncWriteSender{&io.ctx, rfd, big_msg} |
                                       stdexec::then([&](size_t written) {
                                         EXPECT_EQ(written, big_msg.size());
                                       }));
  std::exception_ptr write_error;
  try {
    stdexec::sync_wait(std::move(sender));
  } catch (...) {
    write_error = std::current_exception();
  }

  writer_done = true;
  ::shutdown(rfd, SHUT_WR);
  reader.join();
  if (write_error) std::rethrow_exception(write_error);
  EXPECT_EQ(collected, big_msg);

  close(rfd);
  close(wfd);
}

// =========================================================================
// Shutdown
// =========================================================================

TEST(AsyncIoTest, PendingReadReceivesStoppedOnShutdown) {
  auto [rfd, wfd] = MakeSocketPair();

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();
  std::thread io_thread([&] { ctx.Run(); });
  ThreadGuard io_guard(io_thread);

  std::atomic<bool> stopped{false};
  std::thread waiter([&] {
    auto sender = stdexec::starts_on(
        io_sched, AsyncReadSender{&ctx, rfd} | stdexec::upon_stopped([&] {
                    stopped = true;
                    return AsyncReadResult{};
                  }));
    stdexec::sync_wait(std::move(sender));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ctx.Stop();
  waiter.join();
  EXPECT_TRUE(stopped.load());

  ::close(rfd);
  ::close(wfd);
}

TEST(AsyncIoTest, ScheduleAfterStopCompletesStopped) {
  auto [rfd, wfd] = MakeSocketPair();

  EpollContext ctx;
  ctx.Stop();

  bool stopped = false;
  auto sender = AsyncReadSender{&ctx, rfd} | stdexec::upon_stopped([&] {
                  stopped = true;
                  return AsyncReadResult{};
                });
  stdexec::sync_wait(std::move(sender));

  EXPECT_TRUE(stopped);
  ::close(rfd);
  ::close(wfd);
}

// =========================================================================
// async_accept
// =========================================================================

TEST(AsyncIoTest, AsyncAcceptReturnsClientFd) {
  int listen_fd = CreateListenSocket("127.0.0.1", 0);
  ASSERT_GE(listen_fd, 0);

  sockaddr_in addr{};
  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(
      ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen),
      0);
  int port = ntohs(addr.sin_port);

  IoLoop io;
  auto io_sched = io.ctx.get_scheduler();

  int connect_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  ASSERT_GE(connect_fd, 0);

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);

  int ret =
      ::connect(connect_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
  if (ret < 0) {
    EXPECT_EQ(errno, EINPROGRESS);
  }

  std::atomic<int> accepted_fd{-1};
  auto sender = stdexec::starts_on(
      io_sched, AsyncAcceptSender{&io.ctx, listen_fd} |
                    stdexec::then([&](int fd) { accepted_fd = fd; }));
  stdexec::sync_wait(std::move(sender));

  EXPECT_GE(accepted_fd.load(), 0);

  int nodelay = 0;
  socklen_t optlen = sizeof(nodelay);
  ASSERT_EQ(::getsockopt(accepted_fd.load(), IPPROTO_TCP, TCP_NODELAY, &nodelay,
                         &optlen),
            0);
  EXPECT_EQ(nodelay, 1);

  close(accepted_fd.load());
  close(connect_fd);
  close(listen_fd);
}

}  // namespace
}  // namespace miniredis
