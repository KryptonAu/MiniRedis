# MiniRedis Phase 3: 核心运行时实现计划

> 状态: 已根据审查意见修订 | 日期: 2026-06-27

## 概述

Phase 3 构建 MiniRedis 的核心运行时层——协议解析、配置管理、数据库键空间、客户端状态和 Server 骨架。本阶段不涉及 socket I/O 和命令实现，只定义数据流经的管道、状态容器和后续命令层依赖的明确语义。

核心原则：**Phase 3 每个模块都是纯逻辑层，可脱离 I/O 独立测试。**

本阶段的边界：
- RESP 入站只实现服务端命令帧解析，不实现通用 RESP AST。
- Database 必须定义惰性过期语义，所有 key 访问对已过期 key 表现为不存在。
- Server 不保存隐式 "current client"，命令层必须显式传入 Client 或数据库索引。
- Config 只负责保存和验证阈值，命令层创建新 Value 时从 Server 读取阈值。

---

## 1. 模块依赖

```
RESP Protocol ── Client
Config ───────── Server ── (Phase 4 Commands)
Database ───────┘
```

- **RESP Protocol**: 无内部依赖，负责 RESP2 命令帧解析和 Reply 构造
- **Config**: 仅依赖 `EncodingThresholds`
- **Client**: 依赖 RESP parser，保存连接状态和缓冲区
- **Database**: 依赖 `Value`（Phase 2）和 `ds::Dict`（Phase 1）
- **Server**: 组合 `Database`、`MiniRedisConfig`、`Client` 管理

命名空间约定：所有 Phase 3 类型放在 `namespace miniredis`，使用 Phase 1 字典时显式写 `ds::Dict<...>`。

---

## 2. 文件结构

```
src/core/
├── resp_protocol.h/cpp     # RESP2 协议解析器 + Reply 构造器
├── config.h/cpp            # redis.conf 解析 + 运行时配置
├── database.h/cpp          # Database — 键空间 + 过期
├── client.h/cpp            # Client — 连接状态
└── server.h/cpp            # Server — 单例，协调所有子系统

tests/core/
├── resp_protocol_test.cpp
├── config_test.cpp
├── database_test.cpp
├── client_test.cpp
└── server_test.cpp
```

---

## 3. RESP Protocol

**文件**: `src/core/resp_protocol.h` + `resp_protocol.cpp`

### 3.1 设计要点

目标是兼容 redis-cli 发送的 RESP2 命令帧和 Redis 常用回复格式。Phase 3 的解析器不是通用 RESP 数据模型：入站只接受命令数组，出站由 `RespReply` 构造 RESP2 字节串。

RESP2 回复支持五种类型：

| 类型 | 前缀 | 示例 |
|------|------|------|
| Simple String | `+` | `+OK\r\n` |
| Error | `-` | `-ERR unknown command\r\n` |
| Integer | `:` | `:1000\r\n` |
| Bulk String | `$` | `$5\r\nhello\r\n` |
| Array | `*` | `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n` |

### 3.2 解析器 (RespParser)

**设计**：流式命令解析器，支持 partial read 和 pipelining。`Feed()` 只追加新数据并尽可能解析完整命令；完整命令进入内部队列，未消费的后续字节留在缓冲区。

```cpp
enum class ParseStatus {
    kComplete,    // 本次 Feed 至少解析出一个完整命令
    kIncomplete,  // 数据有效，但还需要更多字节
    kError,       // 协议错误，连接层应返回错误并关闭或 Reset
};

class RespParser {
public:
    RespParser();

    // 追加数据并解析。若一次 Feed 含多条 pipelined 命令，全部入队。
    ParseStatus Feed(std::string_view data);

    bool HasCommand() const;
    size_t PendingCommandCount() const;

    // 取出最早完成的一条命令。仅在 HasCommand() 为 true 时调用。
    std::vector<std::string> TakeCommand();  // [cmd, arg1, arg2, ...]

    // 协议错误或连接复用测试时调用；清空 buffer、命令队列和错误状态。
    void Reset();

    std::optional<std::string_view> LastError() const;
    size_t BufferSize() const;

private:
    std::vector<uint8_t> buffer_;
    std::deque<std::vector<std::string>> ready_commands_;
    std::string error_;
};
```

