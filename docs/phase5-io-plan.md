# MiniRedis Phase 5: TCP I/O Integration with stdexec

> Status: Revised Implementation Spec | Date: 2026-06-28

## Context

Phase 5 turns MiniRedis from an in-process command/runtime library into a TCP
server. The target architecture is still the two-thread model described by the
original design notes:

```text
IO thread                               CMD thread
--------------------------------        -------------------------------
epoll_wait + coroutine resume           command dispatch
async_accept / async_read / async_write Database / Value operations
RESP request parsing                    expiry and command-side state
client connection lifecycle             single-threaded data mutation
```

Each client connection is handled by one coroutine. Socket I/O remains on the
IO thread, while command execution runs on a single CMD thread. The implementation
must preserve the current Phase 3/4 contracts:

- `RespParser` already supports partial reads and pipelined RESP arrays.
- `Server::CreateClient()` can reject duplicate fds and return `nullptr`.
- `CommandRegistry` and `ExecuteCommand()` live in `src/commands`; `miniredis_core`
  must not depend on `miniredis_commands`.
- Current command registration covers 87 commands in this checkout.

## Key Corrections from the Initial Draft

The previous draft was directionally useful, but it hid several implementation
hazards. This revised spec makes those contracts explicit before coding.

1. A stdexec scheduler must be a cheap, copyable handle. The owning event-loop
   objects must be separate from the scheduler handle.
2. Do not use `start_detached` for client sessions. It does not provide shutdown
   tracking or cancellation. Use `exec::async_scope` and join it during shutdown.
3. Async I/O operations need typed lifetime and cancellation ownership. `void*`
   fd slots are not acceptable.
4. `miniredis_core` must not include command-dispatch code. Put the TCP/I/O
   layer in a new library target that depends on both core and commands.
5. Integration tests must read exactly one RESP reply frame, not block waiting
   for EOF on Redis-style persistent connections.

## stdexec Usage

Use the stdexec checkout already fetched by CMake. Include:

```cpp
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/async_scope.hpp>
```

The current stdexec checkout exposes task/scope facilities through the
`experimental::execution` namespace alias `exec`; examples may also refer to
`stdexec::task`. Pick one style locally and use it consistently. This plan uses
`exec::task` and `exec::async_scope` for coroutine/task ownership, and
`stdexec::*` for sender algorithms such as `starts_on`, `then`, and `sync_wait`.

## File and Target Layout

Create a new I/O layer instead of expanding `miniredis_core` into commands:

```text
src/io/
  epoll_context.h
  epoll_context.cpp
  cmd_context.h
  cmd_context.cpp
  async_io.h
  client_session.h
  socket_util.h
  socket_util.cpp
  signal_util.h

tests/io/
  epoll_context_test.cpp
  cmd_context_test.cpp
  async_io_test.cpp
  client_session_test.cpp
  miniredis_tcp_integration_test.cpp
```

CMake targets:

```cmake
add_library(miniredis_io STATIC
    src/io/epoll_context.cpp
    src/io/cmd_context.cpp
    src/io/socket_util.cpp
)
target_include_directories(miniredis_io PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis_io PUBLIC
    miniredis_commands
    miniredis_core
    stdexec
    pthread
)

target_link_libraries(miniredis PRIVATE miniredis_io)
```

Rationale: `client_session.h` calls `ExecuteCommand(registry, ctx, args)`, so it
belongs above both `miniredis_core` and `miniredis_commands`. Do not add a
`CommandRegistry*` field to `Server`.

## Step 5.1: EpollContext and Scheduler Handle

**Goal**: implement an owning epoll event-loop context plus a small scheduler
handle whose `schedule()` sender resumes work on the IO thread.

### Required Shape

