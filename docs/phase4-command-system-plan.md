# MiniRedis Phase 4: 命令系统实现计划

> 状态: 已审查并修订 | 日期: 2026-06-28

## 0. 审查结论

原计划方向正确，但需要先收窄和补强，否则实现时会在注册机制、Server 访问、TTL、命令选项和测试边界上反复返工。

本版计划做如下修正：

- 将 Phase 4 从“直接实现约 100 个命令”改为“先落地命令框架，再按能力分批实现兼容子集”。
- 命令函数不只接收 `Client&` 和 `Database&`，还必须能访问 `Server&`，否则 `SELECT`、`FLUSHALL`、`DBSIZE`、配置阈值和跨 DB 语义都没有可靠入口。
- 不使用静态注册宏作为主路径。改为显式 `RegisterAllCommands()`，避免静态初始化顺序和静态库链接裁剪问题。
- `arity` 只负责基础参数数量；偶数参数、子命令、选项矩阵、score/range 解析由命令内 helper 明确校验。
- 明确哪些命令是 Phase 4 必做，哪些因当前 API 不足而延期，避免把“不支持”伪装成简化语义。
- 所有 RESP 回复通过现有 `RespReply` 构造；新增的错误、数组嵌套、score 格式化 helper 放在 command 层。

---

## 1. Phase 4 目标与非目标

### 1.1 目标

Phase 4 实现 MiniRedis 的纯逻辑命令层：

```
RESP command array
  -> CommandDispatcher::Execute(context, args)
  -> CommandRegistry lookup
  -> command function
  -> RESP reply string
```

命令层输入是 `std::vector<std::string>`，即 `RespParser::TakeCommand()` 已解析出的命令数组；输出是 RESP2 字节串，例如 `+OK\r\n`、`:1\r\n`、`$5\r\nhello\r\n`。

Phase 4 结束时应具备：

- 可测试的命令注册、查找、参数校验和分发框架。
- Server/Key/String/List/Set/Hash/ZSet 的高价值命令子集。
- 明确的 RESP 错误映射、数字解析规则和类型错误处理。
- 和现有 `Database`、`Client`、`Server`、Phase 2 value API 对齐的测试覆盖。

### 1.2 非目标

- 不实现 socket I/O、event loop、真实网络连接；这些留给 Phase 5。
- 不实现 RESP3。
- 不追求完整 Redis 命令矩阵和所有选项。
- 不实现 persistence、replication、ACL、transactions、pub/sub、Lua。
- 不为当前 value API 不支持的命令临时塞入大规模重构。

---

## 2. 当前代码边界

Phase 4 必须尊重已有模块能力：

| 模块 | 已有能力 | 对命令层的约束 |
|------|----------|----------------|
| `RespParser` | 解析 RESP2 命令数组，输出 `vector<string>` | command 层不重新解析 RESP，只处理参数数组 |
| `RespReply` | Simple string、error、integer、bulk string、array helper | command 层复用这些 helper，必要时新增 command-local 组合 helper |
| `Database` | `Find/Set/Delete/Exists/Type/Rename/Keys/RandomKey/TTL/SetExpire/Persist/Clear` | TTL 单位是毫秒；`Keys()` 目前只支持 `*` |
| `Client` | 当前 DB、reply buffer、auth/name 状态 | `SELECT` 通过 `Client::SelectDb()` 修改 DB |
| `Server` | DB 容器、client 管理、配置阈值、flush | 命令创建 Set/Hash/ZSet 时必须使用 `Server::GetEncodingThresholds()` |
| Phase 2 values | 各类型的核心操作 | 命令选项必须受这些 API 能力限制 |

`Value` 是 move-only variant。任何需要复制完整 value 的命令，例如 `COPY`，必须先有显式 clone 能力，否则延期。

---

## 3. 架构设计

### 3.1 文件结构

```
src/commands/
├── command_context.h          # CommandContext
├── dispatcher.h/cpp           # ExecuteCommand + arity/unknown command
├── registry.h/cpp             # CommandRegistry + RegisterAllCommands()
├── command_helpers.h/cpp      # 类型获取、解析、RESP 组合、错误映射
├── string_commands.h/cpp
├── list_commands.h/cpp
├── set_commands.h/cpp
├── hash_commands.h/cpp
├── zset_commands.h/cpp
├── key_commands.h/cpp
└── server_commands.h/cpp

tests/commands/
├── command_test_util.h
├── registry_test.cpp
├── dispatcher_test.cpp
├── string_commands_test.cpp
├── list_commands_test.cpp
├── set_commands_test.cpp
├── hash_commands_test.cpp
├── zset_commands_test.cpp
├── key_commands_test.cpp
└── server_commands_test.cpp
```

