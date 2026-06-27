# MiniRedis Phase 2: 类型系统实现计划

> 状态: 已根据审查意见修订 | 日期: 2026-06-27

## 概述

Phase 2 在 Phase 1 的五种基础数据结构之上，构建 Redis 的类型系统。核心是 `std::variant` 双层设计——**外层区分类型，内层区分编码**。

本阶段不涉及命令实现，仅定义类型及其操作接口。命令层（Phase 4）通过本阶段提供的 API 操作数据。

---

## 1. 架构概览

```
                        Value (std::variant)
                       /    |     |     \     \
                String  List  Set  Hash   ZSet
                  |       |    |     |      |
              [int64|  [quick  [intset|  [listpack|
               string]  list]  Dict-set   Dict-map   [listpack|
                                                       ZSetSkiplist]
```

每层的职责：

| 层 | 职责 | 例子 |
|----|------|------|
| **Value** (顶层 variant) | 类型分派——判断 "这是什么类型的 key" | `TYPE` 命令 |
| **Type Value** (内层 variant) | 编码分派——判断 "用什么编码存储" | `OBJECT ENCODING` 命令 |
| **Data Structure** (Phase 1) | 实际存储和操作 | dict, listpack, skiplist 等 |

---

## 2. 文件结构

```
src/types/
├── value.h                 # Value variant 顶层定义 + type/encoding 枚举
├── string_value.h/cpp      # StringValue: int64 | std::string
├── list_value.h/cpp        # ListValue: Quicklist
├── set_value.h/cpp         # SetValue: Intset | Dict<string,monostate>
├── hash_value.h/cpp        # HashValue: Listpack | Dict<string,string>
├── zset_value.h/cpp        # ZSetValue: Listpack | ZSetSkiplist
├── operation_result.h      # 类型层错误码 + TypeResult<T>
├── numeric_parse.h/cpp     # canonical integer / finite double 解析 helper
├── encoding_thresholds.h   # 编码升级阈值配置
└── type_conversion.h/cpp   # 通用编码转换 helper（listpack ↔ hashtable 等）
```

`ZSetSkiplist` 作为 ZSet 专用的 skiplist+dict 组合结构，定义在 `zset_value.h` 中。

---

## 3. 顶层 Value 定义

### 3.1 类型与编码枚举

```cpp
// value.h
enum class ValueType : uint8_t {
    kString = 0,
    kList   = 1,
    kSet    = 2,
    kHash   = 3,
    kZSet   = 4,
};

enum class ValueEncoding : uint8_t {
    // String
    kIntEmbed = 0,   // int64_t
    kRaw      = 1,   // std::string

    // List
    kQuicklist = 2,

    // Set
    kIntset   = 3,
    kHashtable = 4,  // Dict<string, monostate>

    // Hash
    kListpack  = 5,
    kHashHT    = 6,  // Dict<string,string>

    // ZSet
    kZSetListpack = 7,
    kSkiplist     = 8,
};
```

### 3.2 Value variant

```cpp
using Value = std::variant<
    StringValue,
    ListValue,
    SetValue,
    HashValue,
    ZSetValue
>;

// 辅助访问器
ValueType GetType(const Value& v);
ValueEncoding GetEncoding(const Value& v);
std::string_view TypeName(ValueType type);
```

### 3.3 对象语义：move-only

Phase 2 的所有类型值统一采用 **move-only** 语义：

```cpp
class StringValue {
public:
    StringValue(const StringValue&) = delete;
    StringValue& operator=(const StringValue&) = delete;
    StringValue(StringValue&&) noexcept = default;
    StringValue& operator=(StringValue&&) noexcept = default;
};
```

同样规则适用于 `ListValue`、`SetValue`、`HashValue`、`ZSetValue` 和顶层 `Value`。这样可以避免 `Dict`、`Skiplist` 这类拥有内部节点的结构被隐式复制，也避免 ZSet 中非拥有指针被复制后悬空。