```cpp
class EpollContext {
 public:
  class scheduler {
   public:
    using scheduler_concept = stdexec::scheduler_tag;

    scheduler() = default;
    explicit scheduler(EpollContext* ctx) noexcept : ctx_(ctx) {}

    auto operator==(const scheduler&) const noexcept -> bool = default;
    auto schedule() const noexcept -> EpollScheduleSender;

   private:
    EpollContext* ctx_ = nullptr;
  };

  EpollContext();
  ~EpollContext();

  EpollContext(const EpollContext&) = delete;
  EpollContext& operator=(const EpollContext&) = delete;

  scheduler get_scheduler() noexcept { return scheduler{this}; }

  void Run();
  void Stop();
  bool IsOnThread() const noexcept;

  void Enqueue(EpollOpBase* op) noexcept;
  void ArmIo(EpollIoOpBase* op) noexcept;      // executed on IO thread
  void CancelFd(int fd) noexcept;              // executed on IO thread

 private:
  int epoll_fd_ = -1;
  int wake_fd_ = -1;
  std::atomic<bool> stopping_{false};
  std::thread::id io_thread_id_{};
};
```

`EpollContext` owns file descriptors, queues, and fd state. Its nested
`scheduler` is the stdexec scheduler and must remain copyable and nothrow
movable.

### Schedule Sender Contract

- `schedule()` returns a sender with `set_value_t()` and `set_stopped_t()`.
- `start()` enqueues its operation into `EpollContext`.
- If the context is already stopping, the operation completes with
  `set_stopped()`.
- Otherwise the IO thread calls `set_value()` exactly once.
- The sender environment reports the completion scheduler and default domain.

### Wake Queue

Use an intrusive MPSC queue for scheduled operations. `Enqueue()` may be called
from the CMD thread, tests, or the main thread.

Wake-up uses `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`. Write and drain `uint64_t`
values, not single bytes:

```cpp
uint64_t one = 1;
while (write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {}
```

The IO loop drains the wake fd until `EAGAIN`.

### Run Loop

The loop processes ready schedule operations before and after `epoll_wait()`.
It must handle:

- `EINTR`: retry.
- `EPOLLERR`, `EPOLLHUP`, `EPOLLRDHUP`: complete affected I/O operations with a
  socket error or EOF as appropriate.
- `Stop()`: set `stopping_`, wake the loop, complete queued schedule operations
  with `set_stopped()`, and complete all pending I/O operations with
  `set_stopped()`.

## Step 5.2: Async Read and Write Senders

**Goal**: implement epoll-backed senders that can be awaited from `exec::task`.

### Read Result

Do not encode EOF as an empty string. Use an explicit result:

```cpp
struct AsyncReadResult {
  std::string data;
  bool eof = false;
};
```

`AsyncReadSender` completion signatures:

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(AsyncReadResult),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()>
```

Behavior:

- `read()` returning `0` completes with `{.data = "", .eof = true}`.
- `read()` returning `> 0` completes with `{.data = bytes, .eof = false}`.
- `EAGAIN`/`EWOULDBLOCK` means remain armed.
- Other syscall failures complete with
  `std::make_exception_ptr(std::system_error(errno, std::generic_category()))`.

### Write Sender

`AsyncWriteSender` owns the bytes to send. Use `send(..., MSG_NOSIGNAL)` instead
of plain `write()` so a disconnected client cannot terminate the process with
`SIGPIPE`.

Completion signatures:

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(size_t),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()>
```

Behavior:

- Attempt to send immediately when armed on the IO thread.
- Track `offset_`.
- If all bytes are sent, complete with total bytes.
- If `EAGAIN`, register `EPOLLOUT` and retry on readiness.
- If `ECONNRESET`, `EPIPE`, or another socket error occurs, complete with
  `set_error(exception_ptr)`.

### Fd State Contract

Use typed base classes. Do not store raw `void*` operation slots.

