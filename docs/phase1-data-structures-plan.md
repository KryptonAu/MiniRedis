# MiniRedis Phase 1: 基础数据结构实现计划

> 状态: 已根据审查意见修订 | 日期: 2026-06-27

## 概述

Phase 1 实现 MiniRedis 所需的 5 个核心数据结构。这些结构是整个系统的基石，后续所有类型系统和命令实现都依赖它们。

每个数据结构独立可测，有完整的单元测试覆盖。

本阶段遵循两个边界：
- **行为语义向 Redis 看齐**：哈希表 rehash、listpack/intset 二进制格式、quicklist fill 语义、skiplist 排名语义优先参考 Redis。
- **实现方式向 C++20 看齐**：所有节点生命周期由 RAII 容器或智能指针托管，拓扑指针只作为非拥有观察指针使用。

---

## 1. 实现顺序与依赖关系

```
dict ─────────────────────┐
listpack ──→ quicklist    │
intset                    │
skiplist ─────────────────┘
```

- **dict**: 无内部依赖，最先实现
- **listpack**: 无内部依赖
- **intset**: 无内部依赖
- **quicklist**: 依赖 listpack（每个节点是一个 listpack）
- **skiplist**: 无内部依赖

dict、listpack、intset 可以并行开发。quicklist 必须在 listpack 之后。skiplist 可在任意时机开发。

---

## 2. Dict — 渐进式 Rehash 哈希表

**文件**: `src/ds/dict.h` (header-only 模板)

### 2.1 设计要点

| 特性 | 说明 |
|------|------|
| 双表设计 | `ht[0]` 主表 + `ht[1]` 扩容表 |
| 渐进式 rehash | 每次操作迁移 N 个桶（可配置，默认 1），避免大表 rehash 阻塞服务 |
| rehash 触发条件 | 负载因子 ≥ 1（可配置）时触发扩容；负载因子 < 0.1 时触发缩容 |
| 扩容倍数 | 2x（取下一个 ≥ `used*2` 的 2 的幂） |
| 安全/非安全迭代器 | 两类迭代器都遍历双表；安全迭代器允许迭代期间修改并暂停 rehash，非安全迭代器禁止迭代期间修改 |
| 哈希函数 | 默认 `std::hash<K>`，支持自定义 |
| 链表法解决冲突 | 与 Redis 一致，每个桶是单向链表 |

### 2.2 模板参数

```cpp
template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>
>
class Dict;
```

### 2.3 核心 API

```cpp
// 构造与析构
Dict();
~Dict();

// 容量
size_t Size() const;         // entry 数量，等价于 ht_[0].used + ht_[1].used
size_t Buckets() const;      // bucket 槽位总数，等价于 ht_[0].size + ht_[1].size
bool IsRehashing() const;

// 增删查改
bool Add(Key key, Value value);           // 添加，key 已存在时失败
bool Set(Key key, Value value);           // 设置（覆盖或添加）
Value* Find(const Key& key);              // 查找，返回指针（不存在时 nullptr）
const Value* Find(const Key& key) const;
bool Delete(const Key& key);              // 删除
void Clear();                             // 清空

// 迭代器
class Iterator;            // 非安全迭代器：迭代期间不允许修改 dict
class SafeIterator;        // 安全迭代器：允许迭代期间增删查，期间暂停 rehash 迁移
Iterator begin();
Iterator end();
SafeIterator SafeBegin();
SafeIterator SafeEnd();

// rehash 控制
int Rehash(int n);         // 迁移 n 个桶，返回 1=rehash 未完成, 0=完成
void RehashStep();         // 单步 rehash（迁移 1 个桶）

// 随机 key（用于过期抽样）
std::optional<Key> RandomKey();
```

### 2.4 内部结构

```cpp
struct DictEntry {
    Key key;
    Value value;
    std::unique_ptr<DictEntry> next;  // 单向链表，拥有下一个节点
};

struct DictTable {
    std::vector<std::unique_ptr<DictEntry>> buckets;
    size_t size;      // bucket 槽位数，等价于 buckets.size()
    size_t size_mask; // size - 1，size 为 2 的幂时可用 hash & size_mask
    size_t used;      // entry 数量
};

// Dict 成员
DictTable ht_[2];
long rehash_idx_;          // -1 = 未在 rehash, >=0 = 当前迁移到的 bucket 索引
size_t rehash_step_;       // 每次操作迁移的 bucket 数（默认 1）
size_t safe_iterators_;    // 活跃安全迭代器数量；大于 0 时暂停 rehash 迁移
Hash hash_;
KeyEqual key_equal_;
```