前置要求：
- `ds::Dict` 显式删除 copy，并提供 `noexcept` move constructor / move assignment。
- `ds::Skiplist` 显式删除 copy，并提供 `noexcept` move constructor / move assignment。
- `Value` 可以在数据库哈希表中移动、替换、返回，但不提供隐式复制。后续如 RDB/AOF 或命令层需要复制语义，必须通过显式 `Clone()`/序列化路径实现，而不是启用 copy constructor。

### 3.4 类型层 Result

类型层需要区分“业务上的 false/null”和“操作错误”。例如成员不存在是正常结果，整数解析失败或溢出是错误。为此新增轻量 result 类型：

```cpp
// operation_result.h
enum class TypeError : uint8_t {
    kInvalidInteger,
    kIntegerOverflow,
    kInvalidFloat,
    kInvalidScore,
    kOutOfRange,
};

template <typename T>
using TypeResult = std::variant<T, TypeError>;
```

使用约定：
- 正常返回 `T`，错误返回 `TypeError`。
- `std::optional<T>` 继续表示“key/member/field 不存在”这类正常缺失。
- 命令层负责把 `TypeError` 映射为 RESP error。
- `bool` 仍可用于“是否新增/是否删除/是否存在变化”的正常结果；只有可能失败的解析、溢出、非法 score 等路径使用 `TypeResult<T>`。

### 3.5 数字解析与字节语义

`src/types/numeric_parse.h` 提供类型层共享的解析 helper：

```cpp
struct ParsedInt {
    int64_t value;
};

TypeResult<ParsedInt> ParseCanonicalInt(std::string_view input);
TypeResult<int64_t> AddChecked(int64_t lhs, int64_t rhs);
TypeResult<double> ParseFiniteDouble(std::string_view input);
TypeResult<double> AddFiniteDouble(double lhs, double rhs);
std::string FormatDoubleForStorage(double value);
```

规则：
- `ParseCanonicalInt` 与 Phase 1 `Listpack` 的整数判定保持一致：只接受不改变字节语义的十进制表示。`"0"`、`"-1"`、`"123"` 可以编码为整数；`"001"`、`"+1"`、`"-0"`、空字符串、越界整数必须保留为 raw string 或在计数命令中返回错误。
- `StringValue::Set` 只有在 `ParseCanonicalInt` 成功时才使用 `kIntEmbed`。否则必须保留原始字节到 `kRaw`，`ToString()` 不得改变用户写入的字符串。
- `IncrementBy` / `HINCRBY` 使用 `ParseCanonicalInt` 和 `AddChecked`，解析失败返回 `TypeError::kInvalidInteger`，溢出返回 `TypeError::kIntegerOverflow`。
- `IncrementByFloat` / `HINCRBYFLOAT` / `ZINCRBY` 使用 finite double 规则，拒绝 NaN 和 +/-Inf。浮点结果写回时通过 `FormatDoubleForStorage` 生成稳定字符串，避免平台默认格式带来测试不稳定。

---

## 4. StringValue

**文件**: `src/types/string_value.h` + `string_value.cpp`

### 4.1 设计

| 编码 | 存储 | 条件 |
|------|------|------|
| `kIntEmbed` | `int64_t` | 值可解析为 canonical integer 且不超过 int64 范围时自动选择 |
| `kRaw` | `std::string` | 所有其他情况 |

**编码选择策略**：当通过 `Set(std::string_view)` 写入时，自动尝试将字符串解析为 canonical integer。如果成功，使用 int64 编码；否则使用 raw。canonical integer 规则应与 Phase 1 `Listpack` 的整数解析保持一致，避免把 `"001"`、`"+1"`、`"-0"` 等用户字节规范化成不同的字符串。