```cpp
struct EpollIoOpBase {
  int fd = -1;
  virtual void OnReady(uint32_t events) noexcept = 0;
  virtual void OnStopped() noexcept = 0;
  virtual uint32_t Events() const noexcept = 0;

 protected:
  ~EpollIoOpBase() = default;
};

struct FdState {
  int fd = -1;
  EpollIoOpBase* read_op = nullptr;
  EpollIoOpBase* write_op = nullptr;
  EpollIoOpBase* accept_op = nullptr;
  uint32_t armed_events = 0;
};
```

All fd-state mutation happens on the IO thread. If an operation is started from
another thread, it must first enqueue an IO-thread action. For Phase 5, each fd
allows at most one pending read and one pending write. A duplicate pending op is
a programmer error and should complete with `set_error(logic_error)`.

Use level-triggered epoll initially:

- Register `EPOLLIN` for pending read/accept.
- Register `EPOLLOUT` for pending write.
- Always include `EPOLLERR | EPOLLHUP | EPOLLRDHUP`.
- Recompute the fd event mask with `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, or
  `EPOLL_CTL_DEL` whenever an op starts or completes.

## Step 5.3: Async Accept Sender

**Goal**: accept TCP clients on the IO thread.

`AsyncAcceptSender` completion signatures:

```cpp
stdexec::completion_signatures<
    stdexec::set_value_t(int),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()>
```

Behavior:

- Arm `listen_fd` for `EPOLLIN`.
- On readiness, call `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`.
- On success, set `TCP_NODELAY` on the accepted fd and complete with that fd.
- On `EAGAIN`/`EWOULDBLOCK`, remain armed.
- On `EMFILE`/`ENFILE`, complete with a system error; the accept loop should log
  and retry after a small delay or on the next loop iteration.
- On shutdown, complete pending accept with `set_stopped()`.

## Step 5.4: CmdContext and Scheduler Handle

**Goal**: implement the single CMD thread scheduler.

Like `EpollContext`, split the owner from the scheduler handle:

```cpp
class CmdContext {
 public:
  class scheduler {
   public:
    using scheduler_concept = stdexec::scheduler_tag;
    scheduler() = default;
    explicit scheduler(CmdContext* ctx) noexcept : ctx_(ctx) {}
    auto operator==(const scheduler&) const noexcept -> bool = default;
    auto schedule() const noexcept -> CmdScheduleSender;

   private:
    CmdContext* ctx_ = nullptr;
  };