### 2.5 关键算法

**渐进式 Rehash 流程**:
```
Add/Set/Delete/Find(非 const) 操作前调用 MaybeRehashStep():
  1. 如果 !IsRehashing() 且 ht_[0].used >= ht_[0].size → 初始化 ht_[1] 扩容
  2. 如果 !IsRehashing() 且负载因子 < 0.1 且 size 大于最小表大小 → 初始化 ht_[1] 缩容
  3. 如果 IsRehashing() 且 safe_iterators_ == 0 → Rehash(N) 迁移 N 个非空 bucket
  4. 如果 ht_[0].used == 0 → 释放旧表，将 ht_[1] 变为 ht_[0]，结束 rehash
```

**查找（rehash 期间）**:
```
Find(key):
  1. 在 ht_[0] 中查找（hash & ht_[0].size_mask）
  2. 如果 IsRehashing() → 在 ht_[1] 中也查找
  3. 返回找到的 entry 或 nullptr
```

**插入位置（rehash 期间）**:
```
Add/Set 新 key:
  1. 先在两个表中检查是否已存在
  2. 如果正在 rehash，新 entry 插入 ht_[1]
  3. 否则插入 ht_[0]
```

**安全迭代器约束**:
- `Iterator` 是非安全迭代器，迭代期间调用 `Add/Set/Delete/Clear/Rehash` 属于未定义使用，Debug 构建可用 fingerprint 检测。
- `SafeIterator` 构造时递增 `safe_iterators_`，析构时递减；活跃期间允许修改 dict，但 `MaybeRehashStep()` 不迁移 bucket。
- 两类迭代器在 rehash 期间都需要遍历 `ht_[0]` 和 `ht_[1]`，不能只遍历 `ht_[0]`。

### 2.6 测试要点

- 基本 CRUD 操作
- 扩容触发与完成
- 缩容触发与完成
- rehash 期间的正确读写
- rehash 期间新 key 插入 ht_[1]
- 安全迭代器在 rehash 期间的正确遍历
- 安全迭代器活跃期间不会迁移 bucket
- 边界条件：空表、单元素、重复 key、删除不存在的 key
- `RandomKey()` 空表返回 `std::nullopt`，非空表能覆盖两个表

### 2.7 Redis 参考

- `../redis/src/dict.c` — dictRehash, dictAdd, dictFind, dictDelete 等
- 保持与 Redis 相同的行为语义

---

## 3. Listpack — 紧凑列表

**文件**: `src/ds/listpack.h` + `src/ds/listpack.cpp`

### 3.1 设计要点

| 特性 | 说明 |
|------|------|
| 格式 | 6 字节固定头 + N 个 entry + 1 字节尾标记 `0xFF` |
| entry 结构 | encoding + data + backlen(1-5字节，backlen 记录 encoding+data 的长度) |
| 无 backward 指针 | 通过 backlen 反向遍历，从根本上消除 ziplist 的级联更新问题 |
| 数据访问 | 零拷贝 `std::string_view`，指向 listpack 内部内存 |
| 内存管理 | `std::vector<uint8_t>` 作为底层存储 |

### 3.2 二进制格式

```
┌──────────────┬──────────────────────────────┬──────┐
│  header (6B) │  entry 1 │ entry 2 │ ...     │ 0xFF │
│ totbytes(4B) │          │         │         │ end  │
│ numelems(2B) │          │         │         │      │
└──────────────┴──────────────────────────────┴──────┘
```

Entry 编码必须与 Redis `listpack.c` 一致：