```cpp
class StringValue {
public:
    using Storage = std::variant<int64_t, std::string>;

    StringValue();
    explicit StringValue(std::string_view value);  // 自动选择编码
    explicit StringValue(int64_t value);

    // 读取
    std::string ToString() const;                  // 总是返回字符串表示
    std::optional<std::string_view> AsString() const; // 仅 raw 编码有效
    std::optional<int64_t> AsInt() const;         // 仅 int 编码返回有效值
    ValueEncoding Encoding() const;
    size_t Length() const;                         // strlen / STRLEN 命令

    // 修改
    void Set(std::string_view value);              // 重新选择编码
    void Append(std::string_view suffix);          // APPEND — 必然转为 raw
    void SetRange(size_t offset, std::string_view value);  // SETRANGE
    std::string GetRange(long long start, long long end) const; // GETRANGE

    // 原子计数（仅在 int 编码时高效，raw 时需先解析）
    TypeResult<int64_t> IncrementBy(int64_t delta);
    TypeResult<double> IncrementByFloat(double delta);

    // 比较
    bool operator==(const StringValue& other) const;

private:
    Storage value_;
};
```

### 4.2 编码转换行为

- `Set(std::string_view)`：重新解析决定编码
- `Append`：如果当前是 int 编码，先转为 raw，再追加
- `IncrementBy` / `IncrementByFloat`：如果当前是 raw 编码，先尝试解析为整数/浮点数
- 不存在 "升级" 概念，只有 int ↔ raw 的按需转换

---

## 5. ListValue

**文件**: `src/types/list_value.h` + `list_value.cpp`

### 5.1 设计

List 只有一种编码：quicklist。这是最简单的类型。

```cpp
class ListValue {
public:
    ListValue();

    // 两端操作
    void PushHead(std::string_view value);
    void PushTail(std::string_view value);
    std::optional<std::string> PopHead();
    std::optional<std::string> PopTail();

    // 索引操作
    std::optional<std::string> Get(long long index) const;  // 支持负索引
    bool Set(long long index, std::string_view value);       // LSET
    std::optional<size_t> Find(std::string_view value) const; // LPOS

    // 范围操作
    std::vector<std::string> Range(long long start, long long stop) const;
    bool Trim(long long start, long long stop);

    // 插入/删除
    bool InsertBefore(std::string_view pivot, std::string_view value);  // LINSERT
    bool InsertAfter(std::string_view pivot, std::string_view value);
    size_t Remove(long long count, std::string_view value);  // LREM

    // 信息
    size_t Size() const;
    ValueEncoding Encoding() const;
    bool Empty() const;

private:
    Quicklist list_;
};
```

### 5.2 负索引处理

按照 Redis 语义，`-1` 表示最后一个元素，`-N` 表示倒数第 N 个。需在内部做 `index + size()` 转换，越界的：
- `Get` 返回 `nullopt`
- `Set` 返回 `false`
- `Range` 裁剪到 `[0, size-1]`

---

## 6. SetValue

**文件**: `src/types/set_value.h` + `set_value.cpp`

### 6.1 设计

| 编码 | 存储 | 条件 |
|------|------|------|
| `kIntset` | `Intset` | 所有元素都是 canonical integer 且元素数 ≤ 阈值 |
| `kHashtable` | `Dict<std::string, std::monostate>` | 包含非 canonical integer 元素或超过阈值 |

```cpp
class SetValue {
public:
    explicit SetValue(EncodingThresholds thresholds = {});

    // 增删查
    bool Add(std::string_view member);
    bool Remove(std::string_view member);
    bool Contains(std::string_view member) const;
    std::optional<std::string> Pop();             // SPOP — 随机弹出

    // 信息
    size_t Size() const;
    ValueEncoding Encoding() const;
    std::vector<std::string> Members() const;     // SMEMBERS
    std::optional<std::string> RandomMember() const;

    // 集合运算（返回新集合，不修改自身）
    SetValue Union(const SetValue& other) const;
    SetValue Intersect(const SetValue& other) const;
    SetValue Difference(const SetValue& other) const;

    // 移动成员到另一个集合
    static bool Move(SetValue& from, SetValue& to, std::string_view member);

    // 迭代（用于 SSCAN）
    std::vector<std::string> Scan(size_t cursor, size_t count) const;

private:
    using Hashtable = Dict<std::string, std::monostate>;
    using Storage = std::variant<Intset, Hashtable>;
    Storage encoding_;
    EncodingThresholds thresholds_;

    void MaybeUpgrade();  // intset → hashtable 升级检查
};
```

