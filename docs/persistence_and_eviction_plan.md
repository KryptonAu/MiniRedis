# 持久化和淘汰模块实现计划

> 状态：已审阅并修订 | 日期：2026-06-29

## 审阅结论

原计划的目标方向正确：MiniRedis 当前已有核心数据库、类型系统、命令框架和 epoll + CMD 双线程运行时，但 `src/persistence/` 与 `src/eviction/` 仍为空，需要补齐 RDB/AOF、主动过期、`maxmemory` 和近似 LRU 淘汰。

需要修正的关键点如下：

1. **阶段顺序需要调整**：主动过期、RDB、AOF 和淘汰都依赖数据库遍历、过期元数据读取、随机抽样、内存估算、命令传播结果等基础 API，必须先补这些边界。
2. **不能直接访问 `Database::keyspace_` / `expires_`**：当前二者是私有成员，计划必须新增受控遍历、抽样和元数据接口，而不是让持久化/淘汰模块绕过 `Database`。
3. **不能制造 CMake 循环依赖**：`commands` 需要调用 RDB/AOF，但 AOF replay 又想调用 `CommandRegistry`。首版应让 `persistence` 只依赖 `core`，AOF 读取返回命令数组，启动流程在 `main.cpp` 用 registry 重放。
4. **RDB 兼容目标要分级**：首版可以写 Redis 可识别的通用类型编码；不要用 `RDB_TYPE_LIST_QUICKLIST` 等现代 type byte，除非实现了完全对应的 quicklist/listpack RDB payload。
5. **AOF 传播必须基于结构化执行结果**：当前 `ExecuteCommand()` 只返回 RESP 字符串。仅凭 `CommandFlag::kWrite` 会错误记录失败写命令、条件未命中的写命令或 replay 期间的命令。
6. **`BGSAVE` 不能默认按原计划实现**：MiniRedis 是多线程进程，子进程在 `fork()` 后继续执行复杂 C++ 序列化存在安全风险。首版先实现 `SAVE` 和 `LASTSAVE`，`BGSAVE` 进入明确延期项。
7. **配置需要运行时生效路径**：`ConfigManager::Set()` 目前只改自身配置，`Server::GetConfig()` 返回的是初始化时复制的配置。`CONFIG SET` 不能只扩展 `ConfigManager`，还要通过 `Server` 应用运行时配置。

---

## 当前代码基线

- `Server` 当前只持有 `MiniRedisConfig`、`std::vector<Database>`、客户端表和运行状态；没有 dirty 计数、统计、cron、持久化状态或 maxmemory 状态。
- `Database` 当前持有私有 `keyspace_` 与 `expires_`；`Find()`、`Exists()`、`Size()`、`Keys()` 等会触发惰性过期；`TTL()` 返回毫秒级剩余时间。
- `CmdContext` 已经提供跨线程 `Enqueue(CmdOpBase*)`，适合承载 timer 唤醒后的 cron thunk。
- `EpollContext` 当前只有 `wake_fd_` 和普通 I/O fd 管理，没有 timerfd。
- `CommandRegistry` 已经有 `CommandFlag::kReadOnly`、`kWrite`、`kAdmin`、`kMayCreateKey`；各命令注册多数已经标注 read/write。
- `ExecuteCommand()` 目前只做 arity 检查并返回 `std::string` RESP 回复，没有 dirty/no-dirty、成功/失败或传播参数。
- `CONFIG GET` 命令当前只支持 `databases`、`port`、`bind`；`CONFIG SET` 返回未实现。
- `Listpack` 和 `Intset` 已有 `Data()` / `DataSize()` / `FromBytes()`，但 `Quicklist` 和类型层没有完整 Redis RDB payload 导出接口。

---

## 目标与非目标

### 首版目标

1. 支持 `SAVE` / 启动加载 RDB，使 MiniRedis 自己生成的数据重启后恢复。
2. RDB 写出优先使用 Redis 通用类型编码，避免伪兼容；至少通过自有 round-trip 测试，并为 `redis-check-rdb` 留出明确验收项。
3. 支持 AOF append、启动重放、`appendfsync` 策略和 `CONFIG GET/SET appendonly` 基础能力。
4. 增加 serverCron，周期执行 LRU clock 更新、主动过期、AOF flush 和自动保存检查。
5. 支持 `maxmemory`、`maxmemory-policy noeviction/allkeys-lru/volatile-lru` 和近似 LRU 淘汰。
6. 补充持久化、过期、淘汰、配置、dispatcher 传播路径的 focused tests，并接入 CTest。