| 编码 | 含义 |
|------|------|
| `0xxxxxxx` | 7-bit unsigned integer，范围 0..127 |
| `10xxxxxx` | 6-bit string，长度 0..63 |
| `110xxxxx xxxxxxxx` | 13-bit signed integer |
| `1110xxxx xxxxxxxx` | 12-bit string，长度 0..4095 |
| `0xF0 <u32-len>` | 32-bit string，长度使用 little-endian u32 |
| `0xF1 <i16>` | 16-bit signed integer |
| `0xF2 <i24>` | 24-bit signed integer |
| `0xF3 <i32>` | 32-bit signed integer |
| `0xF4 <i64>` | 64-bit signed integer |
| `0xFF` | EOF marker，不是普通 entry |

backlen 使用 Redis 的反向变长编码，长度 1..5 字节。它记录的是 `encoding + data` 的字节数，不包含 backlen 自身；完整 entry 大小为：

```
entry_size = encoded_payload_size + BacklenSize(encoded_payload_size)
```

### 3.3 核心 API

```cpp
class Listpack {
public:
    struct Value {
        enum class Type { kString, kInteger };
        Type type;
        std::string_view string;
        int64_t integer;

        std::string ToString() const;  // 整数按 Redis 响应语义转为十进制字符串
    };

    // 构造
    Listpack();
    static std::optional<Listpack> FromBytes(std::vector<uint8_t> data);  // RDB 加载，先校验格式

    // 查询
    size_t Size() const;                          // entry 数量
    size_t TotalBytes() const;                    // 总字节数
    const uint8_t* Data() const;                  // 原始数据指针
    size_t DataSize() const;                      // 原始数据大小
    static size_t EncodedEntrySize(std::string_view value);
    static size_t EncodedEntrySize(int64_t value);

    // 读取（零拷贝）
    std::optional<Value> Get(size_t index) const;
    std::optional<std::string_view> GetString(size_t index) const;
    std::optional<int64_t> GetInteger(size_t index) const;

    // 判断 entry 类型
    bool IsString(size_t index) const;
    bool IsInteger(size_t index) const;

    // 修改
    bool Insert(size_t index, std::string_view value);
    bool Insert(size_t index, int64_t value);
    bool Replace(size_t index, std::string_view value);
    bool Replace(size_t index, int64_t value);
    bool Delete(size_t index);
    bool Append(std::string_view value);
    bool Append(int64_t value);
    bool Prepend(std::string_view value);
    bool Prepend(int64_t value);

    // 查找（顺序扫描）
    std::optional<size_t> Find(std::string_view value) const;  // 返回 index
    std::optional<size_t> Find(int64_t value) const;

private:
    std::vector<uint8_t> buf_;

    // 内部辅助
    size_t EntryCount() const;     // 从 header 读取；UINT16_MAX 表示未知时需要扫描
    std::optional<size_t> Seek(size_t index) const;  // 定位到第 index 个 entry 的偏移
    size_t SeekInsertPosition(size_t index) const;   // index == Size() 时返回 EOF 偏移
    size_t DecodeBacklenEndingAt(size_t pos) const;
    size_t EncodedPayloadSizeAt(size_t pos) const;   // encoding + data，不含 backlen
    size_t EntrySizeAt(size_t pos) const;            // EncodedPayloadSize + backlen size
    void UpdateHeader();           // 更新 totbytes 和 numelems
    static bool ValidateBytes(std::span<const uint8_t> data);
};
```

### 3.4 关键算法

**正向遍历**:
```
Seek(index):
  pos = 6  // skip header
  for i in 0..index-1:
    pos += EntrySizeAt(pos)
  return pos
```

**反向遍历**:
```
PrevEntry(pos):
  // pos 是当前 entry 起始偏移，或 EOF 偏移
  payload_len = DecodeBacklenEndingAt(pos - 1)
  backlen_len = BacklenSize(payload_len)
  return pos - payload_len - backlen_len
```

**插入**:
```
Insert(index, value):
  1. SeekInsertPosition(index) 找到插入位置（允许 index == Size()）
  2. 编码 value → new_entry bytes
  3. buf_.insert(pos, new_entry)  // 移动后续数据
  4. UpdateHeader()
```

### 3.5 生命周期注意事项

`Get()` / `GetString()` 返回的 `string_view` 指向 `buf_` 内部。当 listpack 被修改（Insert/Delete/Replace）后，所有之前的 `string_view` 都失效。这个约束在文档中明确标注，调用方负责保证不跨修改使用。