**解析流程**：
```
Feed("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n")
  → 读取 '*' → 期待 array
  → 读取 '3' → array 长度 = 3
  → 读取 "$3\r\nSET\r\n" → bulk string "SET"
  → 读取 "$3\r\nkey\r\n" → bulk string "key"
  → 读取 "$5\r\nvalue\r\n" → bulk string "value"
  → kComplete → HasCommand() = true
  → TakeCommand() = ["SET", "key", "value"]
```

**关键约束**：
- 仅支持 RESP2 命令数组，RESP3 后续加入
- 不支持 inline command（`SET key value` 不带协议前缀）
- 入站命令元素只接受 bulk string；null bulk string、null array、嵌套 array 都视为协议错误
- 空数组 `*0\r\n` 视为协议错误，空 bulk string `$0\r\n\r\n` 是合法参数
- 支持 pipelining：`Feed(cmd1 + cmd2)` 后 `PendingCommandCount() == 2`
- `TakeCommand()` 只移除一条已完成命令，不清空未解析缓冲
- 已消费的字节应从 `buffer_` 中移除；不完整命令的尾部字节必须保留到下一次 `Feed()`
- 解析错误后 `LastError()` 可读，`Reset()` 恢复干净状态
- 进入错误状态后，后续 `Feed()` 在 `Reset()` 前持续返回 `kError`

### 3.3 Reply 构造器 (RespReply)

```cpp
class RespReply {
public:
    // 工厂方法 — 返回 RESP 格式的字节序列
    static std::string SimpleString(std::string_view msg);  // +OK\r\n
    static std::string Error(std::string_view msg);         // -ERR ...\r\n
    static std::string Integer(int64_t n);                  // :42\r\n
    static std::string BulkString(std::string_view data);   // $5\r\nhello\r\n
    static std::string NullBulkString();                    // $-1\r\n
    static std::string ArrayOfBulkStrings(
        const std::vector<std::string>& elements);
    static std::string ArrayOfEncoded(
        const std::vector<std::string>& encoded_elements);
    static std::string EmptyArray();                        // *0\r\n

    // 便捷方法
    static std::string Ok();                   // +OK\r\n
    static std::string Nil();                  // $-1\r\n
    static std::string WrongType();            // -WRONGTYPE ...
    static std::string UnknownCommand(std::string_view cmd);
};
```

`ArrayOfBulkStrings()` 用于 `KEYS`、`LRANGE` 等普通字符串数组回复；`ArrayOfEncoded()` 接收已经由 `RespReply` 构造过的元素，用于未来混合类型或嵌套数组回复，避免把 `:1\r\n` 误编码成 bulk string。

---

## 4. Config

**文件**: `src/core/config.h` + `config.cpp`

### 4.1 设计

```cpp
struct MiniRedisConfig {
    // Network
    std::string bind = "127.0.0.1";
    uint16_t port = 6379;
    int tcp_backlog = 511;

    // Databases
    int databases = 16;

    // Encoding thresholds (overridable in config file)
    size_t set_max_intset_entries = 512;
    size_t hash_max_listpack_entries = 512;
    size_t hash_max_listpack_value = 64;
    size_t zset_max_listpack_entries = 128;
    size_t zset_max_listpack_value = 64;

    // Persistence (Phase 6)
    bool save_enabled = true;
    std::string rdb_filename = "dump.rdb";
    std::string aof_filename = "appendonly.aof";

    // Logging
    std::string log_level = "notice";
    std::string log_file = "";
};

class ConfigManager {
public:
    ConfigManager();

    // Load from redis.conf-style file
    bool LoadFromFile(const std::string& path);

    // Get a config value by key name
    std::optional<std::string> Get(std::string_view key) const;
    bool Set(std::string_view key, std::string_view value);

    // Access thresholds
    EncodingThresholds GetEncodingThresholds() const;

    // Raw config access
    const MiniRedisConfig& Config() const;

private:
    MiniRedisConfig config_;
    // Simple key-value store for unrecognized config directives
    std::unordered_map<std::string, std::string> extras_;
};
```

