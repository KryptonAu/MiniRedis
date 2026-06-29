# MiniRedis 性能测试计划

## 1. 概述

本计划定义 MiniRedis 的性能基准测试方案，以 Redis 作为 baseline 进行对比分析。

### 1.1 测试目标

- 量化 MiniRedis 在不同负载场景下的吞吐量和延迟表现
- 以 Redis 为 baseline，找出性能差距和优化空间
- 建立持续性能回归测试机制

### 1.2 工具选择

| 层次 | 工具 | 说明 |
|------|------|------|
| 对比基准 | `redis-benchmark` (Redis 自带) | 作为 baseline 测试工具，同时可直接用于 MiniRedis（RESP 兼容） |
| 原生集成 | `miniredis-bench` (自制) | C++20 原生实现，集成到 CMake 构建，作为 CI 可运行的基准测试 |
| 性能回归 | 脚本自动化 | 参考 Redis `speed-regression.tcl`，实现版本间性能对比 |

## 2. Redis redis-benchmark 分析

### 2.1 测试命令列表（默认 19 项）

| # | 测试项 | 命令 | 说明 |
|---|--------|------|------|
| 1 | PING_INLINE | `PING\r\n` | 内联协议 PING |
| 2 | PING_MBULK | `*1\r\n$4\r\nPING\r\n` | 标准协议 PING |
| 3 | SET | `SET key:__rand_int__ <data>` | 字符串写入 |
| 4 | GET | `GET key:__rand_int__` | 字符串读取 |
| 5 | INCR | `INCR counter:__rand_int__` | 计数器 |
| 6 | LPUSH | `LPUSH mylist <data>` | 列表左推 |
| 7 | RPUSH | `RPUSH mylist <data>` | 列表右推 |
| 8 | LPOP | `LPOP mylist` | 列表左弹 |
| 9 | RPOP | `RPOP mylist` | 列表右弹 |
| 10 | SADD | `SADD myset element:__rand_int__` | 集合添加 |
| 11 | HSET | `HSET myhash element:__rand_int__ <data>` | 哈希写入 |
| 12 | SPOP | `SPOP myset` | 集合弹出 |
| 13 | ZADD | `ZADD myzset <score> element:__rand_int__` | 有序集合添加 |
| 14 | ZPOPMIN | `ZPOPMIN myzset` | 有序集合最小弹出 |
| 15 | LRANGE_100 | `LRANGE mylist 0 99` | 列表范围查询 (100) |
| 16 | LRANGE_300 | `LRANGE mylist 0 299` | 列表范围查询 (300) |
| 17 | LRANGE_500 | `LRANGE mylist 0 499` | 列表范围查询 (500) |
| 18 | LRANGE_600 | `LRANGE mylist 0 599` | 列表范围查询 (600) |
| 19 | MSET | `MSET key1 v1 ... key10 v10` | 批量写入 (10 keys) |

### 2.2 关键指标

| 指标 | 缩写 | 说明 |
|------|------|------|
| 吞吐量 | RPS | Requests Per Second |
| 平均延迟 | avg | 所有请求的平均延迟 (ms) |
| 最小延迟 | min/p0 | 最小请求延迟 (ms) |
| 中位延迟 | p50 | 50% 请求完成的延迟 (ms) |
| 95分位延迟 | p95 | 95% 请求完成的延迟 (ms) |
| 99分位延迟 | p99 | 99% 请求完成的延迟 (ms) |
| 最大延迟 | max/p100 | 最大请求延迟 (ms) |

### 2.3 关键参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-c` | 50 | 并发连接数 |
| `-n` | 100000 | 总请求数 |
| `-d` | 3 | 数据大小 (bytes) |
| `-r` | 0 | 随机键空间大小 (0=单键) |
| `-P` | 1 | Pipeline 深度 |
| `-q` | off | 简洁输出模式 |
| `--csv` | off | CSV 输出格式 |
| `-k` | 1 | Keepalive |
| `-t` | all | 选择测试项 |
| `--threads` | 0 | 多线程模式 |

## 3. MiniRedis 当前命令覆盖分析

### 3.1 已实现命令统计