### 首版非目标

1. 暂不实现 `BGSAVE` / `BGREWRITEAOF`，除非先完成 fork 安全设计。
2. 暂不承诺加载任意 Redis 真实 `dump.rdb`。先保证 MiniRedis 自己写出的兼容通用编码可读回，再扩展 Redis 现代紧凑编码读取。
3. 暂不做精确 jemalloc 级内存统计。首版使用稳定、可测试的近似内存估算。
4. 暂不实现 Redis 全量淘汰策略；首版只做 `noeviction`、`allkeys-lru`、`volatile-lru`。

---

## 总体架构

```
IO 线程 (EpollContext)                         CMD 线程 (CmdContext)
──────────────────────                         ─────────────────────
epoll_wait                                      command / cron 单线程执行
  ├─ wake_fd_                                   ├─ ExecuteCommand()
  ├─ socket read/write                          │   ├─ OOM/eviction preflight
  └─ timer_fd_                                  │   ├─ handler mutates DB
      └─ timer callback                         │   └─ success write -> AOF/dirty
          └─ PostFunction(cmd_ctx, ...)         └─ ServerCron()
                                                      ├─ UpdateLruClock()
                                                      ├─ ActiveExpireCycle()
                                                      ├─ PerformEvictions()
                                                      ├─ FlushAofIfNeeded()
                                                      └─ TriggerAutoSave()
```

核心规则：

- 所有 `Server` / `Database` 读写仍在 CMD 线程执行。
- `EpollContext` 只负责定时唤醒，不直接访问数据库。
- `persistence` 和 `eviction` 只通过公开 API 操作 `Server` / `Database`。
- RDB/AOF 启动加载在线程启动前完成；运行期 `SAVE` 在 CMD 线程同步执行。

---

## 实现阶段

### 阶段 0：基础 API、分层和测试脚手架

**目标**：先补齐后续模块共同依赖的 API，避免每个模块各自穿透内部结构。

#### 0.1 数据库遍历与元数据接口

**文件**：`src/core/database.h` / `src/core/database.cpp`

新增公开 API：

```cpp
struct KeyView {
  std::string_view key;
  const Value& value;
  std::optional<int64_t> expire_at_ms;
  uint32_t lru_clock;
};

using KeyVisitor = std::function<void(const KeyView&)>;

size_t PurgeExpiredKeys(int64_t now_ms);
void ForEachKey(KeyVisitor visitor);
std::vector<std::string> SampleKeys(size_t count, bool only_volatile);
std::optional<int64_t> ExpireAt(std::string_view key) const;
void RestoreValue(std::string key, Value value, std::optional<int64_t> expire_at_ms);
size_t ApproxMemoryUsage() const;
```

实现要求：

- `PurgeExpiredKeys()` 从 private 改为 public，返回实际删除数，并接受 `now_ms` 以便测试不依赖 sleep。
- `ForEachKey()` 进入遍历前先清理过期 key；visitor 只在 CMD 线程内使用，不能保存 `Value&`。
- `RestoreValue()` 用于 RDB/AOF 加载，必须同时恢复 value、expire、LRU，并遵守 move-only `Value` 语义。
- `Delete()`、`Rename()`、`RenameNX()`、`Clear()` 必须同步维护 `expires_` 和新增的 LRU 元数据。

#### 0.2 Dict 随机抽样

**文件**：`src/ds/dict.h`

新增：

```cpp
std::vector<Entry*> GetSomeKeys(size_t count, uint64_t seed);
```

实现要求：

- 支持 rehash 中的 `ht_[0]` 与 `ht_[1]`。
- 抽样不得总是返回第一个非空桶，避免当前 `RandomKey()` 的确定性偏差。
- 返回 entry 指针只在当前字典未修改期间有效。
- 增加 `tests/ds/dict_test.cpp` 覆盖：空表、少量 key、rehash 中抽样、重复率基本约束。

