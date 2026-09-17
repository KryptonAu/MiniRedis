#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <functional>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "core/config.h"

namespace miniredis {

// ---------------------------------------------------------------------------
// CmdOpBase — non-template base for the CMD-task queue.
// ---------------------------------------------------------------------------
struct CmdOpBase {
  CmdOpBase* next_ = nullptr;
  virtual void Complete() noexcept = 0;
  virtual void CompleteStopped() noexcept = 0;

 protected:
  virtual ~CmdOpBase() = default;
};

class CmdContext;

// ===========================================================================
// CmdContext — owning single-threaded command execution context.
// The queue is SPSC: one producer thread schedules work for the CMD thread.
//
// Work is submitted by enqueueing an op whose operation state derives from
// CmdOpBase — either a purpose-built sender for a real workload (see
// io/cmd_batch.h) or PostFunction() for a fire-and-forget callable.
// ===========================================================================
class CmdContext {
 public:
  explicit CmdContext(
      size_t queue_capacity = MiniRedisConfig{}.command_queue_capacity);
  ~CmdContext() = default;

  CmdContext(const CmdContext&) = delete;
  CmdContext& operator=(const CmdContext&) = delete;

  void Run();
  void Stop();
  bool IsOnThread() const noexcept;

  // Returns false if the context is stopping or the bounded SPSC queue is full
  // (caller should deliver set_stopped() immediately). Single-producer only.
  bool Enqueue(CmdOpBase* op) noexcept;

  // Post a function to be executed on the CMD thread. Returns false if
  // stopping or the bounded SPSC queue is full. Single-producer only.
  bool PostFunction(std::function<void()> fn);

 private:
  friend struct CmdContextTestPeer;

  CmdOpBase* TryDequeue() noexcept;
  void WakeConsumer() noexcept;
  void FinishEnqueue() noexcept;
  static size_t NormalizeQueueCapacity(size_t queue_capacity) noexcept;

  static constexpr size_t kCacheLineSize = 64;

  // Number of pause-spins the consumer performs before parking (see Run()).
  static constexpr int kSpinBeforePark = 64;

  // CPU relaxation hint for the adaptive spin in Run().
  static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  struct alignas(kCacheLineSize) PaddedAtomicSize {
    std::atomic<size_t> value{0};
  };

  struct alignas(kCacheLineSize) PaddedAtomicBool {
    std::atomic<bool> value{false};
  };

