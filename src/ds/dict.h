#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace miniredis {
namespace ds {

template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class Dict;

template <typename Key, typename Value>
struct DictEntry {
  Key key;
  Value value;
  std::unique_ptr<DictEntry> next;
};

template <typename Key, typename Value>
struct DictTable {
  std::vector<std::unique_ptr<DictEntry<Key, Value>>> buckets;
  size_t size = 0;
  size_t size_mask = 0;
  size_t used = 0;
};

template <typename Key, typename Value, typename Hash, typename KeyEqual>
class Dict {
 public:
  using Entry = DictEntry<Key, Value>;

  Dict();
  ~Dict();

  // Move-only
  Dict(const Dict&) = delete;
  Dict& operator=(const Dict&) = delete;
  Dict(Dict&& other) noexcept;
  Dict& operator=(Dict&& other) noexcept;

  size_t Size() const;
  size_t Buckets() const;
  bool IsRehashing() const;

  bool Add(Key key, Value value);
  bool Set(Key key, Value value);
  Value* Find(const Key& key);
  const Value* Find(const Key& key) const;
  bool Delete(const Key& key);
  void Clear();

  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entry;
    using pointer = Entry*;
    using reference = Entry&;

    Iterator() = default;

    reference operator*() const { return *current_; }
    pointer operator->() const { return current_; }

    Iterator& operator++();
    Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return current_ == other.current_;
    }
    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    friend class Dict;
    Dict* dict_ = nullptr;
    int table_index_ = 0;
    size_t bucket_index_ = 0;
    Entry* current_ = nullptr;

    Iterator(Dict* dict, int table_index, size_t bucket_index, Entry* current);
    void AdvanceToNext();
  };

  class SafeIterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entry;
    using pointer = Entry*;
    using reference = Entry&;

    SafeIterator();
    ~SafeIterator();

    SafeIterator(const SafeIterator& other);
    SafeIterator& operator=(const SafeIterator& other);
    SafeIterator(SafeIterator&& other) noexcept;
    SafeIterator& operator=(SafeIterator&& other) noexcept;

    reference operator*() const { return *current_; }
    pointer operator->() const { return current_; }

    SafeIterator& operator++();
    SafeIterator operator++(int) {
      SafeIterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const SafeIterator& other) const {
      return dict_ == other.dict_ && current_ == other.current_;
    }
    bool operator!=(const SafeIterator& other) const {
      return !(*this == other);
    }

   private:
    friend class Dict;
    Dict* dict_ = nullptr;
    int table_ = 0;
    size_t bucket_ = 0;
    Entry* current_ = nullptr;  // entry to return on deref
    Entry* next_ =
        nullptr;  // pre-computed next (safe even if current_ is deleted)
    int next_table_ = 0;
    size_t next_bucket_ = 0;

    SafeIterator(Dict* dict);
    void Release();
    void Acquire();
    void ComputeNext();  // pre-compute next_ from the state after current_
  };

  Iterator begin();
  Iterator end();
  SafeIterator SafeBegin();
  SafeIterator SafeEnd();

  int Rehash(int n);
  void RehashStep();

  std::optional<Key> RandomKey();
  std::vector<Entry*> GetSomeKeys(size_t count, uint64_t seed);

 private:
  static constexpr size_t kInitialSize = 4;
  static constexpr int kRehashStepDefault = 1;

  DictTable<Key, Value> ht_[2];
  long rehash_idx_ = -1;
  size_t rehash_step_ = kRehashStepDefault;
  size_t safe_iterators_ = 0;
  Hash hash_;
  KeyEqual key_equal_;

  void MaybeRehashStep();
  size_t HashKey(const Key& key) const;
  static size_t NextPower(size_t size);
  bool ExpandIfNeeded();
  bool ShrinkIfNeeded();
  bool Resize(size_t new_size);

  friend class Iterator;
  friend class SafeIterator;
};

