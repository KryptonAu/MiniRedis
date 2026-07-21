#include "io/client_session.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>
#include <utility>

#include "commands/registry.h"
#include "core/server.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"

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

template <typename Run>
static void WithClientSession(Run&& run, bool appendonly = false,
                              CommandContext::PropagateFn propagate = {},
                              CommandContext::ApplyConfigFn apply_config = {}) {
  auto [server_fd, client_fd] = MakeSocketPair();

  Server& server = Server::Instance();
  MiniRedisConfig config;
  config.databases = 4;
  config.appendonly = appendonly;
  server.Init(config);

  auto registry = CreateDefaultCommandRegistry();

  EpollContext io_ctx;
  CmdContext cmd_ctx;
  auto io_sched = io_ctx.get_scheduler();
  auto cmd_sched = cmd_ctx.get_scheduler();

  exec::async_scope scope;

  std::thread cmd_thread([&] { cmd_ctx.Run(); });

  std::thread io_thread([&] {
    scope.spawn(stdexec::starts_on(
        io_sched,
        handle_client(io_sched, cmd_sched, server_fd, server, registry,
                      std::move(propagate), std::move(apply_config))));
    io_ctx.Run();
  });

  auto cleanup = ScopeExit([&] {
    if (client_fd >= 0) {
      ::close(client_fd);
      client_fd = -1;
    }
    io_ctx.Stop();
    stdexec::sync_wait(scope.on_empty());
    cmd_ctx.Stop();
    if (io_thread.joinable()) io_thread.join();
    if (cmd_thread.joinable()) cmd_thread.join();
    server.Shutdown();
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::forward<Run>(run)(client_fd);
}

// =========================================================================
// Client session tests
// =========================================================================

TEST(ClientSessionTest, PingRoundTrip) {
  WithClientSession([](int client_fd) {
    std::string ping = "*1\r\n$4\r\nPING\r\n";
    ASSERT_EQ(::write(client_fd, ping.data(), ping.size()),
              static_cast<ssize_t>(ping.size()));

    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    std::string reply = ReadResp(client_fd);
    EXPECT_EQ(reply, "+PONG\r\n");
  });
}

TEST(ClientSessionTest, SetGetRoundTrip) {
  WithClientSession([](int client_fd) {
    std::string set_cmd = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n";
    ASSERT_EQ(::write(client_fd, set_cmd.data(), set_cmd.size()),
              static_cast<ssize_t>(set_cmd.size()));
    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    std::string set_reply = ReadResp(client_fd);
    EXPECT_EQ(set_reply, "+OK\r\n");

    std::string get_cmd = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
    ASSERT_EQ(::write(client_fd, get_cmd.data(), get_cmd.size()),
              static_cast<ssize_t>(get_cmd.size()));
    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    std::string get_reply = ReadResp(client_fd);
    EXPECT_EQ(get_reply, "$5\r\nhello\r\n");
  });
}

TEST(ClientSessionTest, FragmentedCommandRoundTrip) {
  WithClientSession([](int client_fd) {
    std::string part1 = "*1\r\n$4\r\nPI";
    std::string part2 = "NG\r\n";
    ASSERT_EQ(::write(client_fd, part1.data(), part1.size()),
              static_cast<ssize_t>(part1.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(::write(client_fd, part2.data(), part2.size()),
              static_cast<ssize_t>(part2.size()));

    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    EXPECT_EQ(ReadResp(client_fd), "+PONG\r\n");
  });
}

TEST(ClientSessionTest, PipelinedSetGetRoundTrip) {
  WithClientSession([](int client_fd) {
    std::string set_cmd =
        "*3\r\n$3\r\nSET\r\n$8\r\npipe-key\r\n$5\r\nvalue\r\n";
    std::string get_cmd = "*2\r\n$3\r\nGET\r\n$8\r\npipe-key\r\n";
    std::string pipeline = set_cmd + get_cmd;

    ASSERT_EQ(::write(client_fd, pipeline.data(), pipeline.size()),
              static_cast<ssize_t>(pipeline.size()));

    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    EXPECT_EQ(ReadResp(client_fd), "+OK\r\n");
    ASSERT_TRUE(WaitReadable(client_fd, 5000));
    EXPECT_EQ(ReadResp(client_fd), "$5\r\nvalue\r\n");
  });
}

TEST(ClientSessionTest, RuntimeCallbacksAreInvokedDuringCommandDispatch) {
  std::atomic<int> propagated = 0;
  std::atomic<int> applied_config = 0;

  CommandContext::PropagateFn propagate =
      [&](int db_index, const std::vector<std::string>& args) {
        if (db_index == 0 && args.size() == 3 && args[0] == "SET" &&
            args[1] == "callback-key" && args[2] == "value") {
          propagated.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
      };
  CommandContext::ApplyConfigFn apply_config = [&](std::string_view key,
                                                   std::string_view value) {
    if (key == "hz" && value == "20") {
      applied_config.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  };

  WithClientSession(
      [](int client_fd) {
        std::string set_cmd =
            "*3\r\n$3\r\nSET\r\n$12\r\ncallback-key\r\n$5\r\nvalue\r\n";
        ASSERT_EQ(::write(client_fd, set_cmd.data(), set_cmd.size()),
                  static_cast<ssize_t>(set_cmd.size()));
        ASSERT_TRUE(WaitReadable(client_fd, 5000));
        EXPECT_EQ(ReadResp(client_fd), "+OK\r\n");

        std::string config_cmd =
            "*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$2\r\nhz\r\n$2\r\n20\r\n";
        ASSERT_EQ(::write(client_fd, config_cmd.data(), config_cmd.size()),
                  static_cast<ssize_t>(config_cmd.size()));
        ASSERT_TRUE(WaitReadable(client_fd, 5000));
        EXPECT_EQ(ReadResp(client_fd), "+OK\r\n");
      },
      /*appendonly=*/true, std::move(propagate), std::move(apply_config));

  EXPECT_EQ(propagated.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(applied_config.load(std::memory_order_relaxed), 1);
}

}  // namespace
}  // namespace miniredis
