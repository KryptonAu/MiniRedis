# 双线程交互重设计：批量命令提交 + 具体调度器完成

**目标版本:** `ccd54d7`
**相关报告:** `benchmarks/results/perf/REPORT_v6.md`（瓶颈定位）
**实现与实测:** `benchmarks/results/perf/REPORT_v7.md` —— L1+L2 已实现：
机制完全生效（类型擦除符号在火焰图中归零、用户态 CPU −16%、总 CPU −6.8%），
但吞吐受限于两线程串行与内核态开销，基本持平。实现中另外发现并修复了
`std::atomic::wait` 的 `sched_yield` 放大问题（见该报告 §4），因此 `CmdContext::Run()`
增加了 park 前的自适应自旋。

---

## 0. 结论

你的思路是对的，而且比"能不能"更好——**stdexec 提供了一个精确的开关，能让这条路彻底绕开类型擦除**：

> `exec::task` 的 `await_transform` 在把一个 sender 变成 awaitable 之前，会先问一句
> **"这个 sender 会在启动它的那个执行上下文上完成吗？"**
> 如果答案是"会"，就**直接**用具体 receiver 接上；否则才包一层
> `continues_on(get_start_scheduler(*__context_))` —— 而 `__context_` 正是那个
> **类型擦除**的 `__any_scheduler`，也就是火焰图里 `stdexec::__any::__value_proxy_root` /
> `__value_model` 的来源。

代码位置（stdexec `exec/task.hpp:600-617`）：

```cpp
template <sender _CvSender>
  requires __start_scheduler_provider<_Context>
auto await_transform(_CvSender&& __sndr) noexcept -> decltype(auto)
{
  if constexpr (__completes_where_it_starts<set_value_t,
                                            env_of_t<_CvSender>,
                                            __promise_context_t&>)
  {
    return STDEXEC::as_awaitable(static_cast<_CvSender&&>(__sndr), *this);   // ← 直接用具体 receiver
  }
  else
  {
    return STDEXEC::as_awaitable(continues_on(static_cast<_CvSender&&>(__sndr),
                                              get_start_scheduler(*__context_)),  // ← 类型擦除
                             *this);
  }
}
```

而 `__completes_where_it_starts` 完全由 sender 自己声明的 **completion behavior** 决定
（`stdexec/__detail/__completion_behavior.hpp:219`）：

```cpp
concept __completes_where_it_starts =
    __completion_behavior::__is_affine(__completion_behavior_of_v<_Tag, _Attrs, _Env...>);
```

本仓库已用编译期探针验证（见 §7.1）：声明了 `asynchronous_affine` 的 sender 判为 affine，
未声明的判为 not-affine。

**两个推论：**

1. 你的"轻量 Enqueue + op state 持有上下文 + CMD 线程独立执行 + 最后走框架回传"完全可以实现，
   而且回传那一步也能做到**零类型擦除**——只要让 CMD 线程把完成**投递回 IO 线程**，在 IO 线程上
   用具体 receiver `set_value`。这样"在启动它的上下文上完成"就是真命题，`continues_on(__any_scheduler)`
   根本不会被插入。
2. **顺带一个独立的大发现**：现在 `handle_client` 里的三次 await —— `AsyncReadSender`、
   `AsyncWriteSender`、`AsyncAcceptSender` —— **全都没有声明 completion behavior**，
   于是它们的每一次 I/O 完成也都插了一层 `continues_on(__any_scheduler)`。
   这三个 sender 本来就是 affine 的（`start()` 在 IO 线程、完成也在 IO 线程，
   commit `6e90554` 还专门加了 `assert(sched_->IsOnThread())`），却白付了类型擦除的代价。

---

## 1. 现状：一次批提交究竟走了哪些框架层

以 P16（每批 16 条命令）为例，`client_session.h:95-136` 的

```cpp
std::string replies = co_await (cmd_sched.schedule() | stdexec::then([&]() { ... }));
```

实际展开为：