#### 0.3 Value 序列化辅助和内存估算

**文件**：`src/types/value.h` / `src/types/value.cpp`

新增：

```cpp
size_t ApproxMemoryUsage(const Value& value);
```

后续如需要无损 RDB payload，可再新增显式序列化 helper，不启用隐式 copy constructor。

#### 0.4 命令执行结果结构化

**文件**：`src/commands/dispatcher.h` / `src/commands/dispatcher.cpp`

将内部执行路径扩展为结构化结果：

```cpp
struct CommandResult {
  std::string reply;
  bool ok = false;              // handler 是否成功执行
  bool mutated = false;         // 是否实际改变 DB
  std::vector<std::string> propagate_args;
};

CommandResult ExecuteCommandDetailed(CommandRegistry& registry,
                                     CommandContext& context,
                                     const std::vector<std::string>& args);
std::string ExecuteCommand(CommandRegistry& registry, CommandContext& context,
                           const std::vector<std::string>& args);
```

实现要求：

- 保留现有 `ExecuteCommand()` 包装，减少测试改动。
- 先用保守策略：RESP error 视为 `ok=false`；写命令默认 `mutated=true`；条件命令逐步改成精确 dirty。
- AOF replay 期间必须能关闭传播和 dirty 计数。
- 对 `SETNX`、`MSETNX`、`EXPIRE`、`PEXPIRE`、`PERSIST`、`DEL` 这类条件写命令补精确 `mutated` 测试。

#### 0.5 CMake 分层

推荐依赖方向：

```
miniredis_ds
  └─ miniredis_types
       └─ miniredis_core
            ├─ miniredis_persistence
            └─ miniredis_eviction
                 └─ miniredis_commands
                      └─ miniredis_io
                           └─ miniredis executable
```

实际 CMake 写法：

- `miniredis_persistence` 依赖 `miniredis_core`。
- `miniredis_eviction` 依赖 `miniredis_core`。
- `miniredis_commands` 依赖 `miniredis_core`、`miniredis_persistence`、`miniredis_eviction`。
- `miniredis_io` 继续依赖 `miniredis_commands`。
- AOF replay orchestration 放在 `main.cpp` 或独立 executable-level helper，避免 `persistence -> commands -> persistence` 循环。

---

### 阶段 1：运行时配置、统计和 serverCron

**目标**：建立周期性任务机制和运行时状态，但不先实现复杂持久化。

#### 1.1 Config 扩展

**文件**：`src/core/config.h` / `src/core/config.cpp`

新增字段：

```cpp
size_t maxmemory = 0;
std::string maxmemory_policy = "noeviction";
int maxmemory_samples = 5;
int hz = 10;
bool appendonly = false;
std::string appendfsync = "everysec";
std::vector<std::pair<int, int>> save_params = {{3600, 1}, {300, 100}, {60, 10000}};
int active_expire_effort = 1;
```

实现要求：

- `Get()` / `Set()` 必须对称。
- 支持 Redis 风格连字符 key：`maxmemory-policy`、`maxmemory-samples`、`appendfsync`、`appendonly`、`appendfilename`、`dbfilename`。
- 保留现有下划线 key 作为别名，避免破坏当前测试。
- `maxmemory-policy` 只接受 `noeviction`、`allkeys-lru`、`volatile-lru`。
- `appendfsync` 只接受 `always`、`everysec`、`no`。

#### 1.2 Server 运行时状态

**文件**：`src/core/server.h` / `src/core/server.cpp`

新增状态和访问器：

```cpp
struct ServerStats {
  uint64_t dirty = 0;
  uint64_t keyspace_hits = 0;
  uint64_t keyspace_misses = 0;
  uint64_t expired_keys = 0;
  uint64_t evicted_keys = 0;
};

const ServerStats& Stats() const;
void IncrementDirty(uint64_t delta = 1);
void ResetDirty();
uint32_t LruClock() const;
void UpdateLruClock(int64_t now_ms);
size_t ApproxMemoryUsage() const;
bool ApplyConfig(std::string_view key, std::string_view value);
```

注意：

- 首版仍保持 CMD 线程单写模型，统计字段可先不用 atomic；若 IO 线程也读取，再改为 atomic。
- `Server::ApplyConfig()` 必须更新 `config_`，而不仅是 `ConfigManager` 的临时对象。
- `INFO stats` 后续从 `ServerStats` 输出。