### 6.2 编码升级

- 每次 `Add` 后检查：如果当前是 intset 且元素数 > `thresholds_.set_max_intset_entries`，触发升级
- 升级过程：遍历 intset → `std::to_string(v)` → 插入 `Dict<std::string, std::monostate>`
- 编码永远不会降级（与 Redis 一致）
- hashtable 编码必须使用 Phase 1 的 `ds::Dict`，不使用 `std::unordered_set`。`std::monostate` 仅作为占位 value，集合成员存储在 dict key 中。

---

## 7. HashValue

**文件**: `src/types/hash_value.h` + `hash_value.cpp`

### 7.1 设计

| 编码 | 存储 | 条件 |
|------|------|------|
| `kListpack` | `Listpack` | 字段数 ≤ entries 阈值且每个 field/value 长度 ≤ value 阈值 |
| `kHashHT` | `Dict<std::string, std::string>` | 超过阈值 |

listpack 中数据以 `[field1, value1, field2, value2, ...]` 顺序存储。

```cpp
class HashValue {
public:
    explicit HashValue(EncodingThresholds thresholds = {});

    // CRUD
    bool Set(std::string_view field, std::string_view value);   // HSET
    bool SetNX(std::string_view field, std::string_view value); // HSETNX
    std::optional<std::string> Get(std::string_view field) const;
    bool Delete(std::string_view field);                        // HDEL
    bool Exists(std::string_view field) const;                  // HEXISTS

    // 批量
    std::vector<std::optional<std::string>> MGet(const std::vector<std::string>& fields) const;

    // 信息
    size_t Size() const;                          // HLEN
    size_t FieldLength(std::string_view field) const;  // HSTRLEN
    ValueEncoding Encoding() const;

    // 遍历
    std::vector<std::string> Keys() const;        // HKEYS
    std::vector<std::string> Values() const;      // HVALS
    std::vector<std::pair<std::string, std::string>> GetAll() const;  // HGETALL

    // 原子计数
    TypeResult<int64_t> IncrementBy(std::string_view field, int64_t delta);
    TypeResult<double> IncrementByFloat(std::string_view field, double delta);

    // 随机/扫描
    std::optional<std::string> RandomField() const;
    std::vector<std::string> Scan(size_t cursor, size_t count) const;

private:
    using Hashtable = Dict<std::string, std::string>;
    using Storage = std::variant<Listpack, Hashtable>;
    Storage encoding_;
    EncodingThresholds thresholds_;

    void MaybeUpgrade(size_t new_field_size, size_t new_value_size);
    void ConvertToHashtable();
};
```

hashtable 编码必须使用 Phase 1 的 `ds::Dict`，不使用 `std::unordered_map`。`HINCRBY` / `HINCRBYFLOAT` 通过 `TypeResult` 返回解析失败、溢出或非法浮点数错误；字段不存在时按 Redis 语义以 `0` 为旧值继续计算。
listpack 编码升级条件使用 `thresholds_.hash_max_listpack_entries` 和 `thresholds_.hash_max_listpack_value`：新增或更新后字段数超过 entries 阈值，或 field/value 任一字符串长度超过 value 阈值，立即转换为 `Dict`。升级后不降级。

### 7.2 listpack 布局特点

Hash listpack 的查找必须顺序扫描（O(N)）：
```
listpack: [field1, val1, field2, val2, ...]
```
查找 field 时，只检查偶数索引（0, 2, 4...）。一旦找到匹配的 field，下一个 entry 就是 value。