| 类别 | 文件 | 命令数 | 关键命令 |
|------|------|--------|----------|
| String | `string_commands.cpp` | 20 | SET, GET, INCR, DECR, MSET, MGET, APPEND, GETRANGE, SETRANGE, INCRBY, DECRBY, INCRBYFLOAT, SETNX, SETEX, PSETEX, GETSET, GETDEL, GETEX, STRLEN, MSETNX |
| List | `list_commands.cpp` | 12 | LPUSH, RPUSH, LPUSHX, RPUSHX, LPOP, RPOP, LLEN, LINDEX, LRANGE, LTRIM, LREM, RPOPLPUSH |
| Set | `set_commands.cpp` | 7 | SADD, SREM, SMEMBERS, SCARD, SISMEMBER, SPOP, SRANDMEMBER |
| Hash | `hash_commands.cpp` | 9 | HSET, HGET, HDEL, HLEN, HEXISTS, HKEYS, HVALS, HGETALL, HINCRBY |
| ZSet | `zset_commands.cpp` | 15 | ZADD, ZREM, ZCARD, ZCOUNT, ZSCORE, ZRANK, ZREVRANK, ZINCRBY, ZRANGE, ZREVRANGE, ZRANGEBYSCORE, ZPOPMIN, ZPOPMAX, ZREMRANGEBYRANK, ZREMRANGEBYSCORE |
| Key | `key_commands.cpp` | 14 | DEL, EXISTS, TYPE, EXPIRE, PEXPIRE, EXPIREAT, PEXPIREAT, TTL, PTTL, PERSIST, KEYS, RANDOMKEY, RENAME, RENAMENX |
| Server | `server_commands.cpp` | 12 | PING, PONG, CONFIG GET/SET, INFO, ECHO, SELECT, DBSIZE, FLUSHDB, FLUSHALL, TIME, COMMAND, SAVE, LASTSAVE |

**总计: 89 个命令**。redis-benchmark 默认测试所需的全部命令 MiniRedis 均已实现。

### 3.2 尚未支持的关键特性

| 特性 | MiniRedis 状态 | 对性能测试的影响 |
|------|---------------|-----------------|
| Pipeline | 需确认 | 影响高吞吐场景 |
| 多 DB (SELECT) | 已实现 | redis-benchmark 默认使用 SELECT |
| 批量命令 (MSET) | 已实现 | - |
| 持久化 (AOF/RDB) | 已实现 | 可测试持久化对性能的影响 |
| 过期淘汰 | 已实现 | 可测试过期策略的 CPU 开销 |

## 4. 测试场景矩阵

### 4.1 基准吞吐量测试（默认参数）

使用 redis-benchmark 默认参数 (`-c 50 -n 100000 -d 3`) 同时测试 MiniRedis 和 Redis：

```
场景: redis-benchmark -h <host> -p <port> --csv
目标: 获取所有默认测试项的 RPS 和延迟，直接对比
```

### 4.2 并发扩展性测试

```
参数: -c [1, 10, 50, 100, 200, 500] -n [100000 * clients]
目标: 观察 QPS 随并发连接数的变化曲线
重点命令: SET, GET, PING, INCR
```

### 4.3 Pipeline 效率测试

```
参数: -P [1, 4, 8, 16, 32, 64] -c 50 -n 100000
目标: 观察 Pipeline 对吞吐的加速比
重点命令: SET, GET
```

### 4.4 负载大小测试

```
参数: -d [3, 64, 256, 1024, 4096, 65536] -c 50 -n [10000 ~ 100000]
目标: 观察不同 value 大小对 SET/GET 性能的影响
重点命令: SET, GET
```

### 4.5 键空间大小测试

```
参数: -r [0, 1000, 100000, 1000000] -c 50 -n 100000
目标: 观察随机键命中率对性能的影响（评估 dict 查找效率）
重点命令: SET, GET, SADD, HSET
```

### 4.6 各数据类型专项测试

```
目标: 对比每种数据结构操作的性能
Hash: HSET, HGET, HGETALL (不同 field 数量)
List: LPUSH/LPOP, LRANGE (不同列表长度)
Set: SADD, SPOP (不同集合大小)
ZSet: ZADD, ZRANGE (不同元素数量)
```

### 4.7 持久化影响测试

```
场景: 开启/关闭 AOF 和 RDB 保存时的性能对比
目标: 量化持久化子系统对性能的影响
```

### 4.8 混合负载测试

```
目标: 模拟真实场景，混合读写比例
实现: 自制脚本，按比例混合发送不同命令
```

## 5. 实施计划

### Phase A: 基础基准测试（直接用 redis-benchmark）

**工时预估**: 1-2 天

1. 启动 MiniRedis 服务端
2. 使用 `redis-benchmark` 直接连接 MiniRedis，运行默认测试套件
3. 在相同硬件上对 Redis 7.x 运行相同测试
4. 生成对比报告（CSV → 表格/图表）
5. 分析差距，识别瓶颈（profile 辅助）

**输出物**:
- `benchmarks/results/phase_a/` 目录存放原始 CSV
- `BENCHMARK_REPORT.md` 对比分析报告

### Phase B: 原生 miniredis-bench 工具

**工时预估**: 3-5 天

构建 MiniRedis 原生 benchmark 工具，集成到 CMake 构建：

```
src/benchmark/
├── miniredis_bench.cpp    # 主程序入口
├── bench_client.h         # TCP 客户端（异步/协程）
├── bench_client.cpp
├── bench_config.h         # 配置解析
├── bench_reporter.h       # 结果聚合与输出
├── bench_reporter.cpp
└── CMakeLists.txt
```

**关键特性**:
- C++20 协程/stdexec 异步 I/O，充分利用 MiniRedis 自己的网络层
- HDR Histogram 集成（或自实现简化版）
- 支持所有 redis-benchmark 的参数格式
- 输出格式兼容 redis-benchmark（CSV + 详细报告）
- 集成到 CTest 作为可选性能测试