#### 1.3 EpollContext timerfd

**文件**：`src/io/epoll_context.h` / `src/io/epoll_context.cpp`

新增通用 timer API：

```cpp
using TimerCallback = std::function<void(uint64_t expirations)>;

bool ArmPeriodicTimer(uint64_t interval_ms, TimerCallback callback);
void DisarmTimer() noexcept;
```

实现要求：

- `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)`。
- 在 `epoll_wait` 返回后优先识别 `timer_fd_`，读取 `uint64_t expirations` 并执行 callback。
- callback 运行在 IO 线程；它只能通过 `CmdContext` 投递 CMD 任务，不能直接访问 DB。
- 析构和 `Stop()` 路径关闭 timerfd。
- `tests/io/epoll_context_test.cpp` 增加 timer 唤醒、取消、Stop 后不再触发的测试。

#### 1.4 CMD 线程函数任务

**文件**：`src/io/cmd_context.h`

新增可复用 helper：

```cpp
bool PostFunction(CmdContext& ctx, std::function<void()> fn);
```

实现为 heap allocated `CmdOpBase` thunk，`Complete()` 执行并自删除，`CompleteStopped()` 只自删除。

#### 1.5 serverCron 入口

**新文件**：`src/eviction/server_cron.h` / `src/eviction/server_cron.cpp`

新增：

```cpp
struct CronServices {
  std::function<void()> flush_aof_if_needed;
  std::function<void()> trigger_autosave;
};

void ServerCron(Server& server, CronServices services, int64_t now_ms);
```

执行顺序：

1. `server.UpdateLruClock(now_ms)`
2. 同步所有 DB 的当前 LRU 时钟
3. `ActiveExpireCycle(server, now_ms)`
4. `PerformEvictions(server)`
5. `services.flush_aof_if_needed()`
6. `services.trigger_autosave()`

---

### 阶段 2：主动过期

**目标**：在保留惰性过期的基础上，周期扫描未访问 key 的过期元数据。

**新文件**：`src/eviction/expire.h` / `src/eviction/expire.cpp`

核心接口：

```cpp
struct ActiveExpireConfig {
  int keys_per_loop = 20;
  int slow_time_perc = 25;
  int fast_duration_us = 1000;
};

size_t ActiveExpireCycle(Server& server, int64_t now_ms,
                         const ActiveExpireConfig& config = {});
```

算法：

1. `Server` 持有或传入 `current_db` 状态，避免函数级 static 污染测试。
2. 每个 DB 从 `expires_` 抽样 `keys_per_loop` 个 key。
3. 对过期 key 调用 `Database::Delete()`，同时递增 `expired_keys`。
4. 如果本轮过期比例高于 10%，继续扫描同一 DB。
5. 遵守时间预算：慢周期使用 `cron_interval_ms * slow_time_perc / 100`。

测试：

- 未过期 key 不删除。
- 过期 key 即使不被访问也会被删除。
- 大量过期 key 会多轮清理但不无限循环。
- 多 DB 轮转不会饿死后面的 DB。
- 与现有 `Find()` / `TTL()` 惰性过期语义一致。

---

### 阶段 3：LRU 与 maxmemory 淘汰

**目标**：实现可测试的近似 LRU，并在写命令前后保证内存策略生效。

#### 3.1 LRU 元数据

**文件**：`src/core/database.h` / `src/core/database.cpp`

新增：

```cpp
void SetCurrentLruClock(uint32_t clock);
void Touch(std::string_view key);
std::optional<uint32_t> LruOf(std::string_view key) const;
```

实现要求：

- `Find()` 成功时 touch。
- `Set()` / `RestoreValue()` 写入时 touch。
- `Rename()` 迁移 LRU；`Delete()` 删除 LRU。
- 24 位 clock 回绕按 Redis 逻辑处理：`idle = (now >= then) ? now - then : now + (LRU_CLOCK_MAX - then)`。

#### 3.2 内存估算

**文件**：`src/types/value.cpp`、`src/core/database.cpp`、`src/core/server.cpp`

实现：