---

## 8. ZSetValue

**文件**: `src/types/zset_value.h` + `zset_value.cpp`

### 8.1 设计

| 编码 | 存储 | 条件 |
|------|------|------|
| `kZSetListpack` | `Listpack` | 元素数 ≤ entries 阈值且每个 member 长度 ≤ value 阈值 |
| `kSkiplist` | `ZSetSkiplist` | 超过任一阈值 |

listpack 中数据以 `[ele1, score1_as_string, ele2, score2_as_string, ...]` 存储。

### 8.2 ZSet listpack 布局与排序

ZSet listpack 编码不是插入顺序，而是始终按 `(score, element)` 升序保存：

```text
[element_a, score_a, element_b, score_b, ...]
where (score_a, element_a) <= (score_b, element_b)
```

操作约束：
- `score` 必须是 finite double，写入 listpack 时使用 `FormatDoubleForStorage(score)`。
- `Add(element, score)` 在线性扫描中先查找是否已有 element；若存在且 score 不变，返回 `false`；若存在且 score 改变，删除旧 pair 后按 `(score, element)` 重新插入。
- 新增或更新后如果成员数超过 `thresholds_.zset_max_listpack_entries`，或 element 长度超过 `thresholds_.zset_max_listpack_value`，立即转换为 skiplist 编码。
- `Rank(element)` 在 listpack 编码下返回 pair 的 0-based 位置。
- skiplist 底层 `GetRank(score, key)` 返回 1-based rank，`ZSetValue::Rank` 必须减 1 后暴露给类型层；`RevRank` 使用 `Count() - rank_0_based - 1`。
- 编码升级前后，`Range`、`Rank`、`RevRank` 的结果必须一致。

### 8.3 ZSetSkiplist — skiplist + dict 组合

```cpp
struct ZSetSkiplist {
    using Node = Skiplist<std::string, double>::Node;

    ZSetSkiplist() = default;
    ZSetSkiplist(const ZSetSkiplist&) = delete;
    ZSetSkiplist& operator=(const ZSetSkiplist&) = delete;

    // move 后必须重建 dict，不能移动旧 dict 中的裸指针
    ZSetSkiplist(ZSetSkiplist&& other) noexcept;
    ZSetSkiplist& operator=(ZSetSkiplist&& other) noexcept;

    // skiplist: 按 (score, element) 排序，支持 O(logN) 范围/排名操作
    Skiplist<std::string, double> skiplist;
    // dict: element → SkiplistNode*，支持 O(1) 成员查找
    Dict<std::string, Node*> dict;

private:
    void RebuildDict();
};
```

所有权模型：
- skiplist 拥有所有节点（通过其内部 `nodes_` vector）
- dict 中的 value 是非拥有指针（`SkiplistNode*`），指向 skiplist 中的节点
- 删除 ZSetSkiplist 时，skiplist 析构自动释放所有节点，dict 的指针自动失效
- ZSetSkiplist 禁用 copy。move constructor / move assignment 只移动 skiplist，然后从 `skiplist.First()` 沿 level-0 链表重建 dict；旧 dict 不随对象移动，避免旧指针迁移后指向错误对象。
- `ZSetValue` 的 move 操作依赖 `ZSetSkiplist` 的安全 move。任何包含 skiplist 编码的对象移动后，都必须通过测试验证 `Score`、`Rank`、`Remove` 仍能同时命中 skiplist 与 dict。

move 实现约束：
```cpp
ZSetSkiplist::ZSetSkiplist(ZSetSkiplist&& other) noexcept
    : skiplist(std::move(other.skiplist)), dict() {
    RebuildDict();
    other.dict.Clear();
}

ZSetSkiplist& ZSetSkiplist::operator=(ZSetSkiplist&& other) noexcept {
    if (this == &other) return *this;
    dict.Clear();
    skiplist = std::move(other.skiplist);
    RebuildDict();
    other.dict.Clear();
    return *this;
}
```

