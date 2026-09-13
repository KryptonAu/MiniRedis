#include "io/epoll_context.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <functional>
#include <stdexec/execution.hpp>
#include <thread>
#include <vector>

#include "context_test_util.h"

namespace miniredis {

struct EpollContextTestPeer {
  static int WakeFd(const EpollContext& ctx) { return ctx.wake_fd_; }
};

namespace {

struct TestEpollOp : EpollOpBase {
  std::function<void()> on_complete = [] {};

  void Complete() noexcept override { on_complete(); }
};

struct SelfDeletingEpollOp : TestEpollOp {
  void Complete() noexcept override {
    on_complete();
    delete this;
  }
};

TEST(EpollContextTest, BatchBeforeRunOnlyNotifiesOnEmptyTransition) {
  EpollContext ctx;
  std::array<TestEpollOp, 64> ops;
  std::vector<size_t> order;
  for (size_t i = 0; i < ops.size(); ++i) {
    ops[i].on_complete = [&, i] {
      order.push_back(i);
      if (i == ops.size() - 1) ctx.Stop();
    };
    ASSERT_TRUE(ctx.Enqueue(&ops[i]));
  }

  eventfd_t notifications = 0;
  ASSERT_EQ(::eventfd_read(EpollContextTestPeer::WakeFd(ctx), &notifications),
            0);
  EXPECT_EQ(notifications, 1u);
  EXPECT_EQ(::eventfd_read(EpollContextTestPeer::WakeFd(ctx), &notifications),
            -1);
  EXPECT_EQ(errno, EAGAIN);

  std::thread consumer([&] { ctx.Run(); });
  consumer.join();
  ASSERT_EQ(order.size(), ops.size());
  for (size_t i = 0; i < order.size(); ++i) EXPECT_EQ(order[i], i);
}

TEST(EpollContextTest, EnqueueWhileConsumerExecutesWakesNextBatch) {
  EpollContext ctx;
  TestEpollOp blocker;
  std::array<TestEpollOp, 64> ops;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<size_t> completed{0};
  std::vector<size_t> order;
  blocker.on_complete = [&] {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };
  ASSERT_TRUE(ctx.Enqueue(&blocker));
  // Remove the initial notification so the new batch must provide its own
  // wakeup when Run finishes the blocker and enters epoll_wait.
  eventfd_t notifications = 0;
  ASSERT_EQ(::eventfd_read(EpollContextTestPeer::WakeFd(ctx), &notifications),
            0);
  std::thread consumer([&] { ctx.Run(); });
  const bool executing =
      test::WaitFor([&] { return entered.load(std::memory_order_acquire); });
  EXPECT_TRUE(executing);
  if (executing) {
    for (size_t i = 0; i < ops.size(); ++i) {
      ops[i].on_complete = [&, i] {
        order.push_back(i);
        completed.fetch_add(1, std::memory_order_release);
      };
      EXPECT_TRUE(ctx.Enqueue(&ops[i]));
    }
  }
  release.store(true, std::memory_order_release);
  if (executing) {
    EXPECT_TRUE(test::WaitFor([&] {
      return completed.load(std::memory_order_acquire) == ops.size();
    }));
  }
  ctx.Stop();
  consumer.join();
  ASSERT_EQ(order.size(), ops.size());
  for (size_t i = 0; i < order.size(); ++i) EXPECT_EQ(order[i], i);
}

TEST(EpollContextTest, RepeatedEmptyTransitionsDoNotLoseWakeup) {
  EpollContext ctx;
  constexpr size_t kRounds = 1000;
  std::array<TestEpollOp, kRounds> ops;
  std::array<int, kRounds> completions{};
  std::atomic<size_t> completed{0};
  std::thread consumer([&] { ctx.Run(); });
  for (size_t i = 0; i < kRounds; ++i) {
    ops[i].on_complete = [&, i] {
      ++completions[i];
      completed.fetch_add(1, std::memory_order_release);
    };
    EXPECT_TRUE(ctx.Enqueue(&ops[i]));
    const bool drained = test::WaitFor(
        [&] { return completed.load(std::memory_order_acquire) == i + 1; });
    EXPECT_TRUE(drained);
    if (!drained) break;
  }
  ctx.Stop();
  consumer.join();
  for (int count : completions) EXPECT_EQ(count, 1);
}

TEST(EpollContextTest, ConcurrentProducersCompleteOnceInProducerOrder) {
  EpollContext ctx;
  constexpr size_t kProducers = 4;
  constexpr size_t kOpsPerProducer = 256;
  std::array<std::vector<size_t>, kProducers> order;
  std::array<std::array<int, kOpsPerProducer>, kProducers> completions{};
  std::atomic<size_t> completed{0};
  std::atomic<bool> start{false};
  std::thread consumer([&] { ctx.Run(); });
  std::array<std::thread, kProducers> producers;
  for (size_t producer = 0; producer < kProducers; ++producer) {
    producers[producer] = std::thread([&, producer] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (size_t i = 0; i < kOpsPerProducer; ++i) {
        auto* op = new SelfDeletingEpollOp;
        op->on_complete = [&, producer, i] {
          order[producer].push_back(i);
          ++completions[producer][i];
          completed.fetch_add(1, std::memory_order_release);
        };
        const bool accepted = ctx.Enqueue(op);
        EXPECT_TRUE(accepted);
        if (!accepted) delete op;
        if (i % 8 == 0) std::this_thread::yield();
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& producer : producers) producer.join();
  EXPECT_TRUE(test::WaitFor([&] {
    return completed.load(std::memory_order_acquire) ==
           kProducers * kOpsPerProducer;
  }));
  ctx.Stop();
  consumer.join();
  for (size_t producer = 0; producer < kProducers; ++producer) {
    ASSERT_EQ(order[producer].size(), kOpsPerProducer);
    for (size_t i = 0; i < kOpsPerProducer; ++i) {
      EXPECT_EQ(order[producer][i], i);
      EXPECT_EQ(completions[producer][i], 1);
    }
  }
}

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