- `ApproxMemoryUsage(StringValue/ListValue/SetValue/HashValue/ZSetValue)`。
- `Database::ApproxMemoryUsage()` 包含 key 字节、value 估算、expire/LRU 元数据粗略开销。
- `Server::ApproxMemoryUsage()` 汇总所有 DB。

首版只要求稳定和单调合理，不追求与 Redis `used_memory` 完全一致。

#### 3.3 淘汰策略

**新文件**：`src/eviction/evict.h` / `src/eviction/evict.cpp`

核心接口：

```cpp
enum class EvictionResult {
  kOk,
  kNoMemory,
};

EvictionResult PerformEvictions(Server& server);
bool ShouldRejectWriteForOom(const Server& server, const CommandInfo& cmd);
```

算法：

1. 如果 `maxmemory == 0`，直接 `kOk`。
2. 如果当前内存未超过限制，直接 `kOk`。
3. `noeviction`：对可能增加内存的写命令返回 OOM，但允许 `DEL`、`EXPIRE`、`PERSIST`、`FLUSH*` 等释放/元数据命令执行。
4. `allkeys-lru`：从所有 key 抽样。
5. `volatile-lru`：只从带过期时间的 key 抽样。
6. 使用 `maxmemory_samples` 填充候选池，删除 idle 最大的 key。
7. 如果无候选可删或删除后仍超限，返回 `kNoMemory`。

命令层要求：

- 在 handler 执行前做 OOM preflight。
- 对可能大幅增加内存的命令，可在执行后再次调用 `PerformEvictions()`。
- 如果后置淘汰失败，命令不能留下半写状态；首版可选择保守地在执行前拒绝高风险写命令，后续再做精细增长估算。

测试：

- `noeviction` 超限后拒绝 `SET`，但允许 `DEL`。
- `allkeys-lru` 淘汰最久未访问 key。
- `volatile-lru` 只淘汰设置了 TTL 的 key。
- LRU 回绕情况下 idle 计算正确。
- 淘汰统计 `evicted_keys` 递增。

---

### 阶段 4：RDB 持久化

**目标**：实现同步 RDB 保存和启动加载，优先保证自有 round-trip，再扩展 Redis 互操作。

#### 4.1 RDB 格式策略

**新文件**：`src/persistence/rdb_format.h`

首版写出 Redis 通用类型，避免未实现 payload 的现代紧凑类型：

```cpp
namespace rdb {
constexpr uint8_t kOpAux = 0xFA;
constexpr uint8_t kOpResizeDb = 0xFB;
constexpr uint8_t kOpExpireTimeMs = 0xFC;
constexpr uint8_t kOpSelectDb = 0xFE;
constexpr uint8_t kOpEof = 0xFF;

constexpr uint8_t kTypeString = 0;
constexpr uint8_t kTypeList = 1;
constexpr uint8_t kTypeSet = 2;
constexpr uint8_t kTypeZSet2 = 5;
constexpr uint8_t kTypeHash = 4;
constexpr int kRdbVersion = 11;
}
```

说明：

- 列表首版写 `RDB_TYPE_LIST`：元素个数 + 元素字符串。
- 集合首版写 `RDB_TYPE_SET`：成员个数 + 成员字符串。
- 哈希首版写 `RDB_TYPE_HASH`：field/value 对。
- 有序集合首版写 `RDB_TYPE_ZSET_2`：member + binary double。
- 不写 `LIST_QUICKLIST`、`HASH_LISTPACK`、`ZSET_LISTPACK`，除非后续实现精确 payload。

#### 4.2 Serializer

**新文件**：`src/persistence/rdb_serialize.h` / `src/persistence/rdb_serialize.cpp`

接口：

```cpp
class RdbSerializer {
 public:
  bool Save(const std::string& filepath, Server& server);
  bool SaveToString(std::string& out, Server& server);
};
```

流程：

1. 对每个 DB 调用 `PurgeExpiredKeys(now_ms)`。
2. 跳过空 DB。
3. 写 `SELECTDB`、`RESIZEDB`。
4. 通过 `Database::ForEachKey()` 写 key/value/expire。
5. 写 EOF 和 CRC64。
6. 先写临时文件，再 `fsync` + `rename`，避免中途崩溃破坏旧快照。

#### 4.3 Deserializer