`RebuildDict()` 遍历 `skiplist.First()` 到 level-0 尾部，对每个节点执行 `dict.Set(node->key, node)`。

### 8.4 ZSetValue API

```cpp
class ZSetValue {
public:
    explicit ZSetValue(EncodingThresholds thresholds = {});

    // 增删改
    TypeResult<bool> Add(std::string_view element, double score); // ZADD
    bool Remove(std::string_view element);                // ZREM
    TypeResult<double> Update(std::string_view element, double delta); // ZINCRBY

    // 查询
    std::optional<double> Score(std::string_view element) const;  // ZSCORE
    std::optional<size_t> Rank(std::string_view element) const;   // ZRANK (0-based)
    std::optional<size_t> RevRank(std::string_view element) const; // ZREVRANK
    size_t Count() const;                                          // ZCARD
    size_t CountByScore(double min, double max, bool min_ex, bool max_ex) const;  // ZCOUNT
    size_t LexCount(std::string_view min, std::string_view max, bool min_ex, bool max_ex) const;

    // 范围查询
    struct RangeResult {
        std::string element;
        double score;
    };
    std::vector<RangeResult> Range(long long start, long long stop) const;  // ZRANGE
    std::vector<RangeResult> RevRange(long long start, long long stop) const;
    std::vector<RangeResult> RangeByScore(double min, double max, bool min_ex, bool max_ex,
                                          long long offset = 0, long long count = -1) const;
    std::vector<RangeResult> RangeByLex(std::string_view min, std::string_view max,
                                        bool min_ex, bool max_ex, long long offset = 0,
                                        long long count = -1) const;

    // 弹出
    std::optional<RangeResult> PopMin();  // ZPOPMIN
    std::optional<RangeResult> PopMax();  // ZPOPMAX

    // 范围删除
    size_t RemoveRangeByRank(long long start, long long stop);
    size_t RemoveRangeByScore(double min, double max, bool min_ex, bool max_ex);
    size_t RemoveRangeByLex(std::string_view min, std::string_view max, bool min_ex, bool max_ex);

    // 集合运算
    void Union(const ZSetValue& other, std::vector<double> weights, const std::string& aggregate);
    void Intersect(const ZSetValue& other, std::vector<double> weights, const std::string& aggregate);

    // 信息
    ValueEncoding Encoding() const;

    // 遍历
    std::vector<RangeResult> Scan(size_t cursor, size_t count) const;

private:
    using Storage = std::variant<Listpack, ZSetSkiplist>;
    Storage encoding_;
    EncodingThresholds thresholds_;

    void MaybeUpgrade();
    void ConvertToSkiplist();
};
```

---

## 9. 编码阈值配置

**文件**: `src/types/encoding_thresholds.h`

```cpp
struct EncodingThresholds {
    // Set
    size_t set_max_intset_entries = 512;

    // Hash
    size_t hash_max_listpack_entries = 512;
    size_t hash_max_listpack_value = 64;

    // ZSet
    size_t zset_max_listpack_entries = 128;
    size_t zset_max_listpack_value = 64;
};
```

阈值通过 `EncodingThresholds` 在构造 `SetValue`、`HashValue`、`ZSetValue` 时传入（默认值即可），后续 Config/redis.conf 可覆盖。类型对象持有一份配置快照，编码升级只读取自身的 `thresholds_`，避免全局配置变更影响已存在对象的内部不变量。

构造约定：
```cpp
SetValue set(EncodingThresholds{});
HashValue hash(EncodingThresholds{});
ZSetValue zset(EncodingThresholds{});
```

升级条件：
- Set: 当前为 intset 且新增后成员数超过 `set_max_intset_entries`，或新增成员不是 canonical integer。
- Hash: 当前为 listpack 且 field 数超过 `hash_max_listpack_entries`，或任一 field/value 长度超过 `hash_max_listpack_value`。
- ZSet: 当前为 listpack 且 member 数超过 `zset_max_listpack_entries`，或任一 member 长度超过 `zset_max_listpack_value`。
- 所有类型升级后不降级。