`GetString()` 只在 entry 实际为字符串编码时返回视图；整数编码使用 `GetInteger()` 或 `Value::ToString()`。List/Hash/ZSet 等上层命令如果需要 RESP 字节串语义，应使用 `ToString()` 生成拷贝，避免把整数编码误解为空字符串。

### 3.6 测试要点

- 各种编码的读写（7-bit uint、13/16/24/32/64-bit int、6/12/32-bit string）
- 首/中/尾位置的插入和删除
- 大数量 entry（>1000）的性能验证
- 空 listpack 操作
- 边界条件：重复插入、删除最后一个元素
- 反向遍历正确性
- `string_view` 零拷贝验证
- 从原始字节构造（RDB 加载场景）：totbytes、numele、EOF、entry 边界、backlen 都必须校验
- 与 Redis 生成的 listpack 字节样例做 golden fixture 对比

### 3.7 Redis 参考

- `../redis/src/listpack.c` — lpNew, lpInsert, lpReplace, lpDelete, lpGet, lpFind 等
- `../redis/src/listpack.h` — LP_* 编码常量

---

## 4. Intset — 整数集合

**文件**: `src/ds/intset.h` + `src/ds/intset.cpp`

### 4.1 设计要点

| 特性 | 说明 |
|------|------|
| 存储 | Redis 兼容二进制布局：`encoding` + `length` + 有序整数内容 |
| 编码升级 | int16 → int32 → int64，按需自动升级 |
| 去重 | 插入时自动检查重复（二分查找） |
| 有序性 | 始终保持升序 |

### 4.2 核心 API

```cpp
class Intset {
public:
    Intset();
    static std::optional<Intset> FromBytes(std::vector<uint8_t> data);  // RDB 加载，先校验格式

    // 查询
    size_t Size() const;
    bool Contains(int64_t value) const;
    std::optional<int64_t> Get(size_t index) const;
    uint32_t Encoding() const;  // 2, 4, 8，分别表示 int16/int32/int64

    // 修改
    bool Add(int64_t value);      // 返回 true=已添加, false=已存在
    bool Remove(int64_t value);   // 返回 true=已删除, false=不存在

    // 查找
    std::optional<size_t> Find(int64_t value) const;  // 返回 index

    // 遍历
    std::vector<int64_t> Values() const;

    // 序列化
    const uint8_t* Data() const;
    size_t DataSize() const;

private:
    std::vector<uint8_t> buf_;  // Redis intset blob: uint32 encoding + uint32 length + contents

    uint32_t Length() const;
    int64_t GetEncoded(size_t index, uint32_t encoding) const;
    void SetEncoded(size_t index, uint32_t encoding, int64_t value);
    uint32_t RequiredEncoding(int64_t value) const;
    bool Search(int64_t value, size_t* pos) const;
    void UpgradeAndAdd(int64_t value, uint32_t new_encoding);
    void Resize(uint32_t encoding, size_t length);
    static bool ValidateBytes(std::span<const uint8_t> data);
};
```

### 4.3 关键算法

**插入（二分查找定位 + 移动 + 插入）**:
```
Add(value):
  new_encoding = RequiredEncoding(value)
  if new_encoding > Encoding():
    UpgradeAndAdd(value, new_encoding)
    return true

  found, pos = Search(value)
  if found → return false（已存在）
  Resize(Encoding(), Size() + 1)
  将 pos..end 向右移动一个 encoded slot
  SetEncoded(pos, Encoding(), value)
  return true
```

**删除**:
```
Remove(value):
  if RequiredEncoding(value) > Encoding() → return false
  found, pos = Search(value)
  if found:
    将 pos+1..end 向左移动一个 encoded slot
    Resize(Encoding(), Size() - 1)
    return true
  return false
```

**编码升级**:
```
UpgradeAndAdd(value, new_encoding):
  1. 旧内容按旧 encoding 从后向前复制到新 encoding 区域，避免覆盖
  2. 如果 value 为负数，插入 index 0；如果为正数，插入尾部
  3. 更新 header.encoding 和 header.length
```

### 4.4 兼容性决策