**新文件**：`src/persistence/rdb_deserialize.h` / `src/persistence/rdb_deserialize.cpp`

接口：

```cpp
class RdbDeserializer {
 public:
  bool Load(const std::string& filepath, Server& server);
};
```

首版必须支持：

- 自己写出的 `STRING` / `LIST` / `SET` / `HASH` / `ZSET_2`。
- `SELECTDB`、`RESIZEDB`、`EXPIRETIME_MS`、`AUX`、`EOF`、CRC64。
- 过期时间已小于 `now_ms` 的 key 不恢复。

后续扩展：

- `SET_INTSET` 可用 `Intset::FromBytes()`。
- `HASH_LISTPACK` / `ZSET_LISTPACK` 可用 `Listpack::FromBytes()`。
- `LIST_QUICKLIST_2` 需要 Quicklist RDB payload 支持，单独实现和测试。

#### 4.4 SAVE / LASTSAVE

**文件**：`src/commands/server_commands.cpp`

新增：

- `SAVE`：同步保存，成功后 `ResetDirty()` 并更新 `last_save_ms`。
- `LASTSAVE`：返回上次成功保存的 Unix 秒时间戳。

`BGSAVE` 暂不注册；如果为了兼容性必须注册，首版返回明确错误：`ERR BGSAVE is not supported in this build`，并在测试中固定。

#### 4.5 启动加载

**文件**：`src/main.cpp`

启动顺序：

1. 解析配置。
2. `server.Init(config)`。
3. 如果 `appendonly == yes`，优先加载 AOF。
4. 否则如果 RDB 文件存在，加载 RDB。
5. 创建 registry，启动线程。

RDB 加载必须在线程启动前完成。

测试：

- empty DB 保存/加载。
- 五种 value round-trip。
- TTL round-trip，包括已过期 key 不恢复。
- 临时文件 rename 路径。
- CRC 错误拒绝加载。
- `SAVE` 成功更新 `LASTSAVE` 和 dirty。

---

### 阶段 5：AOF 持久化

**目标**：实现 RESP 命令追加、flush 策略和启动重放。

#### 5.1 AOF 文件层

**新文件**：`src/persistence/aof.h` / `src/persistence/aof.cpp`

接口：

```cpp
class AofWriter {
 public:
  bool Open(const std::string& filepath, std::string appendfsync);
  bool AppendCommand(const std::vector<std::string>& args);
  bool FlushIfNeeded(int64_t now_ms);
  void Close();
};

class AofReader {
 public:
  bool ReadCommands(const std::string& filepath,
                    std::vector<std::vector<std::string>>& out);
};
```

要求：

- `AppendCommand()` 写 RESP array of bulk strings。
- `appendfsync always`：每次 append 后 fsync。
- `appendfsync everysec`：由 serverCron 每秒 fsync。
- `appendfsync no`：只依赖 OS。
- `AofReader` 只解析命令，不依赖 `CommandRegistry`。

#### 5.2 命令传播

**文件**：`src/commands/dispatcher.cpp`、`src/io/client_session.h`

执行流程：

1. `ExecuteCommandDetailed()` 查 registry 和 arity。
2. 对写命令执行 OOM preflight。
3. 调用 handler。
4. 如果 `result.ok && result.mutated && cmd.flags & kWrite && !replay_mode`：
   - `AofWriter::AppendCommand(result.propagate_args.empty() ? args : result.propagate_args)`
   - `server.IncrementDirty()`

注意：

- `EXPIRE` / `PEXPIRE` / `EXPIREAT` / `PEXPIREAT` 可以传播原命令。
- `SETEX` / `PSETEX` 可传播原命令，或规范化为 `SET` + `PEXPIREAT`，但必须统一测试。
- `GETEX` 当前注册为 read-only，但它可能修改 TTL；需要在阶段 0/5 修正 flags 和 mutated 语义。
- replay 期间不写回 AOF，不递增 dirty。

#### 5.3 AOF 启动重放

**文件**：`src/main.cpp`

流程：

1. 创建 registry。
2. `AofReader` 读取命令数组。
3. 创建 fd 为 `-1` 的伪 client 或独立 replay client。
4. 逐条选择对应 DB 并执行 `ExecuteCommandDetailed(..., replay_mode=true)`。
5. 任一命令 replay 失败，启动失败并报告错误。

