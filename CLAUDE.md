# MiniRedis

C++20 实现的 Redis 兼容内存数据库，面向 Linux 平台。

## 项目概述

MiniRedis 是一个从零开始用 C++20 实现的 Redis 兼容数据库，目标是复刻 Redis 的核心功能（5 大数据类型、RESP 协议、RDB/AOF 持久化等），同时利用现代 C++ 特性（`std::variant`、协程、`stdexec` 等）重新设计架构。

**设计文档**: `../redis/code_analysis/inspiration.md` — 记录了所有架构决策、灵感讨论和 Phase 1 命令范围。

## 构建系统

### 编译器要求

- **最低版本**: GCC 12
- **必须显式使用**: `gcc-12` 和 `g++-12`

```bash
# 配置（必须指定编译器）
cmake -B build -G Ninja \
  -DCMAKE_C_COMPILER=gcc-12 \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_BUILD_TYPE=Debug

# 构建
ninja -C build

# 运行测试
cd build && ctest --output-on-failure
```

### 依赖

| 依赖 | 说明 |
|------|------|
| stdexec (NVIDIA) | P2300 Sender/Receiver 异步框架，header-only |
| pthread | Linux 原生线程库 |
| liburing (可选) | io_uring 支持，后续按需引入 |

stdexec 通过 CMake `FetchContent` 自动下载，无需手动安装。

## 代码风格

严格遵循 **Google C++ Style**。关键规则：

- 使用 `.clang-format` 自动格式化（Google style）
- 命名：类型 `PascalCase`，函数 `PascalCase`，变量 `snake_case`，常量 `kCamelCase`
- 头文件使用 `#pragma once`
- 不用裸 `new`/`delete`，通过 RAII 和智能指针管理所有生命周期
- 优先使用 C++20 特性：`std::variant`、`std::string_view`、`std::span`、concepts

## 实现原则

**MiniRedis 重视性能表现。** 对于 `inspiration.md` 讨论中未覆盖的技术细节，应当：

1. **参照 Redis 源码** — 还原 Redis 的逻辑与行为，包括算法选择、数据结构布局、边界条件处理
2. **遵循现代 C++ 编码方式** — 用 C++20 惯用法（RAII、智能指针、`std::variant`、concepts 等）重新表达 Redis 的设计意图，而非机械翻译 C 代码

简单说：**逻辑向 Redis 看齐，写法向 C++20 看齐。**

## 架构概览

### 双线程模型

```
IO 线程 (epoll scheduler)          CMD 线程 (单线程)
─────────────────────────         ─────────────────
epoll_wait + coroutine resume     processCommand()
async_read/async_write            数据操作 (dict, listpack, etc.)
RESP 协议解析                     过期/淘汰
新连接 accept                     无锁、无竞争
```

### 目录结构

```
src/
├── core/          # 核心运行时（IO调度器、协议、Server、Database、Client）
├── ds/            # 基础数据结构（dict、skiplist、intset、listpack、quicklist）
├── types/         # 数据类型（Value variant + 五种类型实现）
├── commands/      # 命令注册表 + 各类型命令实现
├── persistence/   # RDB 序列化、AOF 日志
├── eviction/      # 过期与淘汰策略
└── pubsub/        # 发布订阅（Phase 1 暂不实现）
```

### 关键设计决策

| 决策 | 选择 | 说明 |
|------|------|------|
| 对象系统 | `std::variant` | 编译期类型安全，替代 Redis robj |
| 字符串 | `std::string` / `string_view` | 替代 SDS |
| 哈希表 | 自定义 dict（模板） | 渐进式 rehash，双表设计 |
| 紧凑存储 | quicklist + listpack + intset | 弃用 ziplist |
| 内存管理 | `shared_ptr` / `unique_ptr` | 替代手动引用计数 |
| 命令注册 | CommandRegistry 单例 + 宏 | 自动注册 |
| RESP 协议 | 完全兼容 | redis-cli 可直接连接 |
| RDB 格式 | 兼容 Redis | 可互相加载数据 |

## 参考 Redis 源码

Redis 源码位于 `../redis/`，关键模块：

| 模块 | 路径 | 职责 |
|------|------|------|
| 事件循环 | `../redis/src/ae.c` | epoll 封装（MiniRedis 用 stdexec 替代） |
| 网络层 | `../redis/src/networking.c` | 客户端连接、协议解析 |
| 命令表 | `../redis/src/server.c` (redisCommandTable) | 命令注册与分发 |
| 字典 | `../redis/src/dict.c` | 核心数据结构（MiniRedis 复刻） |
| 数据类型 | `../redis/src/t_*.c` | 五种类型的命令实现 |
| 持久化 | `../redis/src/rdb.c`, `../redis/src/aof.c` | RDB/AOF |
| 过期 | `../redis/src/expire.c`, `../redis/src/evict.c` | 过期与淘汰 |

分析文档位于 `../redis/code_analysis/`。

## 代码导航

本项目已配置 Codegraph 代码索引。使用以下工具进行代码导航：

- `codegraph_context` — 任务/功能的架构上下文
- `codegraph_search` — 符号搜索
- `codegraph_trace` — 调用链追踪
- `codegraph_explore` — 多符号源码浏览

## Phase 1 范围

实现约 100 个命令：String(15)、List(12)、Set(13)、Hash(13)、ZSet(22)、Key(18)、Server(10)。

详细命令列表见 `../redis/code_analysis/inspiration.md` 第十一节。