MiniRedis 保留 Redis intset 的外部字节语义：
- header 为 `uint32_t encoding` + `uint32_t length`
- `encoding` 取值为 2、4、8，分别对应 int16/int32/int64
- contents 始终升序，使用当前 encoding 的定宽槽位
- 本机实现使用 `std::vector<uint8_t>` 托管连续内存；读写 header 和元素时通过小端 helper，避免未对齐访问和别名问题

不采用固定 `std::vector<int64_t>`，因为这会破坏 RDB 兼容格式，也无法测试 Redis intset 的编码升级边界。

### 4.5 测试要点

- 基本 Add/Remove/Contains
- 有序性保持
- 重复插入处理
- int16 → int32 → int64 编码升级（正数和负数两条路径）
- 空集合操作
- 大量数据（>1000）的二分查找正确性
- 边界值（INT64_MIN, INT64_MAX）
- `Data()` / `DataSize()` 输出与 Redis intset blob 结构一致
- `FromBytes()` 拒绝长度、encoding、排序或重复元素不合法的输入

### 4.6 Redis 参考

- `../redis/src/intset.c` — intsetAdd, intsetRemove, intsetFind, intsetSearch

---

## 5. Quicklist — 双端链表节点列表

**文件**: `src/ds/quicklist.h` + `src/ds/quicklist.cpp`

### 5.1 设计要点

| 特性 | 说明 |
|------|------|
| 结构 | 双向链表，每个节点是一个 listpack |
| fill 因子 | 控制每个 listpack 节点的最大 entry 数（默认 -2 = 每个节点最多 8KB） |
| 压缩 | 可选中间节点压缩（LZF），Phase 1 暂不实现 |
| 用途 | List 类型的唯一编码 |

### 5.2 核心 API

```cpp
struct QuicklistNode {
    Listpack lp;
    size_t count;
};

class Quicklist {
public:
    Quicklist();

    // 查询
    size_t Size() const;       // 总 entry 数
    size_t NodeCount() const;  // 节点数
    bool Empty() const;

    // 两端操作
    void PushHead(std::string_view value);
    void PushHead(int64_t value);
    void PushTail(std::string_view value);
    void PushTail(int64_t value);
    std::optional<std::string> PopHead();  // 返回拷贝（节点可能被删除）
    std::optional<std::string> PopTail();

    // 索引访问
    std::optional<Listpack::Value> GetValue(size_t index) const;
    std::optional<std::string> Get(size_t index) const;  // RESP 字节串语义，整数编码转十进制字符串
    bool Set(size_t index, std::string_view value);
    bool Set(size_t index, int64_t value);

    // 插入
    bool InsertBefore(size_t index, std::string_view value);
    bool InsertBefore(size_t index, int64_t value);
    bool InsertAfter(size_t index, std::string_view value);
    bool InsertAfter(size_t index, int64_t value);

    // 删除
    bool Delete(size_t index);
    bool DeleteRange(size_t start, size_t count);

    // 查找
    std::optional<size_t> Find(std::string_view value) const;

    // 迭代器
    class Iterator;
    Iterator Begin();
    Iterator End();

private:
    using NodeList = std::list<QuicklistNode>;
    NodeList nodes_;        // std::list 提供稳定节点地址和双向链表语义
    size_t count_;          // 总 entry 数
    int fill_;             // fill 因子

    // 内部辅助
    NodeList::iterator NewNodeBefore(NodeList::iterator pos);
    NodeList::iterator NewNodeAfter(NodeList::iterator pos);
    std::pair<NodeList::iterator, size_t> Seek(size_t index);
    std::pair<NodeList::const_iterator, size_t> Seek(size_t index) const;
    bool NodeAllowInsert(const QuicklistNode& node, size_t encoded_value_size) const;
    bool NodeAllowMerge(const QuicklistNode& a, const QuicklistNode& b) const;
    void MaybeSplit(NodeList::iterator node);   // 节点满时拆分
    void MaybeMerge(NodeList::iterator node);   // 节点太空时合并相邻节点
    size_t FillByteLimit() const;               // fill 为 -1..-5 时映射到 4KB..64KB
};
```

### 5.3 迭代器设计

```cpp
class Quicklist::Iterator {
public:
    std::optional<Listpack::Value> Value() const;
    std::optional<std::string> StringValue() const;
    std::optional<int64_t> IntValue() const;
    bool Next();
    bool Prev();
    size_t Index() const;  // 全局索引
};
```

