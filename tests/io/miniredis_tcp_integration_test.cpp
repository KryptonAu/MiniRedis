#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>

#include "commands/registry.h"
#include "core/config.h"
#include "core/server.h"
#include "io/client_session.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"
#include "io/socket_util.h"

namespace miniredis {
namespace {

// -- Test helpers ----------------------------------------------------------

static bool WaitReadable(int fd, int timeout_ms) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  return ::poll(&pfd, 1, timeout_ms) > 0;
}

static std::string ReadLine(int fd) {
  std::string line;
  char c;
  while (::read(fd, &c, 1) == 1) {
    line += c;
    if (line.size() >= 2 && line[line.size() - 2] == '\r' &&
        line.back() == '\n')
      break;
  }
  return line;
}

static std::string ReadResp(int fd) {
  std::string first = ReadLine(fd);
  if (first.empty()) return "";

  char type = first[0];
  if (type == '+' || type == '-' || type == ':') return first;

  if (type == '$') {
    int len = std::stoi(first.substr(1));
    if (len == -1) return "$-1\r\n";
    std::string result = first;
    std::array<char, 65536> buf{};
    size_t to_read = static_cast<size_t>(len) + 2;
    while (result.size() < first.size() + to_read) {
      ssize_t n =
          ::read(fd, buf.data(), to_read - (result.size() - first.size()));
      if (n <= 0) break;
      result.append(buf.data(), static_cast<size_t>(n));
    }
    return result;
  }

  if (type == '*') {
    int count = std::stoi(first.substr(1));
    if (count == -1) return "*-1\r\n";
    std::string result = first;
    for (int i = 0; i < count; i++) {
      result += ReadResp(fd);
    }
    return result;
  }

  return first;
}

static int ConnectToServer(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }

  // Wait for the connection to complete (writable).
  if (ret < 0) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int pr = ::poll(&pfd, 1, 5000);  // 5s timeout
    if (pr <= 0) {
      ::close(fd);
      return -1;
    }
  }

  return fd;
}

// -- Server harness --------------------------------------------------------

struct TestServer {
  EpollContext io_ctx;
  CmdContext cmd_ctx;
  exec::async_scope scope;
  int listen_fd = -1;
  int port = 0;
  std::thread io_thread;
  std::thread cmd_thread;
  std::unique_ptr<CommandRegistry> registry;
  bool stopped = false;

  ~TestServer() { Stop(); }

  bool Start() {
    Server& server = Server::Instance();
    MiniRedisConfig config;
    config.databases = 4;
    config.port = 0;  // kernel picks
    if (!server.Init(config)) return false;

    registry =
        std::make_unique<CommandRegistry>(CreateDefaultCommandRegistry());

    listen_fd =
        CreateListenSocket(config.bind, config.port, config.tcp_backlog);
    if (listen_fd < 0) return false;

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) <
        0)
      return false;
    port = ntohs(addr.sin_port);

    auto io_sched = io_ctx.get_scheduler();
    auto cmd_sched = cmd_ctx.get_scheduler();

    cmd_thread = std::thread([&] { cmd_ctx.Run(); });

    io_thread = std::thread([&, io_sched, cmd_sched] {
      scope.spawn(stdexec::starts_on(
          io_sched, accept_loop(scope, io_sched, cmd_sched, server, *registry,
                                listen_fd)));
      io_ctx.Run();
    });

    // Give the server a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
  }

  void Stop() {
    if (stopped) return;
    stopped = true;
    io_ctx.Stop();
    stdexec::sync_wait(scope.on_empty());
    cmd_ctx.Stop();
    if (io_thread.joinable()) io_thread.join();
    if (cmd_thread.joinable()) cmd_thread.join();
    if (listen_fd >= 0) {
      ::close(listen_fd);
      listen_fd = -1;
    }
    Server::Instance().Shutdown();
  }
};

// =========================================================================
// Integration tests
// =========================================================================

TEST(TcpIntegrationTest, PingReturnsPong) {
  TestServer server;
  ASSERT_TRUE(server.Start());

  int fd = ConnectToServer(server.port);
  ASSERT_GE(fd, 0);
  auto cleanup = [&] { ::close(fd); };

  std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::write(fd, ping.data(), ping.size()),
            static_cast<ssize_t>(ping.size()));

  ASSERT_TRUE(WaitReadable(fd, 5000)) << "no reply from server";
  std::string reply = ReadResp(fd);
  EXPECT_EQ(reply, "+PONG\r\n");

  cleanup();
  server.Stop();
}

TEST(TcpIntegrationTest, SetGetRoundTrip) {
  TestServer server;
  ASSERT_TRUE(server.Start());

  int fd = ConnectToServer(server.port);
  ASSERT_GE(fd, 0);
  auto cleanup = [&] { ::close(fd); };

  std::string set_cmd = "*3\r\n$3\r\nSET\r\n$4\r\nakey\r\n$5\r\nhello\r\n";
  ASSERT_EQ(::write(fd, set_cmd.data(), set_cmd.size()),
            static_cast<ssize_t>(set_cmd.size()));
  ASSERT_TRUE(WaitReadable(fd, 5000)) << "no SET reply";
  std::string set_reply = ReadResp(fd);
  EXPECT_EQ(set_reply, "+OK\r\n");

  std::string get_cmd = "*2\r\n$3\r\nGET\r\n$4\r\nakey\r\n";
  ASSERT_EQ(::write(fd, get_cmd.data(), get_cmd.size()),
            static_cast<ssize_t>(get_cmd.size()));
  ASSERT_TRUE(WaitReadable(fd, 5000)) << "no GET reply";
  std::string get_reply = ReadResp(fd);
  EXPECT_EQ(get_reply, "$5\r\nhello\r\n");

  cleanup();
  server.Stop();
}

TEST(TcpIntegrationTest, TwoConcurrentClients) {
  TestServer server;
  ASSERT_TRUE(server.Start());

  int fd1 = ConnectToServer(server.port);
  ASSERT_GE(fd1, 0);
  int fd2 = ConnectToServer(server.port);
  ASSERT_GE(fd2, 0);
  auto cleanup = [&] {
    ::close(fd1);
    ::close(fd2);
  };

  std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::write(fd1, ping.data(), ping.size()),
            static_cast<ssize_t>(ping.size()));
  ASSERT_EQ(::write(fd2, ping.data(), ping.size()),
            static_cast<ssize_t>(ping.size()));

  ASSERT_TRUE(WaitReadable(fd1, 5000));
  ASSERT_TRUE(WaitReadable(fd2, 5000));
  std::string reply1 = ReadResp(fd1);
  std::string reply2 = ReadResp(fd2);
  EXPECT_EQ(reply1, "+PONG\r\n");
  EXPECT_EQ(reply2, "+PONG\r\n");

  cleanup();
  server.Stop();
}

TEST(TcpIntegrationTest, ShutdownWithActiveIdleClients) {
  TestServer server;
  ASSERT_TRUE(server.Start());

  int fd = ConnectToServer(server.port);
  ASSERT_GE(fd, 0);

  // Close the client before stopping the server.
  ::close(fd);

  // Server should stop cleanly.
  server.Stop();
  // No crash = success.
}

}  // namespace
}  // namespace miniredis