#### 5.4 AOF rewrite

首版不实现后台 rewrite。可新增同步 `AofWriter::RewriteFromSnapshot(Server&)` 作为测试辅助，但不注册 `BGREWRITEAOF`。

rewrite 生成规则：

- 每个非空 DB 先写 `SELECT db`。
- string -> `SET key value`
- list -> `RPUSH key ...`
- set -> `SADD key ...`
- hash -> `HSET key field value ...`
- zset -> `ZADD key score member ...`
- TTL -> `PEXPIREAT key expire_at_ms`

测试：

- AOF append 后文件内容为合法 RESP。
- 成功写命令被记录，失败写命令不记录。
- 条件未命中的写命令不记录或 dirty 为 0。
- replay 恢复五种类型和 TTL。
- replay 期间不产生二次 AOF。
- `appendfsync` 配置解析和 flush 调用可观察。

---

### 阶段 6：CONFIG、INFO 和集成

#### 6.1 CONFIG GET/SET

**文件**：`src/commands/server_commands.cpp`

支持：

- `CONFIG GET <pattern>`：首版可先支持精确 key 和 `*`。
- `CONFIG SET maxmemory <bytes>`
- `CONFIG SET maxmemory-policy <policy>`
- `CONFIG SET maxmemory-samples <n>`
- `CONFIG SET appendonly yes|no`
- `CONFIG SET appendfsync always|everysec|no`
- `CONFIG SET dbfilename <file>`
- `CONFIG SET appendfilename <file>`

要求：

- `CONFIG SET` 成功后通过 `Server::ApplyConfig()` 更新运行时配置。
- `appendonly yes` 需要打开 AOF；`appendonly no` 需要 flush 后关闭 AOF。
- 不支持的 key 返回错误，不再静默放入 `extras_` 后让用户以为已生效。

#### 6.2 INFO

扩展 `INFO` 输出：

- `# Stats`
- `keyspace_hits`
- `keyspace_misses`
- `expired_keys`
- `evicted_keys`
- `# Persistence`
- `rdb_last_save_time`
- `aof_enabled`
- `aof_current_size`
- `# Memory`
- `used_memory`
- `maxmemory`
- `maxmemory_policy`

#### 6.3 main 集成

**文件**：`src/main.cpp`

启动后：

```cpp
io_ctx.ArmPeriodicTimer(1000 / config.hz, [&](uint64_t) {
  PostFunction(cmd_ctx, [&] {
    ServerCron(server, cron_services, NowMs());
  });
});
```

关闭前：

1. 停止 IO timer。
2. 等待 scope 清空。
3. 在 CMD 线程停止前 flush AOF。
4. 停止 CMD。
5. 关闭 fd 和持久化文件。

---

## 文件变更清单

### 新增文件

```
src/persistence/
  crc64.h
  crc64.cpp
  rdb_format.h
  rdb_serialize.h
  rdb_serialize.cpp
  rdb_deserialize.h
  rdb_deserialize.cpp
  aof.h
  aof.cpp

src/eviction/
  expire.h
  expire.cpp
  evict.h
  evict.cpp
  server_cron.h
  server_cron.cpp

tests/persistence/
  crc64_test.cpp
  rdb_test.cpp
  aof_test.cpp

tests/eviction/
  expire_test.cpp
  evict_test.cpp
```

### 修改文件

```
CMakeLists.txt
tests/CMakeLists.txt
src/ds/dict.h
tests/ds/dict_test.cpp
src/types/value.h
src/types/value.cpp
src/core/config.h
src/core/config.cpp
src/core/database.h
src/core/database.cpp
src/core/server.h
src/core/server.cpp
tests/core/config_test.cpp
tests/core/database_test.cpp
tests/core/server_test.cpp
src/commands/command_context.h
src/commands/dispatcher.h
src/commands/dispatcher.cpp
src/commands/server_commands.cpp
tests/commands/dispatcher_test.cpp
tests/commands/server_commands_test.cpp
src/io/epoll_context.h
src/io/epoll_context.cpp
src/io/cmd_context.h
src/io/client_session.h
tests/io/epoll_context_test.cpp
tests/io/cmd_context_test.cpp
src/main.cpp
```

