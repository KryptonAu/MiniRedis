#include "io/cmd_context.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "context_test_util.h"

namespace miniredis {

struct CmdContextTestPeer {
  static bool ConsumerWaiting(const CmdContext& ctx) {
    return ctx.consumer_waiting_.value.load(std::memory_order_acquire);
  }
};

namespace {

struct TestCmdOp : CmdOpBase {
  bool completed = false;
  bool stopped = false;

  void Complete() noexcept override { completed = true; }
  void CompleteStopped() noexcept override { stopped = true; }
};

TEST(CmdContextTest, PostFunctionRunsOnCmdThread) {
  CmdContext ctx;

  std::thread::id cmd_tid;
  std::thread cmd_thread([&] {
    cmd_tid = std::this_thread::get_id();
    ctx.Run();
  });

  std::thread::id work_tid;
  std::promise<void> done;
  ASSERT_TRUE(ctx.PostFunction([&] {
    work_tid = std::this_thread::get_id();
    done.set_value();
  }));
  done.get_future().wait();

  EXPECT_EQ(work_tid, cmd_tid);
  EXPECT_NE(work_tid, std::this_thread::get_id());

  ctx.Stop();
  cmd_thread.join();
}

TEST(CmdContextTest, MultipleOpsExecuteInFifoOrder) {
  CmdContext ctx;

  std::thread cmd_thread([&] { ctx.Run(); });

  constexpr int kOps = 50;
  std::vector<int> order;
  order.reserve(kOps);
  std::promise<void> done;
  std::atomic<int> remaining{kOps};

  // Queue every op before any of them runs, so this exercises FIFO ordering of
  // the queue rather than 50 sequential round trips.
  for (int i = 0; i < kOps; i++) {
    ASSERT_TRUE(ctx.PostFunction([&, i] {
      order.push_back(i);
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        done.set_value();
      }
    }));
  }
  done.get_future().wait();

  ASSERT_EQ(order.size(), static_cast<size_t>(kOps));
  for (int i = 0; i < kOps; i++) {
    EXPECT_EQ(order[static_cast<size_t>(i)], i);
  }

  ctx.Stop();
  cmd_thread.join();
}

TEST(CmdContextTest, SmallCapacityRunsQueuedOpsInFifoOrder) {
  CmdContext ctx(3);
  std::vector<int> order;

  EXPECT_TRUE(ctx.PostFunction([&] { order.push_back(0); }));
  EXPECT_TRUE(ctx.PostFunction([&] { order.push_back(1); }));
  EXPECT_TRUE(ctx.PostFunction([&] { order.push_back(2); }));
  EXPECT_TRUE(ctx.PostFunction([&] {
    order.push_back(3);
    ctx.Stop();
  }));
  EXPECT_FALSE(ctx.PostFunction([&] { order.push_back(4); }));

  std::thread cmd_thread([&] { ctx.Run(); });
  cmd_thread.join();

  ASSERT_EQ(order.size(), 4u);
  EXPECT_EQ(order[0], 0);
  EXPECT_EQ(order[1], 1);
  EXPECT_EQ(order[2], 2);
  EXPECT_EQ(order[3], 3);
}

TEST(CmdContextTest, EnqueueReturnsFalseWhenRingIsFull) {
  CmdContext ctx(2);
  TestCmdOp first;
  TestCmdOp second;
  TestCmdOp third;

  EXPECT_TRUE(ctx.Enqueue(&first));
  EXPECT_TRUE(ctx.Enqueue(&second));
  EXPECT_FALSE(ctx.Enqueue(&third));
  EXPECT_FALSE(third.completed);
  EXPECT_FALSE(third.stopped);
}

TEST(CmdContextTest, BatchBeforeRunDoesNotNotify) {
  CmdContext ctx(8);
  std::vector<int> order;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(ctx.PostFunction([&, i] {
      order.push_back(i);
      if (i == 7) ctx.Stop();
    }));
  }
  EXPECT_FALSE(CmdContextTestPeer::ConsumerWaiting(ctx));
  EXPECT_FALSE(ctx.PostFunction([] {}));
  EXPECT_FALSE(CmdContextTestPeer::ConsumerWaiting(ctx));

  std::thread consumer([&] { ctx.Run(); });
  consumer.join();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(CmdContextTest, EnqueueWhileConsumerExecutesDoesNotNotify) {
  CmdContext ctx(64);
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<int> completed{0};
  std::vector<int> order;
  ASSERT_TRUE(ctx.PostFunction([&] {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }));
  std::thread consumer([&] { ctx.Run(); });
  const bool executing =
      test::WaitFor([&] { return entered.load(std::memory_order_acquire); });
  EXPECT_TRUE(executing);
  if (executing) {
    EXPECT_FALSE(CmdContextTestPeer::ConsumerWaiting(ctx));
    for (int i = 0; i < 64; ++i) {
      EXPECT_TRUE(ctx.PostFunction([&, i] {
        order.push_back(i);
        completed.fetch_add(1, std::memory_order_release);
      }));
    }
    EXPECT_FALSE(CmdContextTestPeer::ConsumerWaiting(ctx));
  }
  release.store(true, std::memory_order_release);
  if (executing) {
    EXPECT_TRUE(test::WaitFor(
        [&] { return completed.load(std::memory_order_acquire) == 64; }));
  }
  ctx.Stop();
  consumer.join();
  ASSERT_EQ(order.size(), 64u);
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(order[static_cast<size_t>(i)], i);
  }
}