`GetEncodingThresholds()` 必须从 `MiniRedisConfig` 显式构造 Phase 2 的 `EncodingThresholds`。Phase 4 命令创建 `SetValue`、`HashValue`、`ZSetValue` 时，通过 `Server::GetEncodingThresholds()` 或 `Server::GetConfig()` 取得阈值，避免各处使用默认阈值。

### 4.2 文件格式

只支持简单的 `key value` 格式（不实现 Redis 的 `--flag` / include 等高级特性）：

```
# MiniRedis configuration
port 6379
bind 0.0.0.0
databases 16
```

### 4.3 Test 要点

- 默认值正确
- LoadFromFile 解析 key-value 对
- Get/Set 配置项
- 编码阈值提取
- 非法端口号/数字的拒绝
- `databases <= 0`、端口超过 `uint16_t` 范围、阈值为负数时拒绝

---

## 5. Database

**文件**: `src/core/database.h` + `database.cpp`

### 5.1 设计

```cpp
class Database {
public:
    Database();

    // Key CRUD
    bool Exists(std::string_view key);
    std::optional<ValueType> Type(std::string_view key);
    bool Delete(std::string_view key);

    // Value access
    Value* Find(std::string_view key);
    bool Set(std::string_view key, Value value);
    bool Rename(std::string_view old_key, std::string_view new_key);
    bool RenameNX(std::string_view old_key, std::string_view new_key);

    // Scan / iteration
    std::vector<std::string> Keys(std::string_view pattern = "*");
    std::optional<std::string> RandomKey();

    // Info
    size_t Size();
    size_t ExpiresSize() const;

    // Expire (Phase 7 will add active expiration logic here)
    bool SetExpire(std::string_view key, int64_t expire_at_ms);
    bool Persist(std::string_view key);                        // PERSIST
    int64_t TTL(std::string_view key);                         // ms, -1=persistent, -2=not found
    bool IsExpired(std::string_view key) const;

    // Flush
    void Clear();

private:
    // Keyspace: key → Value (move-only)
    ds::Dict<std::string, Value> keyspace_;

    // Expires: key → expiry timestamp (ms since epoch)
    // Only a subset of keys have entries here
    ds::Dict<std::string, int64_t> expires_;

    bool ExpireIfNeeded(std::string_view key);
    void PurgeExpiredKeys();
    int64_t NowMs() const;
};
```

### 5.2 关键设计决策

| 决策 | 选择 | 说明 |
|------|------|------|
| 键空间 | `ds::Dict<string, Value>` | 使用 Phase 1 的 Dict，支持渐进式 rehash |
| 过期 | `ds::Dict<string, int64_t>` | 独立字典，仅存储有过期时间的 key |
| TTL 单位 | 毫秒 (ms) | 内部统一毫秒，命令层适配 EXPIRE(秒)/PEXPIRE(毫秒) |
| Key 名称 | `std::string` | 与 Redis 一致，二进制安全的字符串 |
| Value 所有权 | move-only | `Set` 用 move 语义，被替换的旧 Value 自动析构 |