```
IO 线程
 ① co_await  → await_transform(sender)
        └─ __completes_where_it_starts? 否（CmdScheduleSender 未声明 completion behavior）
        └─ 包一层 continues_on(cmd_sender, any_scheduler{io_sched})
 ② connect   → 在协程帧里构造 then 的 op state
                 └─ 内含 CmdScheduleOpState< then_receiver >
 ③ start()   → CmdContext::Enqueue(op)                     ← 这一步已经是你要的"轻量 Enqueue"
 ④ 协程挂起，控制权回 epoll_wait

CMD 线程
 ⑤ Run() → TryDequeue → op->Complete() → stdexec::set_value(rcvr)
 ⑥ then 的 receiver → 调用 lambda（16 条命令）
 ⑦ set_value(std::string) → 继续在外层 receiver 链上传递
 ⑧ continues_on 生效：__any_scheduler::schedule()
        └─ 构造 any_sender / any_receiver（类型擦除）
        └─ EpollScheduleOpState<any_receiver>::start() → EpollContext::Enqueue（eventfd）

IO 线程
 ⑨ ProcessReadyQueue → EpollScheduleOpState<any_receiver>::Complete()
        └─ 虚调用 _ireceiver_memfn::set_value()            ← 类型擦除的 receiver
        └─ 协程恢复
```

火焰图证据（v6，self time）：

| 符号 | P16 | mixed P1 | c500 P1 | 归属 |
|------|-----|----------|---------|------|
| `stdexec::__any::__value_proxy_root` | 6.41% | 10.20% | 10.69% | `__any_scheduler`/`any_sender` 的值存储 |
| `stdexec::__any::__value_model` | 5.06% | 7.37% | 6.61% | 同上 |
| `miniredis::CmdScheduleOpState` | 7.27% | 3.60% | 5.11% | ⑤⑥⑦ 的恢复链 |
| `experimental::execution::__task::basic_task` | 4.07% | 6.33% | 7.53% | 协程 await 机制 |
| `experimental::execution::_any::_ireceiver_memfn` | — | 1.91% | 1.82% | ⑨ 的虚调用（**inclusive 40~59%**） |

调用者归属分析确认：`value_proxy_root` 在 c500 场景的 462 个样本里 **336 个的调用者是
`experimental::execution::__task::basic_task`** —— 正是上面第 ① 步插入的那层。

**问题的本质**：⑤~⑨ 这段是**纯框架开销**，而且它是每条命令都要付的固定成本；
`value_model`/`value_proxy_root` 两个符号就占 11.5% / 17.6% / 17.3%。

---

## 2. 设计总览

分两层，可以分开落地、分开验证。

| 层 | 内容 | 改动量 | 风险 |
|----|------|--------|------|
| **L1** | 给 `AsyncReadSender` / `AsyncWriteSender` / `AsyncAcceptSender` 声明 `asynchronous_affine` | ~10 行 | 极低（只是把已成立的事实写出来） |
| **L2** | 新增 `CmdBatchSender`：op state 即 SPSC 队列节点，CMD 线程一次虚调用执行整批，回程自己投递到 IO 线程用**具体** receiver 完成 | ~120 行 + `handle_client` 收敛 | 中（停止/生命周期语义要写清楚） |

---

## 3. L1：给 I/O sender 声明 affinity

`src/io/async_io.h`，三个 sender 各加：

```cpp
  // 提交 6e90554 之后：start() 断言在 IO 线程执行；完成由
  // EpollContext::ProcessIoEvent / ProcessReadyQueue 在 IO 线程投递。
  // 所以"在启动我的上下文上完成"是真命题。
  struct env {
    template <class Tag>
    static constexpr auto query(exec::get_completion_behavior_t<Tag>) noexcept {
      return exec::completion_behavior::asynchronous_affine;
    }
  };
  auto get_env() const noexcept -> env { return {}; }
```

注：`AsyncWriteSender::start()` 在能直接 `::send` 完时是 **inline 完成**，`EAGAIN` 时是异步完成。
两者都是 affine（`__is_affine` 只看 `__not_affine_` 位），所以声明 `asynchronous_affine` 覆盖两种情况，
且不影响 `exec::task` 的判定。

**收益**：`handle_client` 里每次 `co_await AsyncReadSender{...}`、`co_await (writer)` 都少一层
`continues_on(any_scheduler)`。read/write 是每批各一次，P1 场景则是每命令各一次 —— 对 P1 意义更大。

---

## 4. L2：`CmdBatchSender` —— op state 即队列节点

### 4.1 新文件 `src/io/cmd_batch.h`