### 3.2 CommandContext

```cpp
struct CommandContext {
  Server& server;
  Client& client;
  Database& db;
};
```

`db` 表示执行当前命令时 `client.CurrentDb()` 对应的数据库。`SELECT` 修改 `client` 后不需要替换本次 `db` 引用，因为 Redis 语义也是从下一条命令开始使用新 DB。

测试中使用统一 harness：

```cpp
struct CommandTestHarness {
  Server& server = Server::Instance();
  Client client{1};

  CommandTestHarness();
  CommandContext Context();
  std::string Call(std::vector<std::string> args);
};
```

每个测试构造时调用 `server.Init(default_config)`，保证单例 Server 的状态隔离。

### 3.3 CommandInfo

```cpp
enum class CommandFlag : uint32_t {
  kReadOnly = 1u << 0,
  kWrite = 1u << 1,
  kAdmin = 1u << 2,
  kMayCreateKey = 1u << 3,
};

struct CommandInfo {
  using Func = std::string (*)(CommandContext&,
                               const std::vector<std::string>&);

  std::string name;       // uppercase ASCII
  int arity;              // >0 exact argc, <0 minimum argc, includes command
  uint32_t flags;
  Func func;
};
```

`std::function` 不是必须的。命令函数不捕获状态，函数指针更简单，也避免额外分配。

### 3.4 CommandRegistry

```cpp
class CommandRegistry {
 public:
  void Register(CommandInfo info);
  const CommandInfo* Find(std::string_view name) const;
  size_t Size() const;
  std::vector<std::string> CommandNames() const;

 private:
  std::unordered_map<std::string, CommandInfo> commands_;
};

CommandRegistry CreateDefaultCommandRegistry();
void RegisterAllCommands(CommandRegistry& registry);
```

命令名查找规则：

- 只对命令名做 ASCII uppercase 归一化。
- key、value、member、field 保持原始字节，不做大小写转换。
- 不使用 locale 相关 API。
- 重复注册同名命令应触发测试失败；实现可 `assert` 或返回 `bool`。

### 3.5 显式注册

每个命令族提供显式注册函数：

```cpp
void RegisterStringCommands(CommandRegistry& registry);
void RegisterListCommands(CommandRegistry& registry);
void RegisterSetCommands(CommandRegistry& registry);
void RegisterHashCommands(CommandRegistry& registry);
void RegisterZSetCommands(CommandRegistry& registry);
void RegisterKeyCommands(CommandRegistry& registry);
void RegisterServerCommands(CommandRegistry& registry);
```

`RegisterAllCommands()` 固定顺序调用这些函数：

```
Server -> Key -> String -> List -> Set -> Hash -> ZSet
```

不采用静态对象自动注册作为主机制。若以后为了书写方便增加宏，也只能展开成普通注册表项，不能依赖全局构造副作用。

### 3.6 Dispatcher

```cpp
std::string ExecuteCommand(CommandRegistry& registry,
                           CommandContext& context,
                           const std::vector<std::string>& args);
```

分发流程：

1. `args.empty()` 返回 `-ERR empty command\r\n`。
2. 归一化 `args[0]` 后查 registry。
3. 未找到返回 `RespReply::UnknownCommand(args[0])`。
4. 执行基础 arity 校验。
5. 调用命令函数。

基础 arity 规则：

```cpp
arity > 0: args.size() == arity
arity < 0: args.size() >= -arity
```

复杂规则在命令内处理，例如：

- `MSET` 参数必须是 key/value 成对。
- `HSET` 参数必须是 field/value 成对。
- `CONFIG GET` 和 `CONFIG SET` 通过 `args[1]` 子命令分派。
- `ZADD` 需要先解析选项，再验证 score/member 成对。
- `LRANGE`、`ZRANGE`、`SCAN` 需要各自解析整数、范围和可选项。

---

## 4. Command Helpers

### 4.1 类型获取

```cpp
template <typename T>
T* GetValueAs(Database& db, std::string_view key);

template <typename T>
const T* GetValueAs(const Value& value);
```

约定：

- key 不存在返回 `nullptr`。
- key 存在但类型错误时，命令直接返回 `RespReply::WrongType()`。
- 不要把“不存在”和“类型错误”混在同一个 helper 返回值里；命令需要区分这两种情况。

推荐写法：