迭代器持有 `std::list` 节点迭代器和节点内偏移，支持前后移动。Quicklist 修改会使相关迭代器失效；Phase 1 不承诺安全迭代器。

### 5.4 关键算法

**Seek（定位到全局 index）**:
```
Seek(index):
  // 优化：判断从 head 还是 tail 开始更近
  if index < count / 2:
    从 head 开始正向遍历
  else:
    从 tail 开始反向遍历
```

**PushHead**:
```
PushHead(value):
  encoded_size = Listpack::EncodedEntrySize(value)
  if nodes_ is empty or !NodeAllowInsert(nodes_.front(), encoded_size):
    NewNodeBefore(nodes_.begin())
  nodes_.front().lp.Prepend(value)
  nodes_.front().count++
  count_++
```

**PopTail**:
```
PopTail():
  if nodes_ is empty → return nullopt
  value = nodes_.back().lp.Get(nodes_.back().lp.Size() - 1)
  result = value->ToString()
  nodes_.back().lp.Delete(nodes_.back().lp.Size() - 1)
  nodes_.back().count--
  count_--
  if nodes_.back().count == 0:
    nodes_.pop_back()
  return result
```

**fill 因子**:
```
fill >= 0: 每个节点最多 fill 个 entry
fill == -1: 每个节点目标最大约 4KB
fill == -2: 每个节点目标最大约 8KB（默认）
fill == -3: 每个节点目标最大约 16KB
fill == -4: 每个节点目标最大约 32KB
fill == -5: 每个节点目标最大约 64KB
```

字节型 fill 必须基于插入后的 listpack `DataSize()` 判断，不能只按 entry 数判断。

### 5.5 测试要点

- Push/Pop 两端操作
- 节点分裂（fill entry 数边界和 fill 字节边界）
- 节点合并（删除导致的空节点）
- `fill = -1..-5` 的字节限制映射
- 索引访问与修改
- 前后插入
- 范围删除
- 迭代器前后遍历
- 空 quicklist 操作
- 大量数据性能（>10000 entry）
- 整数编码 entry 的 Pop/Get 返回 RESP 字符串语义

### 5.6 Redis 参考

- `../redis/src/quicklist.c` — quicklistPushHead/Tail, quicklistPop, quicklistInsertBefore/After, quicklistDelEntry, quicklistGetIteratorAtIdx 等

---

## 6. Skiplist — 泛型跳表

**文件**: `src/ds/skiplist.h` (header-only 模板)

### 6.1 设计要点

| 特性 | 说明 |
|------|------|
| 随机层高 | 概率 1/4 递增，最大 32 层（与 Redis `ZSKIPLIST_MAXLEVEL` 一致） |
| 模板化 | `template<typename Key, typename Score, typename ScoreCompare, typename KeyCompare>` |
| 泛型 | 不直接持有 ZSet dict，但排序语义按 `(score, key)` |
| 跨度 | 每层记录 `span`（跨越的节点数），支持 O(logN) 排名查询 |

### 6.2 模板参数

```cpp
template <
    typename Key,
    typename Score,
    typename ScoreCompare = std::less<Score>,
    typename KeyCompare = std::less<Key>
>
class Skiplist;
```

### 6.3 核心 API