  std::vector<CmdOpBase*> queue_;
  size_t queue_mask_ = 0;
  PaddedAtomicSize head_;
  PaddedAtomicSize tail_;
  PaddedAtomicSize active_enqueues_;
  // Only Run() sets this to true; wakeup sources exchange it to false.
  PaddedAtomicBool consumer_waiting_;
  PaddedAtomicBool stopping_;
  std::thread::id cmd_thread_id_{};
};

// ===========================================================================
// Inline implementations
// ===========================================================================
inline CmdContext::CmdContext(size_t queue_capacity)
    : queue_(NormalizeQueueCapacity(queue_capacity), nullptr),
      queue_mask_(queue_.size() - 1) {}

inline void CmdContext::Run() {
  cmd_thread_id_ = std::this_thread::get_id();

  while (true) {
    if (auto* op = TryDequeue(); op != nullptr) {
      op->Complete();
      continue;
    }

    if (stopping_.value.load(std::memory_order_acquire) &&
        active_enqueues_.value.load(std::memory_order_acquire) == 0) {
      break;
    }

    // Both sides exchange this flag. If the producer exchanges first, acquire
    // makes its publication visible to the second dequeue; otherwise the
    // producer clears the waiting request and notifies the consumer.
    // Replacing the exchanges with plain loads/stores can lose a wakeup.
    consumer_waiting_.value.exchange(true, std::memory_order_acq_rel);

    if (auto* op = TryDequeue(); op != nullptr) {
      consumer_waiting_.value.store(false, std::memory_order_release);
      op->Complete();
      continue;
    }

    if (stopping_.value.load(std::memory_order_acquire) &&
        active_enqueues_.value.load(std::memory_order_acquire) == 0) {
      consumer_waiting_.value.store(false, std::memory_order_release);
      break;
    }

    // Spin briefly before parking. The producer normally publishes the next
    // batch within a few hundred nanoseconds, while entering atomic::wait()
    // costs up to four sched_yield() syscalls before it blocks (see libstdc++'s
    // __atomic_spin) — more than the wait itself is worth. The flag protocol is
    // unchanged: a successful dequeue clears the waiting request, and a
    // producer that exchanges first still notifies.
    bool dequeued = false;
    for (int spin = 0; spin < kSpinBeforePark; ++spin) {
      CpuRelax();
      if (auto* op = TryDequeue(); op != nullptr) {
        consumer_waiting_.value.store(false, std::memory_order_release);
        op->Complete();
        dequeued = true;
        break;
      }
    }
    if (dequeued) continue;

    // A wakeup before wait() leaves false, so wait cannot miss it. Only this
    // consumer can set true again, after wait returns: there is no ABA here.
    consumer_waiting_.value.wait(true, std::memory_order_acquire);
  }

  // Drain remaining queued operations — complete them with set_stopped.
  while (true) {
    auto* op = TryDequeue();
    if (op == nullptr) break;
    op->CompleteStopped();
  }
}

inline void CmdContext::Stop() {
  stopping_.value.store(true, std::memory_order_release);
  WakeConsumer();
}

inline bool CmdContext::IsOnThread() const noexcept {
  return std::this_thread::get_id() == cmd_thread_id_;
}

inline bool CmdContext::Enqueue(CmdOpBase* op) noexcept {
  // If stopping, reject so the caller can deliver set_stopped().
  if (stopping_.value.load(std::memory_order_acquire)) return false;

  active_enqueues_.value.fetch_add(1, std::memory_order_acq_rel);

  if (stopping_.value.load(std::memory_order_acquire)) {
    FinishEnqueue();
    return false;
  }

  const size_t tail = tail_.value.load(std::memory_order_relaxed);
  const size_t head = head_.value.load(std::memory_order_acquire);
  if (tail - head >= queue_.size()) {
    FinishEnqueue();
    return false;
  }

  queue_[tail & queue_mask_] = op;
  tail_.value.store(tail + 1, std::memory_order_release);
  WakeConsumer();
  FinishEnqueue();
  return true;
}

inline CmdOpBase* CmdContext::TryDequeue() noexcept {
  const size_t head = head_.value.load(std::memory_order_relaxed);
  const size_t tail = tail_.value.load(std::memory_order_acquire);
  if (head == tail) return nullptr;

  const size_t slot = head & queue_mask_;
  auto* op = queue_[slot];
  queue_[slot] = nullptr;
  head_.value.store(head + 1, std::memory_order_release);
  return op;
}

inline void CmdContext::WakeConsumer() noexcept {
  // Always exchange, even when not waiting, to publish work/stop state to the
  // consumer's next acquire exchange before its final queue/stop checks.
  if (consumer_waiting_.value.exchange(false, std::memory_order_acq_rel)) {
    consumer_waiting_.value.notify_one();
  }
}

inline void CmdContext::FinishEnqueue() noexcept {
  const size_t previous =
      active_enqueues_.value.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 1 && stopping_.value.load(std::memory_order_acquire)) {
    WakeConsumer();
  }
}

inline size_t CmdContext::NormalizeQueueCapacity(
    size_t queue_capacity) noexcept {
  if (queue_capacity <= 1) return 1;
  if (std::has_single_bit(queue_capacity)) return queue_capacity;

  constexpr size_t kMaxPowerOfTwo =
      size_t{1} << (std::numeric_limits<size_t>::digits - 1);
  if (queue_capacity > kMaxPowerOfTwo) return kMaxPowerOfTwo;
  return std::bit_ceil(queue_capacity);
}

inline bool CmdContext::PostFunction(std::function<void()> fn) {
  struct FuncOp : CmdOpBase {
    std::function<void()> func;
    explicit FuncOp(std::function<void()> f) : func(std::move(f)) {}
    void Complete() noexcept override {
      func();
      delete this;
    }
    void CompleteStopped() noexcept override { delete this; }
  };
  auto* op = new FuncOp(std::move(fn));
  if (!Enqueue(op)) {
    delete op;
    return false;
  }
  return true;
}

}  // namespace miniredis