```cpp
Value* value = db.Find(key);
if (!value) return RespReply::Nil();
auto* string_value = std::get_if<StringValue>(value);
if (!string_value) return RespReply::WrongType();
```

### 4.2 创建聚合类型

创建 Set/Hash/ZSet 时必须使用 Server 配置阈值：

```cpp
SetValue MakeSetValue(CommandContext& ctx);
HashValue MakeHashValue(CommandContext& ctx);
ZSetValue MakeZSetValue(CommandContext& ctx);
```

创建或获取命令遵守规则：

- key 不存在时创建目标类型。
- key 存在且类型匹配时复用。
- key 存在但类型不匹配时返回 WRONGTYPE。
- 对 list/set/hash/zset 执行删除类操作后，如果集合为空，应删除 key，保持 Redis 风格的空聚合清理语义。

### 4.3 数字解析

命令层先复用 Phase 2 的数字工具：

- `ParseCanonicalInt()`：用于整数参数。
- `ParseFiniteDouble()`：用于浮点参数和 score。
- `AddChecked()` / `AddFiniteDouble()`：用于增量操作。
- `FormatDoubleForStorage()`：用于 score 和浮点回复。

注意：`ParseCanonicalInt()` 比 Redis 更严格，会拒绝 `+1`、`001`、`-0`。Phase 4 先保持与当前类型系统一致。如果后续要提高 Redis CLI 兼容性，应新增 `ParseRedisIntegerArgument()`，不要修改 Phase 2 的 canonical 存储规则。

### 4.4 错误回复

新增 command 层错误 helper：

```cpp
std::string WrongArity(std::string_view command);
std::string SyntaxError();
std::string InvalidInteger();
std::string InvalidExpireTime();
std::string InvalidDbIndex();
std::string NotImplemented(std::string_view command);
std::string Unsupported(std::string_view feature);
std::string TypeErrorToResp(TypeError error);
```

错误映射：

| 错误 | RESP |
|------|------|
| unknown command | `-ERR unknown command '<cmd>'\r\n` |
| wrong arity | `-ERR wrong number of arguments for '<cmd>' command\r\n` |
| syntax error | `-ERR syntax error\r\n` |
| WRONGTYPE | 复用 `RespReply::WrongType()` |
| invalid integer | `-ERR value is not an integer or out of range\r\n` |
| integer overflow | `-ERR increment or decrement would overflow\r\n` |
| invalid float | `-ERR value is not a valid float\r\n` |
| invalid score | `-ERR score is not a valid float\r\n` |
| invalid expire | `-ERR invalid expire time\r\n` |
| db index invalid | `-ERR DB index is out of range\r\n` |
| unsupported option | `-ERR syntax error\r\n` |
| unsupported feature | `-ERR unsupported <feature>\r\n` |

### 4.5 RESP 数组组合

现有 `RespReply::ArrayOfBulkStrings()` 覆盖普通 bulk string 数组。命令层还需要：

```cpp
std::string ArrayOfNullBulkStrings(size_t count);
std::string ArrayOfOptionalBulkStrings(
    const std::vector<std::optional<std::string>>& values);
std::string ArrayOfZSetRange(const std::vector<ZSetValue::RangeResult>& values,
                             bool with_scores);
std::string ScanReply(size_t next_cursor,
                      const std::vector<std::string>& elements);
```

Set/Hash/Scan 类命令不保证返回顺序。测试应解析 RESP 后做集合比较，不依赖 `ds::Dict` 当前迭代顺序。

---

## 5. 命令范围

Phase 4 不再以“约 100 个命令一次完成”为成功标准。命令按 P0/P1/P2 分批验收。

### 5.1 P0: 框架、Server、Key、String 基础命令

P0 是最小可用命令层。