```cpp
// 节点结构
struct SkiplistNode {
    Key key;
    Score score;
    SkiplistNode* backward;        // 后退指针（第 0 层），非拥有指针
    struct Level {
        SkiplistNode* forward;     // 前进指针，非拥有指针
        size_t span;              // 跨度（到 forward 的距离）
    };
    std::vector<Level> levels;     // levels.size() = 随机高度
};

class Skiplist {
public:
    Skiplist();

    // 增删查
    SkiplistNode* Insert(Score score, Key key);       // 返回新节点；调用方负责避免重复 key
    bool Delete(Score score, const Key& key);         // 按 score+key 删除
    bool DeleteNode(SkiplistNode* node);              // 内部使用，按指针删除

    // 排名
    std::optional<size_t> GetRank(Score score, const Key& key) const;  // 1-based rank，匹配 Redis
    SkiplistNode* GetByRank(size_t rank) const;                        // 1-based rank

    // 范围查询
    struct RangeSpec {
        Score min;
        Score max;
        bool min_exclusive;
        bool max_exclusive;
    };
    bool ScoreInRange(const RangeSpec& range) const;
    SkiplistNode* FirstInRange(const RangeSpec& range) const;
    SkiplistNode* LastInRange(const RangeSpec& range) const;

    // 信息
    size_t Size() const;
    static int RandomLevel();       // 生成随机层高，范围 1..32

    // 遍历
    SkiplistNode* First() const;
    SkiplistNode* Tail() const;

    // 删除范围
    template <typename OnDelete>
    size_t DeleteRangeByScore(const RangeSpec& range, OnDelete on_delete);
    template <typename OnDelete>
    size_t DeleteRangeByRank(size_t start, size_t end, OnDelete on_delete);

private:
    static constexpr int kMaxLevel = 32;
    static constexpr double kProbability = 0.25;

    std::unique_ptr<SkiplistNode> header_;               // 头节点，最高 32 层，不含数据
    std::vector<std::unique_ptr<SkiplistNode>> nodes_;    // 拥有普通节点；forward/backward 为观察指针
    SkiplistNode* tail_;
    size_t size_;
    int max_level_;          // 当前最大层高
    ScoreCompare score_compare_;
    KeyCompare key_compare_;
};
```

### 6.4 关键算法

**插入**:
```
Insert(score, key):
  level = RandomLevel()
  // 记录每层的前驱节点和排名
  update[32], rank[32]
  x = header_
  for i in max_level_-1 down to 0:
    rank[i] = i == max_level_-1 ? 0 : rank[i+1]
    while x->levels[i].forward exists and Compare(score, key, x->levels[i].forward):
      rank[i] += x->levels[i].span
      x = x->levels[i].forward
    update[i] = x

  // 创建节点
  new_node = std::make_unique<SkiplistNode>(key, score, level)
  raw = new_node.get()
  nodes_.push_back(std::move(new_node))

  // 插入各层
  for i in 0..level-1:
    raw->levels[i].forward = update[i]->levels[i].forward
    update[i]->levels[i].forward = raw
    raw->levels[i].span = update[i]->levels[i].span - (rank[0] - rank[i])
    update[i]->levels[i].span = rank[0] - rank[i] + 1

  // 后退指针（仅 level 0）
  raw->backward = update[0]
  if raw->levels[0].forward:
    raw->levels[0].forward->backward = raw
  else:
    tail_ = raw

  size_++
  return raw
```

**随机层高**:
```
RandomLevel():
  level = 1
  while (random() & 0xFFFF) < (0.25 * 0xFFFF) and level < 32:
    level++
  return level
```

**排序与唯一性**:
- skiplist 的有序关系是 `(score asc, key asc)`，score 相等时必须用 `KeyCompare` 打破平局。
- `Insert()` 不检查 key 是否已存在。ZSet 类型层必须用 `Dict<Key, SkiplistNode*>` 或等价结构维护唯一性，并在更新 score 时先删旧节点再插新节点。
- 因为 skiplist 不是按 key 建索引，`Find(key)` 不提供 O(logN) 语义，Phase 1 不暴露该接口。
- `DeleteRangeByScore()` / `DeleteRangeByRank()` 接收回调 `on_delete(key, score)`，调用方在回调中同步删除 ZSet dict。

### 6.5 测试要点

- 基本 Insert/Delete/正反向遍历
- 排名查询（GetRank/GetByRank）
- 范围查询和范围删除（开闭区间）
- 随机层高分布验证
- 大数量（>10000）性能
- 同分（多个相同 score）的排序
- 空跳表操作
- 连续插入和删除
- ZSet dict + skiplist 联合测试：重复 key 更新 score 后 dict 与 skiplist 保持一致

### 6.6 Redis 参考

- `../redis/src/t_zset.c` — zslInsert, zslDelete, zslGetRank, zslGetElementByRank, zslDeleteRangeByScore/Rank 等
- `../redis/src/server.h` — zskiplistNode, zskiplist 结构定义

---

## 7. 构建集成

### 7.1 CMake 目标

