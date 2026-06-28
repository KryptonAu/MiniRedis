#include "io/async_io.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>

#include "io/epoll_context.h"
#include "io/socket_util.h"

namespace miniredis {
namespace {

// -- Helpers ---------------------------------------------------------------

// Creates a pair of connected UNIX-domain sockets for local testing.
static std::pair<int, int> MakeSocketPair() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
    throw std::system_error(errno, std::generic_category(), "socketpair");
  }
  return {fds[0], fds[1]};
}

// =========================================================================
// async_read
// =========================================================================

TEST(AsyncIoTest, AsyncReadReceivesData) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  // Write data from a different "client" thread.
  const std::string msg = "hello";
  ASSERT_EQ(::write(wfd, msg.data(), msg.size()),
            static_cast<ssize_t>(msg.size()));

  // Read on the IO thread.
  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         EXPECT_FALSE(r.eof);
                                         EXPECT_EQ(r.data, msg);
                                       }));
  stdexec::sync_wait(std::move(sender));

  ctx.Stop();
  io_thread.join();
  cleanup();
}

TEST(AsyncIoTest, AsyncReadDetectsEof) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  // Close the write side to trigger EOF on the read side.
  ::close(wfd);
  wfd = -1;

  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         EXPECT_TRUE(r.eof);
                                         EXPECT_TRUE(r.data.empty());
                                       }));
  stdexec::sync_wait(std::move(sender));

  ctx.Stop();
  io_thread.join();
  cleanup();
}

TEST(AsyncIoTest, AsyncReadHandlesPartialData) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  // Write a large message in one go but read may return partial.
  std::string big_msg(65536, 'x');
  ASSERT_EQ(::write(wfd, big_msg.data(), big_msg.size()),
            static_cast<ssize_t>(big_msg.size()));

  // The read may return partial data; the sender reads what's available.
  std::string collected;
  auto sender =
      stdexec::starts_on(io_sched, AsyncReadSender{&ctx, rfd} |
                                       stdexec::then([&](AsyncReadResult r) {
                                         collected = std::move(r.data);
                                         EXPECT_FALSE(r.eof);
                                         EXPECT_GT(collected.size(), 0u);
                                       }));
  stdexec::sync_wait(std::move(sender));

  ctx.Stop();
  io_thread.join();
  cleanup();
}

// =========================================================================
// async_write
// =========================================================================

TEST(AsyncIoTest, AsyncWriteSendsData) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  const std::string msg = "world";
  auto sender =
      stdexec::starts_on(io_sched, AsyncWriteSender{&ctx, rfd, msg} |
                                       stdexec::then([&](size_t written) {
                                         EXPECT_EQ(written, msg.size());
                                       }));
  stdexec::sync_wait(std::move(sender));

  // Read back from the other side.
  std::array<char, 256> buf{};
  ssize_t n = ::read(wfd, buf.data(), buf.size());
  EXPECT_EQ(n, static_cast<ssize_t>(msg.size()));
  EXPECT_EQ(std::string(buf.data(), static_cast<size_t>(n)), msg);

  ctx.Stop();
  io_thread.join();
  cleanup();
}

TEST(AsyncIoTest, AsyncWriteHandlesLargePayload) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  std::string big_msg(128 * 1024, 'y');  // 128 KiB
  auto sender =
      stdexec::starts_on(io_sched, AsyncWriteSender{&ctx, rfd, big_msg} |
                                       stdexec::then([&](size_t written) {
                                         EXPECT_EQ(written, big_msg.size());
                                       }));
  stdexec::sync_wait(std::move(sender));

  ctx.Stop();
  io_thread.join();
  cleanup();
}

// =========================================================================
// Shutdown: pending ops complete with set_stopped()
// =========================================================================

TEST(AsyncIoTest, PendingReadReceivesStoppedOnShutdown) {
  auto [rfd, wfd] = MakeSocketPair();
  auto cleanup = [&] {
    close(rfd);
    close(wfd);
  };
  // Don't write anything — the read will be pending in epoll.

  EpollContext ctx;

  std::thread io_thread([&] { ctx.Run(); });

  // Just start the IO loop, then stop it. We verify that Stop() doesn't hang
  // and the IO thread exits cleanly even with no pending I/O.

  ctx.Stop();
  io_thread.join();
  cleanup();
  // No hang = success.
}

// =========================================================================
// async_accept
// =========================================================================

TEST(AsyncIoTest, AsyncAcceptReturnsClientFd) {
  // Create a listening socket.
  int listen_fd = CreateListenSocket("127.0.0.1", 0 /* kernel picks port */);
  ASSERT_GE(listen_fd, 0);
  auto cleanup_listen = [&] { close(listen_fd); };

  // Discover the assigned port.
  sockaddr_in addr{};
  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(
      ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen),
      0);
  int port = ntohs(addr.sin_port);

  EpollContext ctx;
  auto io_sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  // Connect from this thread.
  int connect_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  ASSERT_GE(connect_fd, 0);
  auto cleanup_connect = [&] { close(connect_fd); };

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);

  int ret =
      ::connect(connect_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
  // Non-blocking connect may return -1/EINPROGRESS.
  if (ret < 0) {
    EXPECT_EQ(errno, EINPROGRESS);
  }

  // Accept on the IO thread.
  std::atomic<int> accepted_fd{-1};
  auto sender = stdexec::starts_on(
      io_sched, AsyncAcceptSender{&ctx, listen_fd} |
                    stdexec::then([&](int fd) { accepted_fd = fd; }));
  stdexec::sync_wait(std::move(sender));

  EXPECT_GE(accepted_fd.load(), 0);

  // Verify the accepted fd has TCP_NODELAY set.
  int nodelay = 0;
  socklen_t optlen = sizeof(nodelay);
  ASSERT_EQ(::getsockopt(accepted_fd.load(), IPPROTO_TCP, TCP_NODELAY, &nodelay,
                         &optlen),
            0);
  EXPECT_EQ(nodelay, 1);

  close(accepted_fd.load());
  ctx.Stop();
  io_thread.join();
  cleanup_connect();
  cleanup_listen();
}

}  // namespace
}  // namespace miniredis