| 命令族 | 命令 | 说明 |
|--------|------|------|
| Server | `PING`, `ECHO`, `SELECT`, `DBSIZE`, `FLUSHDB`, `FLUSHALL`, `TIME`, `COMMAND`, `INFO` | `INFO` 返回简化 server/keyspace 信息 |
| Config | `CONFIG GET` | 只读，返回当前 `MiniRedisConfig` 已知字段 |
| Key | `DEL`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST`, `KEYS`, `RANDOMKEY`, `RENAME`, `RENAMENX` | `KEYS` 仅支持 `*`，其他 pattern 返回 `Unsupported("KEYS pattern")` |
| String | `GET`, `SET`, `SETNX`, `SETEX`, `PSETEX`, `GETSET`, `GETDEL`, `GETEX`, `APPEND`, `STRLEN`, `GETRANGE`, `SETRANGE`, `INCR`, `DECR`, `INCRBY`, `DECRBY`, `INCRBYFLOAT`, `MGET`, `MSET`, `MSETNX` | `SET` 初版仅支持 `SET key value`，复杂 `SET NX/XX/EX/PX/GET` 选项延期 |

P0 明确延期：

- `CONFIG SET`：当前 `Server` 只暴露 `const MiniRedisConfig&`，没有运行时配置变更 API。
- `COPY`：需要 move-only `Value` 的 clone 支持。
- `SORT`：需要排序规则和 BY/GET/STORE 选项设计。
- `UNLINK`：没有异步删除系统；可作为 `DEL` 别名实现，但不要声称异步。

### 5.2 P1: List、Set、Hash

| 命令族 | 命令 | 说明 |
|--------|------|------|
| List | `LPUSH`, `RPUSH`, `LPUSHX`, `RPUSHX`, `LPOP`, `RPOP`, `LLEN`, `LINDEX`, `LSET`, `LRANGE`, `LTRIM`, `LINSERT`, `LREM`, `LPOS`, `LMOVE`, `RPOPLPUSH` | `LPOS` 初版只支持 `LPOS key element`，复杂 `RANK/COUNT/MAXLEN` 延期 |
| Set | `SADD`, `SREM`, `SPOP`, `SMEMBERS`, `SCARD`, `SISMEMBER`, `SRANDMEMBER`, `SMOVE`, `SUNION`, `SINTER`, `SDIFF`, `SUNIONSTORE`, `SINTERSTORE`, `SDIFFSTORE`, `SSCAN` | 多集合操作通过现有二元 API fold 实现 |
| Hash | `HSET`, `HSETNX`, `HGET`, `HMGET`, `HDEL`, `HLEN`, `HSTRLEN`, `HEXISTS`, `HKEYS`, `HVALS`, `HGETALL`, `HINCRBY`, `HINCRBYFLOAT`, `HSCAN`, `HRANDFIELD` | `HRANDFIELD` 初版只支持返回一个 field |

P1 简化规则：

- `SPOP key [count]` 和 `SRANDMEMBER key [count]` 可以先只支持无 count 形式；若传 count 但未实现，返回 syntax error。
- `SSCAN/HSCAN` 返回 RESP 形态 `*2\r\n$<cursor>\r\n...\r\n*<items>\r\n...`；cursor 简化为基于 vector 下标的游标，遍历完成返回 `"0"`。
- 删除后集合为空时删除 key。

### 5.3 P2: ZSet

| 命令 | 说明 |
|------|------|
| `ZADD` | 支持基础 score/member；`NX/XX/CH/INCR` 可分步加入，必须有选项测试 |
| `ZREM`, `ZCARD`, `ZCOUNT`, `ZSCORE`, `ZRANK`, `ZREVRANK`, `ZINCRBY` | 直接映射到 `ZSetValue` |
| `ZRANGE`, `ZREVRANGE` | 支持 rank range 和 `WITHSCORES` |
| `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE` | 支持 `min max [WITHSCORES] [LIMIT offset count]` |
| `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZLEXCOUNT` | 使用现有 lex API |
| `ZREMRANGEBYRANK`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYLEX` | 删除后空 zset 删除 key |
| `ZPOPMIN`, `ZPOPMAX` | 初版支持无 count 或 count 循环 |
| `ZSCAN` | 同 scan 简化语义 |

P2 延期或谨慎项：

- `ZUNION`, `ZINTER`, `ZUNIONSTORE`, `ZINTERSTORE` 只有在 `ZSetValue::Union/Intersect` 行为被单元测试覆盖清楚后再实现。它们涉及权重、聚合策略、结果编码和返回格式，不能只做表面注册。
- Redis 6.2 之后统一的 `ZRANGE BYSCORE/BYLEX/REV/LIMIT` 语法可延期；先保留旧命令族。

---

## 6. 关键语义约定

### 6.1 TTL

`Database::TTL()` 返回毫秒：

- `TTL key` 将毫秒转换为秒。建议使用向上取整 `(ms + 999) / 1000`，避免刚设置的 1 秒 TTL 立刻显示 0。
- `PTTL key` 直接返回毫秒。
- key 不存在返回 `-2`。
- key 存在但无过期返回 `-1`。

过期命令：