```cmake
# 主 CMakeLists.txt
add_library(miniredis_ds STATIC
    src/ds/listpack.cpp
    src/ds/intset.cpp
    src/ds/quicklist.cpp
)
target_include_directories(miniredis_ds PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(miniredis_ds PUBLIC cxx_std_20)

target_link_libraries(miniredis PRIVATE miniredis_ds stdexec pthread)

if(MINIREDIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

```cmake
# tests/CMakeLists.txt
add_executable(miniredis_ds_test
    ds/dict_test.cpp
    ds/listpack_test.cpp
    ds/intset_test.cpp
    ds/quicklist_test.cpp
    ds/skiplist_test.cpp
)
target_link_libraries(miniredis_ds_test PRIVATE miniredis_ds GTest::gtest_main)
target_compile_features(miniredis_ds_test PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(miniredis_ds_test)
```

### 7.2 文件清单

```
src/ds/
├── dict.h              # header-only
├── listpack.h
├── listpack.cpp
├── intset.h
├── intset.cpp
├── quicklist.h
├── quicklist.cpp
├── skiplist.h          # header-only
└── ds_common.h         # 公共类型、常量、小端读写 helper

tests/ds/
├── dict_test.cpp
├── listpack_test.cpp
├── intset_test.cpp
├── quicklist_test.cpp
└── skiplist_test.cpp

tests/fixtures/
├── listpack_*.bin       # Redis 生成的 listpack golden fixture
└── intset_*.bin         # Redis 生成的 intset golden fixture
```

---

## 8. 编码规范与质量要求

遵循 `CLAUDE.md` 中的编码规则：

- **Google C++ Style**，由 `.clang-format` 自动格式化
- 命名：类型 `PascalCase`，函数 `PascalCase`，变量 `snake_case`，常量 `kCamelCase`
- 头文件使用 `#pragma once`
- `const` 正确性：不修改的方法标记 `const`
- 不用裸 `new`/`delete`：dict/skiplist 节点等用 `std::unique_ptr`
- 优先 `std::string_view` 作为参数传递，`std::span` 作为缓冲区视图
- `noexcept` 只标注确实不会分配内存、不会校验失败、不会抛异常的方法

### 8.1 关于 new/delete 的特别说明

节点和连续内存的生命周期策略：
- Dict: bucket 使用 `std::vector<std::unique_ptr<DictEntry>>`，链表的 `next` 也是 `std::unique_ptr<DictEntry>`；rehash 通过移动 `unique_ptr` 转移节点所有权
- Listpack: 使用 `std::vector<uint8_t>` 托管 Redis 兼容字节布局
- Intset: 使用 `std::vector<uint8_t>` 托管 Redis 兼容 intset blob
- Quicklist: 使用 `std::list<QuicklistNode>` 托管节点，节点内直接持有 `Listpack`
- Skiplist: 普通节点由 `std::unique_ptr<SkiplistNode>` 的拥有容器托管，forward/backward 指针只作为非拥有拓扑指针

禁止在实现中直接写裸 `new` / `delete`。如果某处确实需要自定义分配策略，必须封装在 RAII 类型中，并增加释放路径测试。

---

## 9. 实现时间估算

| 数据结构 | 预估工作量 | 说明 |
|---------|-----------|------|
| dict | 较大 | 双表 rehash、迭代器、模板设计 |
| listpack | 中等 | 编码/解码逻辑、顺序扫描 |
| intset | 中等 | Redis 兼容编码升级、字节布局校验 |
| quicklist | 中等 | 链表操作 + listpack 集成 |
| skiplist | 中等 | 插入/删除算法、rank 计算 |

---

## 10. 成功标准

- [ ] 所有数据结构通过各自的单元测试
- [ ] 测试覆盖：正常路径、边界条件、错误路径
- [ ] `ctest --output-on-failure` 全绿
- [ ] `ctest -N` 能列出 ds 相关测试，避免“测试未接入但 ctest 通过”
- [ ] `clang-format --dry-run --Werror -style=Google` 通过
- [ ] 编译零警告（`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`）
- [ ] ASan/UBSan 清洁（`MINIREDIS_USE_SANITIZERS=ON`）
- [ ] listpack/intset golden fixture 与 Redis 字节格式一致