过期语义：
- `SetExpire(key, expire_at_ms)` 接收绝对时间戳（milliseconds since epoch），命令层负责把 EXPIRE/PEXPIRE 的相对时间转换为绝对时间。
- `TTL(key)` 返回 Redis 风格 sentinel：`-2` 表示 key 不存在或已过期，`-1` 表示存在但无过期时间，非负数表示剩余毫秒。
- `Find`、`Exists`、`Type`、`Keys`、`RandomKey`、`Size`、`Rename` 在返回前必须调用惰性过期路径，已过期 key 会被删除。
- `Keys` 和 `Size` 需要调用 `PurgeExpiredKeys()`，遍历所有 key 并删除已过期项，保证返回值不包含逻辑上不存在的 key。
- 因为惰性过期会修改字典，Phase 3 的 keyspace 访问 API 默认不是 `const`。后续如果需要 const 查询，应先为 `ds::Dict` 增加 const-safe iterator 或快照机制。
- `Set(key, value)` 必须清除旧 key 的过期时间，保持 Redis `SET` 默认移除 TTL 的语义；需要保留 TTL 的变体留给 Phase 4 命令层扩展。
- `Rename(old, new)` 必须把 old 的 TTL 一起迁移到 new；如果 new 已存在，其原有 TTL 被 old 的 TTL 覆盖。`RenameNX` 在 new 存在时不改变 old、new 或过期字典。

Phase 3 对 `ds::Dict` 的前置要求：
- 当前 `ds::Dict` 没有 const iterator，Database 遍历接口不要设计成 `const`。
- `RandomKey()` 当前只返回第一个遇到的 key；若 Phase 3 要测试随机性，需要先改进 `ds::Dict::RandomKey()`，否则只测试“返回一个存在的 key”。
- `Keys(pattern)` Phase 3 只要求支持 `*` 全量匹配；通配符完整语义可留到命令层或后续补充。

### 5.3 Test 要点

- 基本 CRUD（Set/Find/Exists/Delete）
- Type 正确识别五种类型
- Rename（普通/覆盖/NX）
- TTL 设置/查询/过期判断
- 访问已过期 key 会惰性删除，并表现为不存在
- `Set` 覆盖旧 key 时清除 TTL
- `Rename` 迁移 TTL，`RenameNX` 失败时不改变 TTL
- RandomKey / Keys 模式匹配
- Clear 清空
- Size / ExpiresSize

---

## 6. Client

**文件**: `src/core/client.h` + `client.cpp`

### 6.1 设计

Client 表示一个连接的状态。由于 Phase 3 不做 I/O，Client 在此阶段是**轻量状态容器**，I/O 相关字段在 Phase 5 加入。

```cpp
class Client {
public:
    Client(int fd, int db_index = 0);
    ~Client();

    // Identity
    int Fd() const;
    size_t Id() const;

    // Database selection
    int CurrentDb() const;
    bool SelectDb(int index, int db_count);

    // Query buffer (I/O thread feeds data here)
    std::vector<uint8_t>& QueryBuffer();
    const std::vector<uint8_t>& QueryBuffer() const;
    RespParser& Parser();
    const RespParser& Parser() const;

    // Reply buffer (commands write replies here, I/O thread sends)
    std::string& ReplyBuffer();
    const std::string& ReplyBuffer() const;

    // Authenticated
    bool IsAuthenticated() const;
    void SetAuthenticated(bool auth);

    // Name
    std::string Name() const;
    void SetName(std::string_view name);

private:
    int fd_;
    size_t id_;
    int db_index_;
    std::vector<uint8_t> query_buffer_;
    RespParser parser_;
    std::string reply_buffer_;
    bool authenticated_;
    std::string name_;

    static size_t next_id_;
};
```

### 6.2 Test 要点

- 构造 / ID 自增
- CurrentDb / SelectDb，拒绝负数和超过 `db_count - 1` 的索引
- Query/Reply buffer 读写，Parser 可接收 partial read 并保留 pipelined 命令
- 简单状态转换

---

## 7. Server

**文件**: `src/core/server.h` + `server.cpp`

### 7.1 设计

Server 是框架骨架，组合所有子系统。在 Phase 3 中它是一个**可构造、可测试的容器**，不启动实际 I/O 循环。