- `EXPIRE key seconds` 和 `PEXPIRE key milliseconds` 使用相对时间。
- `EXPIREAT key unix_seconds` 和 `PEXPIREAT key unix_milliseconds` 使用绝对时间戳。
- 过期时间小于等于当前时间时，可以调用 `SetExpire()` 后通过下一次访问惰性删除，也可以命令内直接 `Delete()`；测试只要求之后 `EXISTS` 为 0。
- 解析溢出或负数返回 `invalid expire time`。

### 6.2 key 类型

`TYPE` 返回：

| ValueType | 回复 |
|-----------|------|
| missing | `+none\r\n` |
| string | `+string\r\n` |
| list | `+list\r\n` |
| set | `+set\r\n` |
| hash | `+hash\r\n` |
| zset | `+zset\r\n` |

### 6.3 不存在 key 的读语义

常见规则：

- string/list/hash/zset 单元素读：返回 nil。
- `LLEN/SCARD/HLEN/ZCARD`：返回 0。
- 集合枚举：返回空数组。
- 删除类命令：返回 0。
- `INCR/HINCRBY/ZINCRBY`：不存在时按 0 创建。

### 6.4 写命令与 TTL

已有 `Database::Set()` 会清除 TTL。命令层应遵守：

- `SET`、`GETSET`、`MSET` 覆盖 key 时清除旧 TTL。
- `SETEX/PSETEX` 先 `Set()` 再 `SetExpire()`。
- in-place 修改，例如 `APPEND`、`INCR`、`LPUSH`、`HSET`，不应清除已有 TTL。
- `RENAME` 保留源 key TTL，这已由 `Database::Rename()` 实现。

---

## 7. 构建集成

`CMakeLists.txt` 新增：

```cmake
add_library(miniredis_commands STATIC
    src/commands/registry.cpp
    src/commands/dispatcher.cpp
    src/commands/command_helpers.cpp
    src/commands/string_commands.cpp
    src/commands/list_commands.cpp
    src/commands/set_commands.cpp
    src/commands/hash_commands.cpp
    src/commands/zset_commands.cpp
    src/commands/key_commands.cpp
    src/commands/server_commands.cpp
)
target_include_directories(miniredis_commands PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis_commands PUBLIC miniredis_core)
target_compile_features(miniredis_commands PUBLIC cxx_std_20)
```

`miniredis` 可先链接 `miniredis_commands`，即使 Phase 5 才真正接入网络执行：

```cmake
target_link_libraries(miniredis PRIVATE
    miniredis_commands miniredis_core miniredis_types miniredis_ds stdexec pthread)
```

`tests/CMakeLists.txt` 新增独立命令测试二进制：

```cmake
add_executable(miniredis_commands_test
    commands/registry_test.cpp
    commands/dispatcher_test.cpp
    commands/string_commands_test.cpp
    commands/list_commands_test.cpp
    commands/set_commands_test.cpp
    commands/hash_commands_test.cpp
    commands/zset_commands_test.cpp
    commands/key_commands_test.cpp
    commands/server_commands_test.cpp
)
target_link_libraries(miniredis_commands_test
    PRIVATE miniredis_commands GTest::gtest_main)
target_compile_features(miniredis_commands_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_commands_test)
```

---

## 8. 实现顺序

### Step 1: 框架

- 新增 `CommandContext`。
- 新增 `CommandInfo`、`CommandRegistry`、`CreateDefaultCommandRegistry()`。
- 新增 `ExecuteCommand()`。
- 测试 unknown command、大小写命令名、空命令、wrong arity、重复注册。

### Step 2: helpers

- 类型获取 helper。
- 数字解析和 `TypeError` 映射。
- RESP optional array、zset range、scan reply helper。
- 测试每个 helper 的边界。

### Step 3: Server commands

实现 `PING/ECHO/SELECT/DBSIZE/FLUSHDB/FLUSHALL/TIME/COMMAND/INFO/CONFIG GET`。

重点测试：

- `SELECT` 越界。
- `FLUSHDB` 只清当前 DB。
- `FLUSHALL` 清所有 DB。
- `COMMAND` 包含已注册命令名。

### Step 4: Key commands

实现 P0 key 命令。

重点测试：

- TTL 秒/毫秒转换。
- expired key 的惰性删除。
- `RENAME` 保留 TTL。
- `KEYS *` 返回当前 keyspace；非 `*` 返回 unsupported error。
- `RANDOMKEY` 只断言返回存在 key，不断言具体 key。

### Step 5: String commands

实现 P0 string 命令。

重点测试：