```cpp
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <stdexec/execution.hpp>

#include "commands/command_context.h"
#include "io/cmd_context.h"
#include "io/epoll_context.h"

namespace miniredis {

// CMD 线程执行一批命令所需的全部上下文。由协程帧持有，生命周期覆盖整次提交。
struct CmdBatchContext {
  Server* server;
  Client* client;
  CommandRegistry* registry;
  const CommandContext::PropagateFn* propagate;
  const CommandContext::ApplyConfigFn* apply_config;
  std::span<const std::span<const std::string_view>> args;  // 每条命令的参数视图
  size_t count = 0;
};

// 纯函数：在 CMD 线程上按序执行整批并拼接回复。
std::string RunCommandBatch(const CmdBatchContext& batch);   // 实现见 §4.4

// ---------------------------------------------------------------------------
// CmdBatchSender
// ---------------------------------------------------------------------------
class CmdBatchSender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(std::string),
                                     stdexec::set_stopped_t()>;

  CmdContext* cmd_ctx_ = nullptr;
  EpollContext* io_ctx_ = nullptr;
  CmdBatchContext* batch_ = nullptr;

  template <class Rcvr>
  struct OpState;

  template <class Rcvr>
  auto connect(Rcvr rcvr) const noexcept -> OpState<Rcvr> {
    return OpState<Rcvr>{*this, std::move(rcvr)};
  }

  // ★ 关键：告诉 exec::task "我会在启动我的上下文（IO 线程）上完成"，
  //   于是 task 的 await_transform 不会包 continues_on(__any_scheduler)。
  struct env {
    template <class Tag>
    static constexpr auto query(exec::get_completion_behavior_t<Tag>) noexcept {
      return exec::completion_behavior::asynchronous_affine;
    }
  };
  auto get_env() const noexcept -> env { return {}; }
};

template <class Rcvr>
struct CmdBatchSender::OpState final : CmdOpBase {
  CmdContext* cmd_ctx_;
  EpollContext* io_ctx_;
  CmdBatchContext* batch_;
  Rcvr rcvr_;
  std::string result_;   // CMD 线程写、IO 线程读（由队列的 release/acquire 建立 happens-before）

  // 回程节点：内嵌在 op state 里，零分配。用独立的 next_ 链接，不和 CmdOpBase 冲突。
  struct Deliver final : EpollOpBase {
    OpState* self_;
    explicit Deliver(OpState* self) noexcept : self_(self) {}
    void Complete() noexcept override {
      // 在 IO 线程上、用具体 receiver 完成 —— 没有 any_sender/any_receiver。
      stdexec::set_value(std::move(self_->rcvr_), std::move(self_->result_));
    }
  } deliver_{this};

  // ---- IO 线程：一次裸 Enqueue，不带任何框架 ----
  void start() & noexcept {
    if (!cmd_ctx_->Enqueue(this)) {
      // 队列满 / 正在停止：就地 set_stopped（本身也是 affine）
      stdexec::set_stopped(std::move(rcvr_));
    }
  }

  // ---- CMD 线程：一次虚调用跑完整批，中途不碰框架 ----
  void Complete() noexcept override {
    result_ = RunCommandBatch(*batch_);
    DeliverToIo();
  }

  void CompleteStopped() noexcept override {
    DeliverToIo(/*stopped=*/true);
  }

 private:
  void DeliverToIo(bool stopped = false) noexcept {
    if (io_ctx_->Enqueue(&deliver_)) return;      // 唯一一次调度介入
    // IO 循环已停止（关机路径）：与现有 continues_on 失败路径一致，就地完成。
    stopped ? stdexec::set_stopped(std::move(rcvr_))
            : stdexec::set_value(std::move(rcvr_), std::move(result_));
  }
};

}  // namespace miniredis
```

### 4.2 `handle_client` 的改法

```cpp
    CmdBatchContext batch_ctx{&server, client, &registry, &propagate, &apply_config};

    while (true) {
      // ... 读 + 解析出 count 条命令（与现在相同）...
      if (count > 0) {
        batch_ctx.args  = std::span<const std::span<const std::string_view>>(batch_args.data(), count);
        batch_ctx.count = count;

        std::string replies = co_await CmdBatchSender{&cmd_ctx, io_sched.GetContext(), &batch_ctx};

        assert(io_sched.GetContext()->IsOnThread());  // 守住 affinity 契约（Debug）
        query_buf.Consume(scanned);
        for (size_t i = 0; i < count; ++i) batch_storage[i].ReleaseOversizedHeap();
        reply_buf += replies;
      }
      // ...
    }
```