```cpp
class Server {
public:
    static Server& Instance();

    // Initialization
    bool Init(const MiniRedisConfig& config);

    // Database access
    Database* GetDb(int index);
    const Database* GetDb(int index) const;
    Database* GetDbFor(const Client& client);
    std::optional<size_t> DbSize(int index);
    bool FlushDb(int index);
    void FlushAll();
    int DbCount() const;

    // Client management
    Client* CreateClient(int fd);
    void RemoveClient(int fd);
    Client* FindClient(int fd);
    size_t ClientCount() const;

    // Config
    const MiniRedisConfig& GetConfig() const;
    EncodingThresholds GetEncodingThresholds() const;

    // Server info
    bool IsRunning() const;
    void Shutdown();

private:
    Server() = default;

    MiniRedisConfig config_;
    std::vector<Database> databases_;
    ds::Dict<int, std::unique_ptr<Client>> clients_;
    bool running_ = false;
};
```

Server 语义：
- `Init(config)` 校验 `config.databases > 0`，按配置创建数据库数量，不硬编码 16。
- `GetDb(index)` 越界返回 `nullptr`，不使用 `-1` 表示 current client。
- `GetDbFor(client)` 显式根据 `client.CurrentDb()` 查找数据库，命令层不得依赖 Server 内部隐式当前连接。
- `DbSize(index)` 越界返回 `std::nullopt`，有效索引返回对应 `Database::Size()`。
- `CreateClient(fd)` 默认选择 DB 0；如果 fd 已存在，返回 `nullptr`，不替换已有连接状态。
- `Shutdown()` 清空 clients 和 databases，并把 `running_` 设为 false；再次 `Init()` 应可重新初始化干净状态。

### 7.2 Test 要点

- 单例访问
- Init 按配置初始化数据库数量，拒绝非法数量
- Create/Remove/Find Client
- GetDb / GetDbFor / FlushDb / FlushAll
- Shutdown 清理

---

## 8. 构建集成

```cmake
# CMakeLists.txt 新增
add_library(miniredis_core STATIC
    src/core/resp_protocol.cpp
    src/core/config.cpp
    src/core/database.cpp
    src/core/client.cpp
    src/core/server.cpp
)
target_include_directories(miniredis_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis_core PUBLIC miniredis_types)
target_compile_features(miniredis_core PUBLIC cxx_std_20)
```

```cmake
# tests/CMakeLists.txt 新增
add_executable(miniredis_core_test
    core/resp_protocol_test.cpp
    core/config_test.cpp
    core/database_test.cpp
    core/client_test.cpp
    core/server_test.cpp
)
target_link_libraries(miniredis_core_test PRIVATE miniredis_core GTest::gtest_main)
target_compile_features(miniredis_core_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_core_test)
```

---

## 9. 实现顺序

```
RESP Protocol → Config → Dict 前置确认 → Database → Client → Server
```

- **RESP Protocol**: 无任何内部依赖，最先实现
- **Config**: 简单 key-value 解析，无依赖
- **Dict 前置确认**: 不新增 const iterator；Database API 保持非 const。确认 `RandomKey()` 只测试存在性。
- **Database**: 依赖 Value（Phase 2）和 Dict（Phase 1）
- **Client**: 状态容器，持有 RESP parser 或 query/reply buffer
- **Server**: 组合上述所有模块

---

## 10. 成功标准

- [ ] RESP 解析器正确处理 partial read 和 pipelined 命令数组
- [ ] RESP 解析器拒绝 null bulk、null array、嵌套 array、inline command 等非 Phase 3 入站格式
- [ ] Reply 构造器输出与 redis-cli 兼容的字节序列
- [ ] Config 正确解析 key-value 格式配置文件
- [ ] Config 暴露的编码阈值能传递给 Phase 2 Value 构造路径
- [ ] Database CRUD 五种类型 Value 正确存储和检索
- [ ] Database TTL 正确计算，访问已过期 key 时惰性删除
- [ ] Database `Set` 覆盖旧 key 时清除 TTL
- [ ] Client 管理（创建/删除/查找）
- [ ] Client DB 选择拒绝越界索引
- [ ] Server 按配置初始化 Database，且不依赖隐式 current client
- [ ] `ctest` 全绿
- [ ] 编译零警告