---

## 验收测试计划

### 阶段内测试

| 阶段 | 目标命令 |
|------|----------|
| 基础 API | `cmake --build build --target miniredis_ds_test miniredis_types_test miniredis_core_test` |
| 命令传播 | `cmake --build build --target miniredis_commands_test` |
| IO timer | `cmake --build build --target miniredis_io_test` |
| 持久化 | `cmake --build build --target miniredis_persistence_test` |
| 淘汰/过期 | `cmake --build build --target miniredis_eviction_test` |

### 全量验证

```bash
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -N
git diff --check
clang-format --dry-run --Werror -style=Google \
  src/persistence/*.h src/persistence/*.cpp \
  src/eviction/*.h src/eviction/*.cpp \
  src/core/*.h src/core/*.cpp \
  src/commands/*.h src/commands/*.cpp \
  src/io/*.h src/io/*.cpp \
  tests/persistence/*.cpp tests/eviction/*.cpp
```

如果 repo-wide `format-check` 因历史文件失败，使用上述 touched-file `clang-format` 结果作为本阶段门槛。

### 端到端验证

1. `SET key value` -> `SAVE` -> 重启 -> `GET key` 返回 `value`。
2. 五种类型写入 -> RDB round-trip -> 类型、内容、TTL 一致。
3. 启用 AOF -> 写命令 -> 重启 -> 数据恢复。
4. 失败写命令不进入 AOF。
5. `PEXPIRE key 100` -> 等待 -> serverCron 主动删除 key。
6. `CONFIG SET maxmemory <small>` + `allkeys-lru` -> 写入大 key -> 触发淘汰并更新 `INFO stats`。
7. `CONFIG SET maxmemory-policy volatile-lru` -> 只淘汰带 TTL 的 key。
8. `CONFIG SET appendonly yes/no` 能打开/关闭 AOF writer。

### Redis 互操作验收（后续增强 gate）

1. `redis-check-rdb` 能验证 MiniRedis 生成的 RDB。
2. Redis 能加载 MiniRedis 生成的 RDB，并读出五种类型。
3. MiniRedis 能加载 Redis 生成的通用类型 RDB。
4. MiniRedis 能加载 Redis 现代紧凑编码 RDB：`SET_INTSET`、`HASH_LISTPACK`、`ZSET_LISTPACK`、`LIST_QUICKLIST_2`。

---

## 延期项

1. `BGSAVE`：需要先设计 fork 安全策略。可选方向是停止/隔离其他线程后 fork，或引入子进程快照协议；不能简单在多线程进程的子进程中执行复杂 C++ 序列化。
2. `BGREWRITEAOF`：需要后台 rewrite 期间的增量缓冲、rename 原子切换和 replay 一致性测试。
3. 精确 Redis RDB 紧凑编码：`LIST_QUICKLIST_2`、`HASH_LISTPACK`、`ZSET_LISTPACK`、`SET_LISTPACK`。
4. 更多淘汰策略：`allkeys-random`、`volatile-random`、`volatile-ttl`、LFU。
5. 更精确的内存统计：allocator overhead、container bucket overhead、client buffer memory。

---

## 实现注意事项

1. **线程边界**：timer callback 在 IO 线程，数据库操作必须投递到 CMD 线程。
2. **move-only Value**：RDB/AOF/rewrite 必须通过遍历和序列化读取 value，不得启用隐式复制。
3. **TTL 单位**：`Database::TTL()` 返回毫秒；命令层 `TTL` 秒级转换不能影响持久化内部的毫秒过期时间。
4. **AOF 传播**：只传播成功且实际 mutation 的写命令；replay 模式禁用传播。
5. **OOM 策略**：`noeviction` 下不能让失败写命令留下部分状态；必要时先拒绝高风险写命令。
6. **RDB 兼容**：不要把 MiniRedis 的逻辑元素列表写到 Redis 现代紧凑 type byte 下；type byte 和 payload 必须匹配。
7. **原子文件写入**：RDB/AOF rewrite 均使用临时文件、fsync、rename。
8. **测试时间**：过期和 fsync 测试尽量注入 `now_ms`，避免依赖不稳定 sleep。