`CmdContext::scheduler` 需要补一个 `GetContext()`（与 `EpollContext::scheduler::GetContext()` 对称），
或者把 `handle_client` 的形参从 `CmdContext::scheduler` 换成 `CmdContext&`。

### 4.3 与现状逐项对比（每批一次）

| 环节 | 现在 | 新设计 |
|------|------|--------|
| 提交端构造 | `continues_on` 包装 + `then` sexpr op state + `CmdScheduleOpState<then_receiver>` | 直接构造 `CmdBatchSender::OpState` |
| 提交 | `CmdContext::Enqueue`（SPSC push + 按需 futex） | **同左，不变** |
| CMD 执行 | `Complete()` → `set_value` → then receiver → lambda → `set_value`（4~5 层 receiver） | `Complete()` → `RunCommandBatch()`（**1 层虚调用**） |
| 回程调度 | `continues_on(__any_scheduler)` → `__any_scheduler::schedule()` → `any_sender`（类型擦除）→ `EpollScheduleOpState<any_receiver>` | `EpollContext::Enqueue(&deliver_)`（内嵌节点，无分配、无擦除） |
| 回程完成 | 虚 `_ireceiver_memfn::set_value` → `__async_receiver` → 协程 | 具体 `set_value` → 协程 |
| 类型擦除 | 有（`value_proxy_root`/`value_model`） | **无** |

### 4.4 `RunCommandBatch` 就是现在 lambda 的搬家

语义必须逐条保持（尤其是批内 `SELECT`）：

```cpp
std::string RunCommandBatch(const CmdBatchContext& batch) {
  std::string out;
  for (size_t i = 0; i < batch.count; ++i) {
    // 每条重新解析，使批内 SELECT 对后续命令生效
    Database* db = batch.server->GetDbFor(*batch.client);
    if (db == nullptr) { out += RespReply::Error("ERR invalid DB index"); continue; }

    CommandContext ctx{*batch.server, *batch.client, *db};
    if (*batch.propagate)   ctx.propagate    = std::cref(*batch.propagate);
    if (*batch.apply_config) ctx.apply_config = std::cref(*batch.apply_config);
    out += ExecuteCommand(*batch.registry, ctx, batch.args[i]);
  }
  return out;
}
```

---

## 5. 正确性：必须写清楚的语义

### 5.1 affinity 契约

`asynchronous_affine` 是一个**契约**：完成必须发生在启动它的那个执行上下文上。
本设计满足它：`start()` 在 IO 线程（`handle_client` 在 IO 线程恢复），完成由
`EpollContext::Enqueue(&deliver_)` → `ProcessReadyQueue` 在 IO 线程投递。
**任何"在 CMD 线程上直接 set_value"的写法都会破坏这个契约**（协程会错误地在 CMD 线程恢复，
进而从 CMD 线程碰 `query_buf`/`reply_buf`）。所以 `DeliverToIo` 只有在 `Enqueue` 失败时才就地完成，
且要加注释说明这是关机路径。Debug 构建下在 await 之后加 `assert(io_ctx->IsOnThread())` 兜底。

### 5.2 停止路径

| 情形 | 处理 |
|------|------|
| `CmdContext::Enqueue` 返回 false（队列满 / cmd 已停止） | `start()` 就地 `set_stopped`（IO 线程，affine ✓） |
| `CmdContext::Run()` 排水时调用 `CompleteStopped()`（CMD 线程） | `DeliverToIo(stopped=true)` → 投递回 IO 再 `set_stopped` |
| `EpollContext::Enqueue` 返回 false（IO 已停止） | 就地 `set_stopped`（与现有 `continues_on` 失败路径行为一致） |

⚠️ **这是最容易出错的地方**：如果 IO 已停止而完成节点永远不被执行，`handle_client` 协程永不结束，
`main.cpp:282` 的 `stdexec::sync_wait(scope.on_empty())` 会挂住。必须在关机压测中专门验证
（边打流量边 Ctrl-C、500 连接下关机）。

现有实现同样依赖"`EpollContext::Enqueue` 失败 → 就地 set_stopped"，所以这不是新引入的风险，
但新代码必须显式复刻这条路径。

### 5.3 生命周期

op state 存放在协程帧里（`co_await` 的 awaiter），队列里只有裸指针：

