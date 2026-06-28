#include "io/cmd_context.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexec/execution.hpp>
#include <thread>

namespace miniredis {
namespace {

TEST(CmdContextTest, ScheduleRunsOnCmdThread) {
  CmdContext ctx;
  auto sched = ctx.get_scheduler();

  std::thread::id cmd_tid;
  std::thread cmd_thread([&] {
    cmd_tid = std::this_thread::get_id();
    ctx.Run();
  });

  std::thread::id work_tid;
  auto sender = stdexec::starts_on(
      sched, stdexec::just() |
                 stdexec::then([&] { work_tid = std::this_thread::get_id(); }));
  stdexec::sync_wait(std::move(sender));

  EXPECT_EQ(work_tid, cmd_tid);

  ctx.Stop();
  cmd_thread.join();
}

TEST(CmdContextTest, MultipleOpsExecuteInFifoOrder) {
  CmdContext ctx;
  auto sched = ctx.get_scheduler();

  std::thread cmd_thread([&] { ctx.Run(); });

  std::vector<int> order;
  for (int i = 0; i < 50; i++) {
    auto sender = stdexec::starts_on(
        sched, stdexec::just() | stdexec::then([&, i] { order.push_back(i); }));
    stdexec::sync_wait(std::move(sender));
  }

  EXPECT_EQ(order.size(), 50u);
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(order[static_cast<size_t>(i)], i);
  }

  ctx.Stop();
  cmd_thread.join();
}

TEST(CmdContextTest, StopWakesRunAndStopsQueuedOps) {
  CmdContext ctx;
  auto sched = ctx.get_scheduler();

  std::atomic<bool> ran{false};
  std::thread cmd_thread([&] { ctx.Run(); });

  ctx.Stop();
  cmd_thread.join();

  // After Stop(), new work should complete with set_stopped.
  auto sender = stdexec::starts_on(
      sched, stdexec::just() | stdexec::then([&] { ran = true; }));

  try {
    stdexec::sync_wait(std::move(sender));
  } catch (...) {
    // Expected: set_stopped may throw.
  }
  EXPECT_FALSE(ran);
}

TEST(CmdContextTest, SchedulerIsCopyable) {
  CmdContext ctx;
  CmdContext::scheduler sched = ctx.get_scheduler();
  CmdContext::scheduler sched2(sched);
  EXPECT_EQ(sched, sched2);
}

}  // namespace
}  // namespace miniredis