TEST(CmdContextTest, RepeatedEmptyTransitionsAndRingWrapDoNotLoseWakeup) {
  CmdContext ctx(4);
  std::atomic<int> completed{0};
  std::vector<int> order;
  std::thread consumer([&] { ctx.Run(); });

  constexpr int kRounds = 1000;
  constexpr int kBatchSize = 3;
  for (int round = 0; round < kRounds; ++round) {
    // Alternate a registered wait with a race against the consumer returning
    // from the previous callback and preparing to wait.
    if (round % 2 == 0) {
      const bool waiting = test::WaitFor(
          [&] { return CmdContextTestPeer::ConsumerWaiting(ctx); });
      EXPECT_TRUE(waiting);
      if (!waiting) break;
    }
    for (int i = 0; i < kBatchSize; ++i) {
      const int id = round * kBatchSize + i;
      EXPECT_TRUE(ctx.PostFunction([&, id] {
        order.push_back(id);
        completed.fetch_add(1, std::memory_order_release);
      }));
    }
    const bool drained = test::WaitFor([&] {
      return completed.load(std::memory_order_acquire) ==
             (round + 1) * kBatchSize;
    });
    EXPECT_TRUE(drained);
    if (!drained) break;
  }

  ctx.Stop();
  consumer.join();
  ASSERT_EQ(order.size(), static_cast<size_t>(kRounds * kBatchSize));
  for (size_t i = 0; i < order.size(); ++i) {
    EXPECT_EQ(order[i], static_cast<int>(i));
  }
}

TEST(CmdContextTest, StopWakesRegisteredWait) {
  CmdContext ctx;
  std::atomic<bool> finished{false};
  std::thread consumer([&] {
    ctx.Run();
    finished.store(true, std::memory_order_release);
  });
  EXPECT_TRUE(
      test::WaitFor([&] { return CmdContextTestPeer::ConsumerWaiting(ctx); }));
  ctx.Stop();
  EXPECT_TRUE(
      test::WaitFor([&] { return finished.load(std::memory_order_acquire); }));
  consumer.join();
}

TEST(CmdContextTest, StopBeforeRunDoesNotWait) {
  CmdContext ctx;
  ctx.Stop();
  std::atomic<bool> finished{false};
  std::thread consumer([&] {
    ctx.Run();
    finished.store(true, std::memory_order_release);
  });
  EXPECT_TRUE(
      test::WaitFor([&] { return finished.load(std::memory_order_acquire); }));
  consumer.join();
}

TEST(CmdContextTest, StopWhileConsumerExecutesDoesNotWait) {
  CmdContext ctx;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> finished{false};
  ASSERT_TRUE(ctx.PostFunction([&] {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }));
  std::thread consumer([&] {
    ctx.Run();
    finished.store(true, std::memory_order_release);
  });
  EXPECT_TRUE(
      test::WaitFor([&] { return entered.load(std::memory_order_acquire); }));
  EXPECT_FALSE(CmdContextTestPeer::ConsumerWaiting(ctx));
  ctx.Stop();
  release.store(true, std::memory_order_release);
  EXPECT_TRUE(
      test::WaitFor([&] { return finished.load(std::memory_order_acquire); }));
  consumer.join();
}

TEST(CmdContextTest, StopWakesRunAndStopsQueuedOps) {
  CmdContext ctx;

  std::atomic<bool> ran{false};
  std::thread cmd_thread([&] { ctx.Run(); });

  // Stop() must wake the (idle) consumer so Run() can return.
  ctx.Stop();
  cmd_thread.join();

  // After Stop(), new work is rejected instead of being executed.
  EXPECT_FALSE(ctx.PostFunction([&] { ran = true; }));
  EXPECT_FALSE(ran.load());
}

TEST(CmdContextTest, StopConcurrentWithSingleProducerDoesNotLoseWakeup) {
  for (int iteration = 0; iteration < 100; iteration++) {
    CmdContext ctx(8);
    std::atomic<bool> start{false};
    std::atomic<bool> finished{false};
    TestCmdOp ops[64];
    int accepted = 0;

    std::thread cmd_thread([&] {
      ctx.Run();
      finished.store(true, std::memory_order_release);
    });

    std::thread producer([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = 0; i < 64; i++) {
        if (!ctx.Enqueue(&ops[i])) {
          break;
        }
        ++accepted;
      }
    });

    start.store(true, std::memory_order_release);
    ctx.Stop();
    producer.join();
    cmd_thread.join();

    EXPECT_TRUE(finished.load(std::memory_order_acquire));
    for (int i = 0; i < 64; ++i) {
      EXPECT_EQ(
          static_cast<int>(ops[i].completed) + static_cast<int>(ops[i].stopped),
          i < accepted ? 1 : 0);
    }
  }
}

}  // namespace
}  // namespace miniredis