* CMD 队列持有指针期间，协程正挂在 `co_await` 上，帧必然存活 → 安全；
* 反过来说，**不能**在 op 在队期间销毁协程帧（`exec::async_scope` 的 `on_empty()` 语义保证了这一点：
  协程结束才会被销毁，而协程结束要求 op 完成）；
* `CmdBatchSender::OpState` 必须 **non-movable**（和现有 `CmdScheduleOpState` 一样 `= delete` 移动构造），
  因为 `deliver_{this}` 捕获了自身地址；C++17 保证 `connect()` 返回 prvalue 时直接构造在最终位置。

### 5.4 内存序

`result_` 由 CMD 线程写、IO 线程读，跨线程可见性由 `EpollContext::Enqueue` 的
`head_.compare_exchange_weak(..., release)` 与 `ProcessReadyQueue` 的
`head_.exchange(nullptr, acquire)` 建立，与现有 ready-queue 语义一致。
`CmdContext::Enqueue` / `TryDequeue` 的 release/acquire 同理保证批参数对 CMD 线程可见。

### 5.5 参数视图生命周期

`batch.args` 指向 `query_buf`，`batch_storage` 在协程帧里。
只要 `query_buf.Consume(scanned)` 仍然发生在 await **之后**（保持现状），视图就有效。

---

## 6. 预期收益

### 6.1 直接消失的符号（v6 实测 self time）

| 符号 | P16 | mixed P1 | c500 P1 | 新设计下 |
|------|-----|----------|---------|---------|
| `stdexec::__any::__value_proxy_root` | 6.41% | 10.20% | 10.69% | 基本消失 |
| `stdexec::__any::__value_model` | 5.06% | 7.37% | 6.61% | 基本消失 |
| `miniredis::CmdScheduleOpState` | 7.27% | 3.60% | 5.11% | 变成更薄的 `OpState::Complete` |
| `_any::_ireceiver_memfn`（含所有 await） | — | 1.91% | 1.82% | 具体调用，虚分发消失 |
| `__task::basic_task` | 4.07% | 6.33% | 7.53% | 减少（每次 await 少一层包装） |

L1 额外覆盖 read/write 两次 await；L2 覆盖批提交。

### 6.2 端到端

* **CMD 线程是关键路径上的串行点**。`Complete()` 从"4~5 层 receiver + 类型擦除回程调度"
  收缩成"1 次虚调用 + 1 次裸 Enqueue"，直接缩短串行段。
* 粗估：**用户态 CPU −15% ~ −25%**；受内核态占比（57%~72%）稀释后，
  端到端吞吐预计 **+8% ~ +20%**（P16 与 P1 分别实测确认）。
* 更重要的验收指标是**火焰图里 `__value_proxy_root` / `__value_model` 从前 20 名消失**。

> 这个估算必须在实现后用同一套场景复测，不接受推算结论。

---

## 7. 验证方案

### 7.1 机制验证（已完成）

用一个 20 行的编译期探针确认 `__completes_where_it_starts` 由 completion behavior 决定：

```cpp
struct AffineSender { /* env 里 query(get_completion_behavior_t<Tag>) -> asynchronous_affine */ };
struct PlainSender  { /* 不声明 */ };

template <class S>
constexpr bool completes_where_it_starts() {
  using Attrs = stdexec::env_of_t<S>;
  return stdexec::__completion_behavior::__is_affine(
      stdexec::__completion_behavior_of_v<stdexec::set_value_t, Attrs>);
}
static_assert(completes_where_it_starts<AffineSender>());
static_assert(!completes_where_it_starts<PlainSender>());
```

实测输出：`affine=1 plain=0` —— 声明 affine 确实让 `exec/task.hpp:605` 走"不包 `continues_on`"分支。

### 7.2 功能验证

* `ctest`：389 个用例。
* 协议级脚本（沿用 REPORT_v6 的方式）：跨批顺序、批内 `SELECT`、21 参数 `MSET`、
  TCP 分包、批内协议错误后刷回复。
* **新增单测** `tests/io/cmd_batch_test.cpp`：
  1. 批次在 CMD 线程执行（记录 `cmd_ctx.IsOnThread()`）；
  2. 完成在 IO 线程投递（记录线程 id）；
  3. `CmdContext` 停止时 `set_stopped`；
  4. `CmdContext` 队列满时 `set_stopped`；
  5. 关机路径：IO 上下文先停止时协程不悬挂（用超时守护）。