// ===== Implementation =====

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::Dict() {
  ht_[0].size = kInitialSize;
  ht_[0].size_mask = kInitialSize - 1;
  ht_[0].buckets.resize(kInitialSize);
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::~Dict() = default;

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::Dict(Dict&& other) noexcept
    : ht_{std::move(other.ht_[0]), std::move(other.ht_[1])},
      rehash_idx_(other.rehash_idx_),
      rehash_step_(other.rehash_step_),
      safe_iterators_(other.safe_iterators_),
      hash_(std::move(other.hash_)),
      key_equal_(std::move(other.key_equal_)) {
  other.rehash_idx_ = -1;
  other.safe_iterators_ = 0;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>& Dict<Key, Value, Hash, KeyEqual>::operator=(
    Dict&& other) noexcept {
  if (this != &other) {
    ht_[0] = std::move(other.ht_[0]);
    ht_[1] = std::move(other.ht_[1]);
    rehash_idx_ = other.rehash_idx_;
    rehash_step_ = other.rehash_step_;
    safe_iterators_ = other.safe_iterators_;
    hash_ = std::move(other.hash_);
    key_equal_ = std::move(other.key_equal_);
    other.rehash_idx_ = -1;
    other.safe_iterators_ = 0;
  }
  return *this;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
size_t Dict<Key, Value, Hash, KeyEqual>::Size() const {
  return ht_[0].used + ht_[1].used;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
size_t Dict<Key, Value, Hash, KeyEqual>::Buckets() const {
  return ht_[0].size + ht_[1].size;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::IsRehashing() const {
  return rehash_idx_ != -1;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
size_t Dict<Key, Value, Hash, KeyEqual>::HashKey(const Key& key) const {
  return hash_(key);
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
size_t Dict<Key, Value, Hash, KeyEqual>::NextPower(size_t size) {
  if (size == 0) return 0;
  size_t i = 4;  // minimum is 4
  while (i < size) i *= 2;
  return i;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::Resize(size_t new_size) {
  if (IsRehashing() || ht_[0].used > new_size) return false;

  size_t real_size = NextPower(new_size == 0 ? kInitialSize : new_size);
  if (real_size == ht_[0].size) return false;

  ht_[1].size = real_size;
  ht_[1].size_mask = real_size - 1;
  ht_[1].buckets.resize(real_size);
  ht_[1].used = 0;
  rehash_idx_ = 0;
  return true;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::ExpandIfNeeded() {
  if (IsRehashing()) return false;
  if (ht_[0].size == 0) return Resize(kInitialSize);
  if (ht_[0].used >= ht_[0].size) return Resize(ht_[0].used * 2);
  return false;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::ShrinkIfNeeded() {
  if (IsRehashing()) return false;
  // Shrink when load factor < 0.1 and used > kInitialSize
  if (ht_[0].size > kInitialSize && ht_[0].used * 10 < ht_[0].size) {
    return Resize(ht_[0].used);
  }
  return false;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
int Dict<Key, Value, Hash, KeyEqual>::Rehash(int n) {
  if (!IsRehashing()) return 0;

  while (n-- > 0) {
    // Find a non-empty bucket in ht_[0]
    while (rehash_idx_ < static_cast<long>(ht_[0].size) &&
           ht_[0].buckets[static_cast<size_t>(rehash_idx_)] == nullptr) {
      rehash_idx_++;
    }

    if (static_cast<size_t>(rehash_idx_) >= ht_[0].size) {
      // Rehash complete — swap tables
      ht_[0] = std::move(ht_[1]);
      ht_[1] = DictTable<Key, Value>{};
      rehash_idx_ = -1;
      return 0;
    }

    // Move all entries from this bucket to ht_[1]
    auto& bucket = ht_[0].buckets[static_cast<size_t>(rehash_idx_)];
    while (bucket) {
      auto entry = std::move(bucket);
      bucket = std::move(entry->next);

      size_t idx = HashKey(entry->key) & ht_[1].size_mask;
      entry->next = std::move(ht_[1].buckets[idx]);
      ht_[1].buckets[idx] = std::move(entry);

      ht_[0].used--;
      ht_[1].used++;
    }
    rehash_idx_++;
  }
  return 1;  // still rehashing
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::RehashStep() {
  Rehash(static_cast<int>(rehash_step_));
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::MaybeRehashStep() {
  if (IsRehashing() && safe_iterators_ == 0) {
    RehashStep();
  } else {
    ExpandIfNeeded();
    ShrinkIfNeeded();
  }
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::Add(Key key, Value value) {
  MaybeRehashStep();

  // Check if key exists in either table
  size_t h = HashKey(key);

  auto find_in_table = [&](DictTable<Key, Value>& table) -> Entry* {
    if (table.size == 0) return nullptr;
    size_t idx = h & table.size_mask;
    Entry* entry = table.buckets[idx].get();
    while (entry) {
      if (key_equal_(entry->key, key)) return entry;
      entry = entry->next.get();
    }
    return nullptr;
  };

  if (find_in_table(ht_[0]) || find_in_table(ht_[1])) return false;

  // Insert into ht_[1] if rehashing, otherwise ht_[0]
  DictTable<Key, Value>& target = IsRehashing() ? ht_[1] : ht_[0];
  size_t idx = h & target.size_mask;

  auto new_entry = std::make_unique<Entry>();
  new_entry->key = std::move(key);
  new_entry->value = std::move(value);
  new_entry->next = std::move(target.buckets[idx]);
  target.buckets[idx] = std::move(new_entry);
  target.used++;
  return true;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::Set(Key key, Value value) {
  MaybeRehashStep();

  size_t h = HashKey(key);

  // Search ht_[0]
  if (ht_[0].size > 0) {
    size_t idx = h & ht_[0].size_mask;
    Entry* entry = ht_[0].buckets[idx].get();
    while (entry) {
      if (key_equal_(entry->key, key)) {
        entry->value = std::move(value);
        return true;  // Updated existing
      }
      entry = entry->next.get();
    }
  }

  // Search ht_[1]
  if (IsRehashing() && ht_[1].size > 0) {
    size_t idx = h & ht_[1].size_mask;
    Entry* entry = ht_[1].buckets[idx].get();
    while (entry) {
      if (key_equal_(entry->key, key)) {
        entry->value = std::move(value);
        return true;  // Updated existing
      }
      entry = entry->next.get();
    }
  }

  // Not found — insert as new
  DictTable<Key, Value>& target = IsRehashing() ? ht_[1] : ht_[0];
  size_t idx = h & target.size_mask;

  auto new_entry = std::make_unique<Entry>();
  new_entry->key = std::move(key);
  new_entry->value = std::move(value);
  new_entry->next = std::move(target.buckets[idx]);
  target.buckets[idx] = std::move(new_entry);
  target.used++;
  return true;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Value* Dict<Key, Value, Hash, KeyEqual>::Find(const Key& key) {
  // Cast to const and call const version, then cast away const
  const auto* result = static_cast<const Dict*>(this)->Find(key);
  return const_cast<Value*>(result);
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
const Value* Dict<Key, Value, Hash, KeyEqual>::Find(const Key& key) const {
  size_t h = HashKey(key);

  auto find_in_table = [&](const DictTable<Key, Value>& table) -> const Value* {
    if (table.size == 0) return nullptr;
    size_t idx = h & table.size_mask;
    const Entry* entry = table.buckets[idx].get();
    while (entry) {
      if (key_equal_(entry->key, key)) return &entry->value;
      entry = entry->next.get();
    }
    return nullptr;
  };

  if (auto* val = find_in_table(ht_[0])) return val;
  if (IsRehashing()) return find_in_table(ht_[1]);
  return nullptr;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
bool Dict<Key, Value, Hash, KeyEqual>::Delete(const Key& key) {
  if (ht_[0].size == 0 && ht_[1].size == 0) return false;

  if (safe_iterators_ == 0) MaybeRehashStep();

  size_t h = HashKey(key);

  auto delete_from_table = [&](DictTable<Key, Value>& table) -> bool {
    if (table.size == 0) return false;
    size_t idx = h & table.size_mask;
    auto* prev = &table.buckets[idx];
    while (*prev) {
      if (key_equal_((*prev)->key, key)) {
        auto to_delete = std::move(*prev);
        *prev = std::move(to_delete->next);
        table.used--;
        return true;
      }
      prev = &(*prev)->next;
    }
    return false;
  };

  if (delete_from_table(ht_[0])) return true;
  if (IsRehashing() && delete_from_table(ht_[1])) return true;

  // Maybe shrink after deletion
  if (safe_iterators_ == 0 && !IsRehashing()) {
    ShrinkIfNeeded();
  }
  return false;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::Clear() {
  ht_[0] = DictTable<Key, Value>{};
  ht_[1] = DictTable<Key, Value>{};
  ht_[0].size = kInitialSize;
  ht_[0].size_mask = kInitialSize - 1;
  ht_[0].buckets.resize(kInitialSize);
  rehash_idx_ = -1;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
std::optional<Key> Dict<Key, Value, Hash, KeyEqual>::RandomKey() {
  if (Size() == 0) return std::nullopt;

  // Try ht_[0] first
  if (ht_[0].used > 0) {
    for (auto& bucket : ht_[0].buckets) {
      if (bucket) return bucket->key;
    }
  }
  // Then ht_[1] (if rehashing)
  if (IsRehashing() && ht_[1].used > 0) {
    for (auto& bucket : ht_[1].buckets) {
      if (bucket) return bucket->key;
    }
  }
  return std::nullopt;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
std::vector<DictEntry<Key, Value>*>
Dict<Key, Value, Hash, KeyEqual>::GetSomeKeys(size_t count, uint64_t seed) {
  std::vector<Entry*> samples;
  if (Size() == 0) return samples;
  samples.reserve(count);

  auto collect_from_table = [&](DictTable<Key, Value>& table,
                                size_t start_bucket) {
    if (table.size == 0 || samples.size() >= count) return;
    // Use seed-based starting position to avoid always hitting first
    // non-empty buckets (unlike RandomKey).
    size_t b = start_bucket % table.size;
    size_t visited = 0;
    while (visited < table.size && samples.size() < count) {
      auto* entry = table.buckets[b].get();
      while (entry && samples.size() < count) {
        samples.push_back(entry);
        entry = entry->next.get();
      }
      b = (b + 1) % table.size;
      visited++;
    }
  };

  // Distribute sampling across ht_[0] and ht_[1] proportional to used count.
  size_t need = count;
  if (IsRehashing() && ht_[1].used > 0) {
    size_t ht1_count = std::min(
        need, static_cast<size_t>((static_cast<uint64_t>(ht_[1].used) * count) /
                                  Size()));
    if (ht1_count == 0 && need > 0) ht1_count = 1;  // at least try 1
    collect_from_table(ht_[1], static_cast<size_t>(seed));
    if (samples.size() >= count) return samples;
    need = count - samples.size();
  }

  collect_from_table(ht_[0], static_cast<size_t>(seed));
  return samples;
}

// ===== Iterator Implementation =====

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::Iterator::Iterator(Dict* dict,
                                                     int table_index,
                                                     size_t bucket_index,
                                                     Entry* current)
    : dict_(dict),
      table_index_(table_index),
      bucket_index_(bucket_index),
      current_(current) {}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::Iterator::AdvanceToNext() {
  // Move to next entry in the current bucket's chain
  if (current_ && current_->next) {
    current_ = current_->next.get();
    return;
  }

  // Move to next non-empty bucket
  while (table_index_ < 2) {
    auto& table = dict_->ht_[table_index_];
    if (table.size > 0) {
      while (bucket_index_ < table.size) {
        if (table.buckets[bucket_index_]) {
          current_ = table.buckets[bucket_index_].get();
          return;
        }
        bucket_index_++;
      }
    }
    // Move to next table
    table_index_++;
    bucket_index_ = 0;
  }

  // End of both tables
  current_ = nullptr;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::Iterator&
Dict<Key, Value, Hash, KeyEqual>::Iterator::operator++() {
  if (!current_) return *this;

  // Move to next entry in chain, or next bucket
  if (current_->next) {
    current_ = current_->next.get();
  } else {
    bucket_index_++;
    AdvanceToNext();
  }
  return *this;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::Iterator
Dict<Key, Value, Hash, KeyEqual>::begin() {
  Iterator it(this, 0, 0, nullptr);
  it.AdvanceToNext();
  return it;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::Iterator
Dict<Key, Value, Hash, KeyEqual>::end() {
  return Iterator(this, 2, 0, nullptr);
}

// ===== SafeIterator Implementation =====

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::SafeIterator::ComputeNext() {
  if (!dict_) return;

  // If current_ has a chain successor, that's the next entry
  if (current_ && current_->next) {
    next_ = current_->next.get();
    next_table_ = table_;
    next_bucket_ = bucket_;
    return;
  }

  // Advance to next bucket
  size_t b = current_ ? bucket_ + 1 : bucket_;
  int t = table_;

  while (t < 2) {
    auto& ht = dict_->ht_[t];
    if (ht.size > 0) {
      while (b < ht.size) {
        if (ht.buckets[b]) {
          next_ = ht.buckets[b].get();
          next_table_ = t;
          next_bucket_ = b;
          return;
        }
        b++;
      }
    }
    t++;
    b = 0;
  }

  // End reached
  next_ = nullptr;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::SafeIterator()
    : dict_(nullptr) {}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::SafeIterator(Dict* dict)
    : dict_(dict) {
  Acquire();
  // Find first entry
  table_ = 0;
  bucket_ = 0;
  current_ = nullptr;

  while (table_ < 2) {
    auto& ht = dict_->ht_[table_];
    if (ht.size > 0) {
      while (bucket_ < ht.size) {
        if (ht.buckets[bucket_]) {
          current_ = ht.buckets[bucket_].get();
          ComputeNext();
          return;
        }
        bucket_++;
      }
    }
    table_++;
    bucket_ = 0;
  }
  // Empty dict
  next_ = nullptr;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::~SafeIterator() {
  Release();
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::SafeIterator(
    const SafeIterator& other)
    : dict_(other.dict_),
      table_(other.table_),
      bucket_(other.bucket_),
      current_(other.current_),
      next_(other.next_),
      next_table_(other.next_table_),
      next_bucket_(other.next_bucket_) {
  Acquire();
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::SafeIterator&
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::operator=(
    const SafeIterator& other) {
  if (this != &other) {
    Release();
    dict_ = other.dict_;
    table_ = other.table_;
    bucket_ = other.bucket_;
    current_ = other.current_;
    next_ = other.next_;
    next_table_ = other.next_table_;
    next_bucket_ = other.next_bucket_;
    Acquire();
  }
  return *this;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::SafeIterator(
    SafeIterator&& other) noexcept
    : dict_(other.dict_),
      table_(other.table_),
      bucket_(other.bucket_),
      current_(other.current_),
      next_(other.next_),
      next_table_(other.next_table_),
      next_bucket_(other.next_bucket_) {
  other.dict_ = nullptr;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::SafeIterator&
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::operator=(
    SafeIterator&& other) noexcept {
  if (this != &other) {
    Release();
    dict_ = other.dict_;
    table_ = other.table_;
    bucket_ = other.bucket_;
    current_ = other.current_;
    next_ = other.next_;
    next_table_ = other.next_table_;
    next_bucket_ = other.next_bucket_;
    other.dict_ = nullptr;
  }
  return *this;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::SafeIterator::Acquire() {
  if (dict_) {
    dict_->safe_iterators_++;
  }
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
void Dict<Key, Value, Hash, KeyEqual>::SafeIterator::Release() {
  if (dict_) {
    dict_->safe_iterators_--;
    dict_ = nullptr;
  }
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::SafeIterator&
Dict<Key, Value, Hash, KeyEqual>::SafeIterator::operator++() {
  if (!next_) {
    // End reached
    current_ = nullptr;
    return *this;
  }

  // Advance to pre-computed next (safe even if current_ was deleted)
  table_ = next_table_;
  bucket_ = next_bucket_;
  current_ = next_;

  // Pre-compute the next-next
  ComputeNext();
  return *this;
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::SafeIterator
Dict<Key, Value, Hash, KeyEqual>::SafeBegin() {
  return SafeIterator(const_cast<Dict*>(this));
}

template <typename Key, typename Value, typename Hash, typename KeyEqual>
typename Dict<Key, Value, Hash, KeyEqual>::SafeIterator
Dict<Key, Value, Hash, KeyEqual>::SafeEnd() {
  SafeIterator it;
  it.dict_ = const_cast<Dict*>(this);
  it.Acquire();
  it.current_ = nullptr;
  it.next_ = nullptr;
  return it;
}

}  // namespace ds
}  // namespace miniredis