- 不存在 key。
- WRONGTYPE。
- `MSET/MSETNX` 参数成对。
- `INCRBY` 溢出。
- `INCRBYFLOAT` 非有限浮点拒绝。
- `SETEX/PSETEX` 设置 TTL。
- in-place string 修改不清除 TTL。

### Step 6: List/Set/Hash

按 List -> Set -> Hash 顺序实现 P1。

重点测试：

- 空集合删除 key。
- 范围和负索引。
- 多集合 fold 行为。
- Hash 多 field/value 参数成对。
- 无序结果使用无序断言。

### Step 7: ZSet

实现 P2 中不依赖额外重构的 zset 命令。

重点测试：

- score 解析与格式化。
- `WITHSCORES` 返回交错 bulk string。
- rank/range 边界。
- lex range 语法。
- 删除后空 zset 删除 key。

---

## 9. 测试策略

### 9.1 单元测试分层

| 测试 | 覆盖 |
|------|------|
| `registry_test.cpp` | 注册、重复名、大小写查找、命令列表 |
| `dispatcher_test.cpp` | unknown、empty、arity、调用顺序 |
| `*_commands_test.cpp` | 每个命令族的成功路径和错误路径 |
| `command_helpers` 覆盖 | 类型获取、解析、RESP 组合 |

### 9.2 每个命令至少覆盖

- 成功路径。
- wrong arity。
- key 不存在语义。
- wrong type。
- 参数解析失败。
- 对 DB 状态的实际影响。

### 9.3 RESP 断言

简单回复直接比较字符串：

```cpp
EXPECT_EQ(Call({"SET", "k", "v"}), "+OK\r\n");
EXPECT_EQ(Call({"GET", "k"}), "$1\r\nv\r\n");
```

数组回复建议提供测试解析 helper，避免无序集合因为迭代顺序导致 flaky：

```cpp
EXPECT_THAT(ParseBulkArray(Call({"SMEMBERS", "s"})),
            UnorderedElementsAre("a", "b"));
```

### 9.4 回归测试清单

- `SET` 覆盖带 TTL 的 key 后 TTL 被清除。
- `APPEND/INCR/HSET/LPUSH/SADD/ZADD` 修改带 TTL 的 key 后 TTL 保留。
- 过期 key 被所有读写命令视为不存在。
- `MSETNX` 只要任意 key 存在就不写入任何 key。
- `RENAMENX` 目标存在时不修改源 key。
- `SELECT` 后下一条命令使用新 DB。

---

## 10. 成功标准

Phase 4 完成标准：

- [ ] `miniredis_commands` 可独立构建并链接到测试。
- [ ] Dispatcher 和 Registry 单元测试通过。
- [ ] P0 命令全部实现并测试通过。
- [ ] P1 命令全部实现并测试通过，注明简化选项。
- [ ] P2 中不延期的 ZSet 命令实现并测试通过。
- [ ] 延期命令有清单，不在 registry 中静默注册成假实现。
- [ ] `cmake --build build` 成功。
- [ ] `ctest --test-dir build --output-on-failure` 全绿。
- [ ] `git diff --check` 无 whitespace error。

---

## 11. 延期清单

以下命令或选项不作为 Phase 4 必须完成项：

| 项目 | 延期原因 |
|------|----------|
| `COPY` | `Value` move-only，缺少 clone API |
| `SORT` | 需要完整排序和 STORE/BY/GET 设计 |
| `CONFIG SET` | 当前 Server/config 无运行时 mutation API |
| 完整 glob `KEYS pattern` | `Database::Keys()` 当前只支持 `*` |
| 完整 `SCAN/SSCAN/HSCAN/ZSCAN` cursor 语义 | 当前 Phase 1/2 API 只适合简化扫描 |
| `LPOS` 复杂选项 | `ListValue::Find()` 只提供首个位置 |
| `HRANDFIELD` count/withvalues | `HashValue::RandomField()` 只返回单 field |
| `SRANDMEMBER`/`SPOP` 完整 count 语义 | 需要明确随机性、去重和负 count 规则 |
| `ZUNION/ZINTER/ZUNIONSTORE/ZINTERSTORE` | 需要先补足并验证 zset 聚合语义 |
| 新式 `ZRANGE BYSCORE/BYLEX/REV/LIMIT` | 可由旧命令族覆盖主要能力 |

延期项如果被调用，优先选择“不注册 -> unknown command”。只有为了兼容测试需要显式暴露时，才注册并返回 `NotImplemented()` 或 `Unsupported()`，且必须有测试说明。
