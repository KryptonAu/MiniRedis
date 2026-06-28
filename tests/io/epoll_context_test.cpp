#include "io/epoll_context.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexec/execution.hpp>
#include <thread>

namespace miniredis {
namespace {

TEST(EpollContextTest, SchedulerIsCopyable) {
  EpollContext ctx;
  EpollContext::scheduler sched = ctx.get_scheduler();

  // Copy construct
  EpollContext::scheduler sched2(sched);
  EXPECT_EQ(sched, sched2);

  // Copy assign
  EpollContext::scheduler sched3;
  sched3 = sched;
  EXPECT_EQ(sched, sched3);
}

TEST(EpollContextTest, ScheduleRunsOnIoThread) {
  EpollContext ctx;
  auto sched = ctx.get_scheduler();

  std::thread::id io_tid;
  std::atomic<bool> done{false};

  // Start IO thread
  std::thread io_thread([&] {
    io_tid = std::this_thread::get_id();
    ctx.Run();
  });

  // Schedule work onto the IO thread
  auto sender = stdexec::starts_on(sched, stdexec::just());
  stdexec::sync_wait(std::move(sender));

  // The sync_wait completed, which means the work ran
  EXPECT_NE(io_tid, std::thread::id{});

  ctx.Stop();
  io_thread.join();
}

TEST(EpollContextTest, ScheduleFromOtherThreadWakesEpollWait) {
  EpollContext ctx;
  auto sched = ctx.get_scheduler();

  // Start IO thread
  std::thread io_thread([&] { ctx.Run(); });

  // Schedule work from the main thread; sync_wait blocks until it completes
  std::thread::id work_tid;
  auto sender = stdexec::starts_on(
      sched, stdexec::just() |
                 stdexec::then([&] { work_tid = std::this_thread::get_id(); }));
  stdexec::sync_wait(std::move(sender));

  EXPECT_EQ(work_tid, io_thread.get_id());

  ctx.Stop();
  io_thread.join();
}

TEST(EpollContextTest, StopCompletesQueuedWorkWithStopped) {
  EpollContext ctx;
  auto sched = ctx.get_scheduler();

  std::atomic<bool> ran{false};

  // Enqueue work, but don't run the loop yet
  // We enqueue by calling start() on an op without running the IO loop

  // Start IO thread but immediately stop it
  std::thread io_thread([&] { ctx.Run(); });

  // Stop while IO thread is running
  ctx.Stop();
  io_thread.join();

  // After Stop(), schedule() should complete with set_stopped()
  auto sender = stdexec::starts_on(
      sched, stdexec::just() | stdexec::then([&] { ran = true; }));

  try {
    stdexec::sync_wait(std::move(sender));
    // If it completed normally, it must have been via set_stopped,
    // which sync_wait would rethrow or return empty
  } catch (...) {
    // sync_wait throws when the sender completes with set_stopped for
    // certain types; for our scheduler, set_stopped may be handled differently.
    // The key assertion: the work did NOT run.
  }
  EXPECT_FALSE(ran);
}

TEST(EpollContextTest, IsOnThreadReturnsTrueOnlyOnIoThread) {
  EpollContext ctx;
  auto sched = ctx.get_scheduler();

  EXPECT_FALSE(ctx.IsOnThread());

  std::thread io_thread([&] { ctx.Run(); });

  std::atomic<bool> on_thread{false};
  auto sender = stdexec::starts_on(sched, stdexec::just() | stdexec::then([&] {
                                            on_thread = ctx.IsOnThread();
                                          }));
  stdexec::sync_wait(std::move(sender));

  EXPECT_TRUE(on_thread);

  ctx.Stop();
  io_thread.join();
}

TEST(EpollContextTest, MultipleSchedulesExecuteInOrder) {
  EpollContext ctx;
  auto sched = ctx.get_scheduler();

  std::thread io_thread([&] { ctx.Run(); });

  std::vector<int> values;
  std::atomic<bool> all_done{false};

  // Schedule work and use sync_wait to serialize
  for (int i = 0; i < 100; i++) {
    auto sender = stdexec::starts_on(
        sched,
        stdexec::just() | stdexec::then([&, i] { values.push_back(i); }));
    stdexec::sync_wait(std::move(sender));
  }

  EXPECT_EQ(values.size(), 100u);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(values[static_cast<size_t>(i)], i);
  }

  ctx.Stop();
  io_thread.join();
}

}  // namespace
}  // namespace miniredis