**设计要点**:
```cpp
// bench_config.h - 参数结构
struct BenchConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    int num_clients = 50;
    int num_requests = 100'000;
    int data_size = 3;
    int pipeline = 1;
    int random_keyspace = 0;
    bool keepalive = true;
    bool quiet = false;
    bool csv = false;
    int num_threads = 0;
    std::vector<std::string> tests;
};

// 延迟采集使用轻量级 HDR Histogram
// 或直接使用 <chrono> + 分桶统计（p50/p95/p99）
```

### Phase C: 性能回归自动化

**工时预估**: 2-3 天

```
benchmarks/scripts/
├── run_benchmarks.sh      # 自动化启动 MiniRedis/Redis + 运行测试
├── compare_results.py     # 结果对比 + 图表生成
└── speed_regression.sh    # CI 集成：对比 main vs 当前分支
```

**CI 集成** (GitHub Actions / 本地钩子):

```yaml
# .github/workflows/perf.yml (示例)
name: Performance Regression
on:
  pull_request:
    paths: ['src/**', 'CMakeLists.txt']
jobs:
  benchmark:
    runs-on: ubuntu-24.04
    steps:
      - run: benchmarks/scripts/run_benchmarks.sh --baseline main --target HEAD
      - uses: actions/upload-artifact@v4
        with: { name: 'bench-results', path: 'benchmarks/results/' }
```

### Phase D: 深度分析工具

**工时预估**: 2-3 天

1. **内置延迟统计**: 在 MiniRedis 内部添加命令级延迟统计（类似 Redis `INFO commandstats`）
2. **Perf 集成**: 编写 perf script 采集 CPU 微架构数据
3. **内存分析**: 使用 heaptrack/valgrind massif 对比内存占用

## 6. MiniRedis 特有考量

### 6.1 双线程模型瓶颈分析

MiniRedis 使用 IO 线程 + CMD 线程分离架构。benchmark 需特别关注：

- **线程间通信延迟**: 通过 `exec::async_scope` 和 sender/receiver 传递请求，需测量传递开销
- **IO 线程饱和度**: 在高并发连接数下 epoll 的 CPU 占用
- **锁竞争**: CMD 线程是无锁设计，但 IO 线程的 epoll 事件注册/注销可能成为瓶颈

### 6.2 C++ vs C 基础开销

可能存在的固有差距：
- `std::string` vs SDS 的内存分配模式
- `std::variant` 访问 vs `robj->type` 整数比较
- `shared_ptr` 引用计数 vs 手动 refcount
- 模板实例化带来的 I-cache 压力

### 6.3 数据结构对标

| 结构 | Redis (C) | MiniRedis (C++20) | 预期差异 |
|------|-----------|-------------------|---------|
| 哈希表 | dict.c (渐进rehash) | 模板 dict (同算法) | 接近 |
| 紧凑列表 | quicklist + listpack | quicklist + listpack | 接近 |
| 整数集合 | intset.c | intset.cpp | 接近 |
| 跳表 | zskiplist.c | C++ zskiplist | 接近 |
| 字符串 | SDS | `std::string` | SDS 更紧凑 |

## 7. 预期性能目标

初步目标（Phase A 完成后根据实际数据调整）:

| 指标 | 目标 | 说明 |
|------|------|------|
| PING | Redis 的 80%+ | 纯协议开销 |
| SET | Redis 的 70%+ | 涉及 value 写入 |
| GET | Redis 的 80%+ | 纯读取 |
| LPUSH/LPOP | Redis 的 70%+ | 涉及 list 操作 |
| SADD/SPOP | Redis 的 70%+ | 涉及 dict 操作 |
| 延迟 p50 | < 2x Redis | 中位延迟不应超过 Redis 2 倍 |
| 内存占用 | < 1.5x Redis | 现代 C++ 开销可控 |

## 8. 风险与应对

| 风险 | 应对 |
|------|------|
| MiniRedis 不稳定导致 benchmark 崩溃 | 先用功能测试确保稳定性，再跑性能 |
| 双线程模型在高并发下出现瓶颈 | 用 `perf record` + FlameGraph 定位热点 |
| stdexec 协程调度开销大 | 对比同步 epoll 版本，量化 stdexec 开销 |
| redis-benchmark 参数不兼容 | 先验证基础命令连通性 (PING) |
| 测试环境噪音（CPU 频率、NUMA 等） | 固定 CPU 频率、绑核、多次运行取中位数 |

## 9. 优先级与排期

| Phase | 内容 | 优先级 | 建议时间 |
|-------|------|--------|---------|
| Phase A | redis-benchmark 直连测试 | 🔴 最高 | 第 1 周 |
| Phase B.1 | miniredis-bench 基础版本 | 🟡 高 | 第 2 周 |
| Phase B.2 | miniredis-bench 完整版本 | 🟢 中 | 第 3 周 |
| Phase C | 性能回归自动化 | 🟢 中 | 第 3 周 |
| Phase D | 深度分析工具 | 🔵 低 | 第 4 周+ |