* **关机压测**：500 连接持续打流量的同时发 SIGINT，确认进程能退出（`on_empty()` 不 hang）。

### 7.3 性能验证

同一套三个场景 + `perf_summary.py`：

```bash
# 基线（已有）：perf_{throughput,mixed,highconn}_v6.{data,perf}
benchmarks/scripts/profile_cpu.sh -s throughput -c 50 -n 4000000 -P 16 -F 999 --no-build -l v7
benchmarks/scripts/profile_cpu.sh -s mixed      -c 50 -n 100000      -F 999 --no-build -l v7
benchmarks/scripts/profile_cpu.sh -s highconn   -c 500 -n 500000     -F 999 --no-build -l v7
```

---

## 8. 变体（更激进，可选）

把 `handle_client` 的协程整体去掉，改成**每连接显式状态机**：

```
IO:   read 完成 → 解析 → cmd_ctx.Enqueue(batch_op) → 返回事件循环
CMD:  执行 → 追加到 client->ReplyBuffer() → io_ctx.Enqueue(flush_op)
IO:  flush_op::Complete() → 直接 send / AsyncWriteSender → 重新 arm read
```

* 收益：`exec::task` 从热路径彻底消失（没有协程帧、没有 `await_transform`），
  `value_proxy_root`/`value_model`/`basic_task` 全部归零。
* 代价：连接的读/写/关闭状态要手写；`exec::async_scope` 的连接生命周期管理要换成
  自维护的注册表 + 引用计数；可读性下降。
* 建议：**先做 L1+L2 并测量**，如果 `basic_task` 仍是显著项再考虑这一步。

---

## 9. 落地顺序

1. **L1**（~10 行，低风险）：三个 I/O sender 加 env；跑 ctest + 三场景 profile，确认
   read/write 路径上的 `continues_on` 消失。
2. **L2-A**（~80 行）：新增 `cmd_batch.h` + `RunCommandBatch`，`handle_client` 切换到
   `CmdBatchSender`，`CmdContext::scheduler` 补 `GetContext()`。跑 ctest + 协议脚本。
3. **L2-B**（安全网）：新增 `cmd_batch_test.cpp`，覆盖 §7.2 的 5 个用例；做关机压测。
4. **测量**：三场景复测 + 火焰图对比；确认 §6 的符号消失、吞吐提升。
5. **清理（已完成）**：`CmdScheduleSender` / `CmdScheduleOpState` / `CmdContext::scheduler`
   已删除 —— 生产路径零引用（`nm` 确认服务器二进制中 `CmdSchedule*` 符号为 0），
   测试改用 `PostFunction`（fire-and-forget）与 `CmdBatchSender`（IO↔CMD 往返）。
   `handle_client` / `accept_loop` 现在直接接收 `CmdContext&`；
   `Client::ArgStorage()`（同样已被批处理取代）尚未清理。

---

## 10. 附：关键源码位置

| 内容 | 位置 |
|------|------|
| task 的 await_transform 分叉（本设计的开关） | `build/Release/_deps/stdexec-src/include/exec/task.hpp:600-617` |
| `__completes_where_it_starts` 定义 | `.../stdexec/__detail/__completion_behavior.hpp:219` |
| completion behavior 语义（affine / inline / async） | `.../stdexec/__detail/__completion_behavior.hpp:50-120` |
| 声明 query 的既有范例 | `.../stdexec/__detail/__just.hpp:47-53` |
| sticky `__any_scheduler` 的定义 | `.../exec/task.hpp:49-140` |
| 公开的 `exec::get_completion_behavior_t` / `exec::completion_behavior` | `.../exec/completion_behavior.hpp:17-70` |
| 现有提交路径 | `src/io/client_session.h:95-136`（`co_await` 在 `:102`） |
| SPSC 队列 | `src/io/cmd_context.h:251`（`Enqueue`）、`:197`（`Run`） |
| ready queue（回程用） | `src/io/epoll_context.cpp:127`（`Enqueue`，已做唤醒合并）、`:257`（`ProcessReadyQueue`） |
| I/O sender | `src/io/async_io.h:49`（`AsyncReadSender`）、`:104`（`AsyncWriteSender`）、`:160`（`AsyncAcceptSender`） |
| 关机顺序 | `src/main.cpp:281-283`（`io_ctx.Stop()` → `sync_wait(scope.on_empty())` → `cmd_ctx.Stop()`） |