  scheduler get_scheduler() noexcept { return scheduler{this}; }
  void Run();
  void Stop();
  void Enqueue(CmdOpBase* op) noexcept;
  bool IsOnThread() const noexcept;

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<CmdOpBase*> queue_;
  std::atomic<bool> stopping_{false};
  std::thread::id cmd_thread_id_{};
};
```

Contracts:

- `Run()` executes queued operations on the CMD thread without holding the mutex.
- Operations already dequeued may finish normally.
- Operations still queued after `Stop()` complete with `set_stopped()`.
- New enqueues after `Stop()` complete with `set_stopped()` and do not block.

## Step 5.5: Client Session and Accept Loop

**Goal**: one coroutine per client, with explicit thread ownership.

### Thread Ownership

- IO thread owns socket fds, RESP parsing, reply buffering, and `Server::clients_`
  insertion/removal.
- CMD thread owns `Database`, `Value`, command handlers, config reads performed
  by commands, expiry side effects, and command-driven `Client` state such as
  `SELECT`.
- A `Client&` may be read/mutated on the CMD thread only while its IO coroutine
  is suspended awaiting command execution. The coroutine resumes on the IO thread
  before touching parser/reply state again.
- `server.Shutdown()` runs only after IO and CMD threads have joined.

### Command Dispatch

Use `stdexec::starts_on(cmd_sched, ...)` to run the command lambda on the CMD
thread, but lock the behavior with a thread-affinity test. The session must catch
exceptions and return normally so scoped background work does not surface an
unhandled `set_error`.

Sketch:

```cpp
exec::task<void> handle_client(EpollContext::scheduler io_sched,
                               CmdContext::scheduler cmd_sched,
                               int client_fd,
                               Server& server,
                               CommandRegistry& registry) {
  Client* client = server.CreateClient(client_fd);
  if (client == nullptr) {
    close(client_fd);
    co_return;
  }

  auto cleanup = ScopeExit([&] {
    close(client_fd);
    server.RemoveClient(client_fd);
  });

  try {
    auto& parser = client->Parser();
    auto& reply_buf = client->ReplyBuffer();

    while (true) {
      AsyncReadResult read = co_await AsyncReadSender{io_sched, client_fd};
      if (read.eof) co_return;

      ParseStatus status = parser.Feed(read.data);
      if (status == ParseStatus::kError) {
        std::string err = RespReply::Error("ERR protocol error");
        (void)co_await AsyncWriteSender{io_sched, client_fd, std::move(err)};
        co_return;
      }

      while (parser.HasCommand()) {
        auto args = parser.TakeCommand();
        std::string reply = co_await stdexec::starts_on(
            cmd_sched,
            stdexec::just(std::move(args)) |
                stdexec::then([&server, client, &registry](
                                  std::vector<std::string> cmd_args) {
                  Database* db = server.GetDbFor(*client);
                  if (db == nullptr) {
                    return RespReply::Error("ERR invalid DB index");
                  }
                  CommandContext ctx{server, *client, *db};
                  return ExecuteCommand(registry, ctx, cmd_args);
                }));
        reply_buf += reply;
      }

      if (!reply_buf.empty()) {
        std::string out = std::exchange(reply_buf, {});
        (void)co_await AsyncWriteSender{io_sched, client_fd, std::move(out)};
      }
    }
  } catch (...) {
    co_return;
  }
}
```

`ScopeExit` can be a small local RAII helper in `client_session.h`; do not add a
new dependency for it.

### Accept Loop

The accept loop is also scoped:

```cpp
exec::task<void> accept_loop(exec::async_scope& scope,
                             EpollContext::scheduler io_sched,
                             CmdContext::scheduler cmd_sched,
                             Server& server,
                             CommandRegistry& registry,
                             int listen_fd) {
  while (true) {
    int client_fd = co_await AsyncAcceptSender{io_sched, listen_fd};
    scope.spawn(stdexec::starts_on(
        io_sched,
        handle_client(io_sched, cmd_sched, client_fd, server, registry)));
  }
}
```

The real implementation must catch accept errors:

- transient accept resource errors: log/rate-limit and continue;
- `set_stopped`: exit normally;
- unexpected errors: log and continue unless shutdown has started.

## Step 5.6: Main Integration and Shutdown

`main.cpp` should:

1. Load configuration.
2. Initialize `Server::Instance()`.
3. Create and register `CommandRegistry`.
4. Create the listening socket with `CreateListenSocket()`.
5. Block `SIGINT` and `SIGTERM` before worker threads are started.
6. Create `EpollContext`, `CmdContext`, and `exec::async_scope`.
7. Start IO and CMD threads.
8. Spawn `accept_loop` into the scope.
9. Wait for shutdown via `sigwait`.
10. Stop accepting and stop contexts.
11. Join IO/CMD threads.
12. Wait for `scope.on_empty()` if any scoped operations still need explicit
    completion handling.
13. Close the listening socket.
14. Call `server.Shutdown()`.

Shutdown invariant:

- `Stop()` on the IO context completes pending accept/read/write operations with
  `set_stopped()`.
- Client coroutines observe stop, run cleanup, close fds, and remove clients.
- CMD context drains or stops queued command operations.
- `Server`, `CommandRegistry`, contexts, and schedulers outlive all tasks that
  reference them.

Do not claim that `start_detached` or `exec::task` destructors automatically
drain all sessions; the implementation must prove this through the scope and
context stop protocol.

## Socket Utilities

`CreateListenSocket(bind, port, backlog)` should live in `src/io/socket_util.*`.

Requirements:

- `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`.
- `SO_REUSEADDR`.
- Check `inet_pton()` return value.
- Bind and listen; close fd on all failure paths.
- Support `port = 0` for integration tests and use `getsockname()` to discover
  the selected port.

## Test Plan

### Unit Tests

`tests/io/epoll_context_test.cpp`

- `schedule()` work runs on the IO thread.
- Scheduling from another thread wakes `epoll_wait`.
- `Stop()` completes not-yet-run scheduled work with `set_stopped()`.
- Scheduler handle is copyable and satisfies `stdexec::scheduler`.

`tests/io/cmd_context_test.cpp`

- Work runs on the CMD thread.
- Multiple enqueued operations preserve FIFO order.
- `Stop()` wakes `Run()` and stops queued operations.
- Cross-thread `starts_on(cmd_sched, just() | then(...))` executes on CMD.

`tests/io/async_io_test.cpp`

- `async_read` handles partial data and EOF distinctly.
- `async_write` handles large payloads and EAGAIN.
- `async_accept` returns non-blocking fds with `TCP_NODELAY`.
- `EPOLLHUP`/`EPOLLERR`/`EPOLLRDHUP` complete operations exactly once.
- Pending read/write/accept complete with `set_stopped()` on shutdown.

`tests/io/client_session_test.cpp`

- PING round trip.
- SET/GET round trip on one persistent connection.
- Pipelined commands return concatenated RESP replies in order.
- Protocol error writes an error reply and closes the session.
- Duplicate fd rejection closes the new fd and does not corrupt the existing
  client entry.

### Integration Tests

`tests/io/miniredis_tcp_integration_test.cpp`

Use real loopback TCP sockets. The client helper must read exactly one RESP
reply frame (or a known number of frames for pipelining). It must not block until
EOF because Redis connections are persistent.

Minimum cases:

- `PING` returns `+PONG\r\n`.
- `SET x hello` then `GET x` returns the stored bulk string.
- Two concurrent clients can issue read commands.
- A client can close immediately after sending a command without crashing the
  server.
- Shutdown with active idle clients joins cleanly.

## RESP Reply Reader for Tests

Implement a small test helper that reads one complete RESP value:

- `+`, `-`, `:`: read through CRLF.
- `$`: read length line, then payload plus trailing CRLF; support `$-1`.
- `*`: read array length, then recursively read that many values; support nested
  arrays because command responses may contain arrays.

This helper prevents the integration tests from hanging on long-lived
connections.

## Success Criteria

- [ ] `EpollContext::scheduler` and `CmdContext::scheduler` satisfy
      `stdexec::scheduler`.
- [ ] The owning context classes are not treated as schedulers.
- [ ] All async I/O operations have typed opstate ownership and exactly-once
      completion.
- [ ] Shutdown completes pending accept/read/write/schedule operations with
      `set_stopped()`.
- [ ] Client sessions are tracked by `exec::async_scope`; no `start_detached`
      is used for long-lived sessions.
- [ ] `miniredis_core` does not depend on `miniredis_commands`.
- [ ] `main.cpp` runs a TCP server that accepts `redis-cli -p <port>` for basic
      registered commands such as PING, SET, and GET.
- [ ] Integration tests read exact RESP frames and do not wait for EOF.
- [ ] `cmake --build build` succeeds.
- [ ] `ctest --test-dir build --output-on-failure` succeeds.
- [ ] `git diff --check -- docs/phase5-io-plan.md CMakeLists.txt`
      `tests/CMakeLists.txt src/io tests/io src/main.cpp` succeeds.
- [ ] Targeted format check succeeds for touched C++ files:
      `clang-format --dry-run --Werror -style=Google src/io/*.h`
      `src/io/*.cpp tests/io/*.cpp src/main.cpp`.

## Future Work

- Edge-triggered epoll after the level-triggered implementation is correct.
- Active expiry cron scheduled onto the CMD thread.
- Connection-level command batching before switching to CMD.
- Zero-copy or `writev`-based replies.
- `io_uring` backend behind the same sender API.