---

## 10. 编码转换通用 helper

**文件**: `src/types/type_conversion.h` + `type_conversion.cpp`

提供 listpack ↔ `ds::Dict` / ZSet 组合结构的转换工具：

```cpp
using SetHashtable = Dict<std::string, std::monostate>;
using HashHashtable = Dict<std::string, std::string>;

// intset → Dict<string, monostate>
SetHashtable IntsetToSetHashtable(const Intset& intset);

// listpack → Dict<string, string>
// 注意 lp 格式: [field1, val1, field2, val2, ...]
HashHashtable ListpackToHashDict(const Listpack& lp);

// listpack → ZSetSkiplist
// 注意 lp 格式: [ele1, score_str1, ele2, score_str2, ...]
ZSetSkiplist ListpackToZSetSkiplist(Listpack&& lp);
```

这些转换遵循 "读旧→拷贝→写新→释旧" 模式。读取 listpack 时优先使用 `string_view` 避免额外拷贝；如果 entry 是整数编码，则必须通过 `Listpack::Value::ToString()` 生成字符串，不能假设所有 entry 都有可借用的字符串视图。
`ListpackToZSetSkiplist` 必须按 listpack 中已有的 `(score, element)` 顺序插入 skiplist，并在插入后由 `ZSetSkiplist::RebuildDict()` 建立 element → node 映射；如果解析到非 finite score，返回 `TypeError::kInvalidScore` 的路径应由调用方在转换前完成校验。

---

## 11. ZSetSkiplist 操作细节

### 11.1 插入 (ZADD)

```
Add(element, score):
  0. 如果 score 不是 finite double → return TypeError::kInvalidScore
  1. dict.Find(element) → 如果存在:
     a. 旧节点 = *dict.Find(element)
     b. 如果 score 相同 → return false (不更新)
     c. skiplist.DeleteNode(旧节点)
  2. 新节点 = skiplist.Insert(score, element)
  3. dict.Set(element, 新节点)
  → return true
```

### 11.2 删除 (ZREM)

```
Remove(element):
  1. dict.Find(element) → 如果不存在 → return false
  2. 节点 = *dict.Find(element)
  3. skiplist.DeleteNode(节点)
  4. dict.Delete(element)
  → return true
```

### 11.3 更新分数 (ZINCRBY)

```
Update(element, delta):
  0. 如果 delta 不是 finite double → return TypeError::kInvalidScore
  1. dict.Find(element) → 如果不存在:
     a. 新分数 = delta
     b. 新节点 = skiplist.Insert(新分数, element)
     c. dict.Set(element, 新节点)
     d. return 新分数
  2. 旧节点 = *dict.Find(element)
  3. 新分数 = 旧节点->score + delta
  4. 如果新分数不是 finite double → return TypeError::kInvalidScore
  5. skiplist.DeleteNode(旧节点)
  6. 新节点 = skiplist.Insert(新分数, element)
  7. dict.Set(element, 新节点)
  → 返回新分数
```

### 11.4 范围删除 (ZREMRANGEBY*)

skiplist 的 `DeleteRangeByScore`/`DeleteRangeByRank` 接收回调，在回调中同步删除 dict：

```cpp
skiplist.DeleteRangeByScore(range, [&](const std::string& key, double) {
    dict.Delete(key);
});
```

---

## 12. 测试策略

### 12.1 测试文件

```
tests/types/
├── numeric_parse_test.cpp
├── string_value_test.cpp
├── list_value_test.cpp
├── set_value_test.cpp
├── hash_value_test.cpp
├── zset_value_test.cpp
└── encoding_upgrade_test.cpp
```

### 12.2 各类型测试覆盖

| 类型 | 测试重点 |
|------|---------|
| numeric_parse | canonical integer 接受/拒绝、整数溢出、finite double、NaN/Inf 拒绝、double 格式化稳定性 |
| StringValue | int/raw 编码自动选择、非 canonical 数字保留原始字节、Append 转 raw、IncrementBy 解析/溢出、SetRange/GetRange |
| ListValue | Push/Pop、负索引、Range/Trim、Insert 前后、LREM 计数、空列表 |
| SetValue | Add/Remove/Contains、非 canonical 成员触发 Dict、intset→Dict entries 阈值升级、集合运算、SPOP、move-only 编译期约束 |
| HashValue | Set/Get/Delete、listpack→Dict entries/value 阈值升级、HINCRBY 错误返回、HKEYS/HVALS |
| ZSetValue | ZADD 去重/更新、listpack 按 (score, element) 排序、rank 0-based、ZINCRBY 错误返回、范围查询、listpack→skiplist 升级、skiplist+dict 一致性、安全 move 后重建 dict |
| encoding_upgrade | 各类型升级边界、value 长度阈值、升级后数据完整性、升级触发阈值 |

---

## 13. 构建集成

```cmake
# CMakeLists.txt 新增
add_library(miniredis_types STATIC
    src/types/numeric_parse.cpp
    src/types/string_value.cpp
    src/types/list_value.cpp
    src/types/set_value.cpp
    src/types/hash_value.cpp
    src/types/zset_value.cpp
    src/types/type_conversion.cpp
)
target_include_directories(miniredis_types PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis_types PUBLIC miniredis_ds)
target_compile_features(miniredis_types PUBLIC cxx_std_20)
```

```cmake
# tests/CMakeLists.txt 新增
add_executable(miniredis_types_test
    types/numeric_parse_test.cpp
    types/string_value_test.cpp
    types/list_value_test.cpp
    types/set_value_test.cpp
    types/hash_value_test.cpp
    types/zset_value_test.cpp
    types/encoding_upgrade_test.cpp
)
target_link_libraries(miniredis_types_test PRIVATE miniredis_types GTest::gtest_main)
target_compile_features(miniredis_types_test PRIVATE cxx_std_20)
gtest_discover_tests(miniredis_types_test)
```

---

## 14. 实现顺序

```
StringValue → ListValue → SetValue → HashValue → ZSetValue
                     ↓           ↓          ↓
              (最简单)    intset↔Dict  listpack↔Dict (最复杂)
```

- **StringValue**: 最简单，两个 variant alternative，无复杂依赖
- **ListValue**: 封装 Quicklist，实现负索引逻辑
- **SetValue**: intset↔Dict 转换
- **HashValue**: listpack↔Dict 转换
- **ZSetValue**: 最复杂 — ZSetSkiplist 组合结构 + 编码转换

每个类型实现完成后立即编写测试（延续 TDD 风格）。

---

## 15. 成功标准

- [ ] 所有类型通过各自单元测试
- [ ] 编码升级测试覆盖：触发边界、数据完整性、升级前后语义一致
- [ ] ZSet skiplist+dict 双重结构一致性测试
- [ ] ZSet listpack 与 skiplist 编码在 `(score, element)` 排序、0-based rank、range 输出上保持一致
- [ ] `static_assert` 覆盖：`Value` 和五种 Type Value 不可 copy、可 move
- [ ] ZSet skiplist 编码对象 move 后，`Score`/`Rank`/`Remove` 仍保持 skiplist 与 dict 一致
- [ ] `TypeResult` 错误路径覆盖：整数解析失败、整数溢出、非法浮点数、非法 score
- [ ] 阈值配置测试覆盖：默认配置、自定义 entries 阈值、自定义 value 长度阈值
- [ ] 字符串字节语义测试覆盖：`"001"`、`"+1"`、`"-0"` 等非 canonical 数字写入后 `ToString()` 原样返回
- [ ] `ctest` 全绿
- [ ] 编译零警告
- [ ] ASan/UBSan 清洁
