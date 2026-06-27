#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace miniredis {
namespace ds {

template <typename Key, typename Score>
struct SkiplistNode {
  Key key;
  Score score;
  SkiplistNode* backward = nullptr;
  struct Level {
    SkiplistNode* forward = nullptr;
    size_t span = 0;
  };
  std::vector<Level> levels;
};

template <typename Key, typename Score, typename ScoreCompare = std::less<Score>,
          typename KeyCompare = std::less<Key>>
class Skiplist {
public:
  using Node = SkiplistNode<Key, Score>;

  struct RangeSpec {
    Score min;
    Score max;
    bool min_exclusive = false;
    bool max_exclusive = false;
  };

  Skiplist();
  ~Skiplist() = default;

  // Move-only
  Skiplist(const Skiplist&) = delete;
  Skiplist& operator=(const Skiplist&) = delete;
  Skiplist(Skiplist&& other) noexcept;
  Skiplist& operator=(Skiplist&& other) noexcept;

  Node* Insert(Score score, Key key);
  bool Delete(Score score, const Key& key);
  bool DeleteNode(Node* node);

  std::optional<size_t> GetRank(Score score, const Key& key) const;
  Node* GetByRank(size_t rank) const;

  bool ScoreInRange(const RangeSpec& range) const;
  Node* FirstInRange(const RangeSpec& range) const;
  Node* LastInRange(const RangeSpec& range) const;

  size_t Size() const { return size_; }
  static int RandomLevel();

  Node* First() const { return header_->levels[0].forward; }
  Node* Tail() const { return tail_; }

  template <typename OnDelete>
  size_t DeleteRangeByScore(const RangeSpec& range, OnDelete on_delete);

  template <typename OnDelete>
  size_t DeleteRangeByRank(size_t start, size_t end, OnDelete on_delete);

private:
  static constexpr int kMaxLevel = 32;
  static constexpr double kProbability = 0.25;

  std::unique_ptr<Node> header_;
  std::vector<std::unique_ptr<Node>> nodes_;
  Node* tail_ = nullptr;
  size_t size_ = 0;
  int max_level_ = 0;
  ScoreCompare score_compare_;
  KeyCompare key_compare_;

  bool CompareLess(Score a, const Key& ka, Score b, const Key& kb) const;
  bool ScoreLess(Score a, Score b) const;
  bool ScoreEqual(Score a, Score b) const;
  bool ScoreGreater(Score a, Score b) const;
  bool InRange(Score score, const RangeSpec& range) const;
};

// ===== Implementation =====

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
Skiplist<Key, Score, ScoreCompare, KeyCompare>::Skiplist() {
  header_ = std::make_unique<Node>();
  header_->levels.resize(kMaxLevel);
  for (int i = 0; i < kMaxLevel; i++) {
    header_->levels[i].forward = nullptr;
    header_->levels[i].span = 0;
  }
  header_->backward = nullptr;
}

template <typename Key, typename Score, typename ScoreCompare,
          typename KeyCompare>
Skiplist<Key, Score, ScoreCompare, KeyCompare>::Skiplist(
    Skiplist&& other) noexcept
    : header_(std::move(other.header_)),
      nodes_(std::move(other.nodes_)),
      tail_(other.tail_),
      size_(other.size_),
      max_level_(other.max_level_),
      score_compare_(std::move(other.score_compare_)),
      key_compare_(std::move(other.key_compare_)) {
  other.tail_ = nullptr;
  other.size_ = 0;
  other.max_level_ = 0;
}

template <typename Key, typename Score, typename ScoreCompare,
          typename KeyCompare>
Skiplist<Key, Score, ScoreCompare, KeyCompare>&
Skiplist<Key, Score, ScoreCompare, KeyCompare>::operator=(
    Skiplist&& other) noexcept {
  if (this != &other) {
    header_ = std::move(other.header_);
    nodes_ = std::move(other.nodes_);
    tail_ = other.tail_;
    size_ = other.size_;
    max_level_ = other.max_level_;
    score_compare_ = std::move(other.score_compare_);
    key_compare_ = std::move(other.key_compare_);
    other.tail_ = nullptr;
    other.size_ = 0;
    other.max_level_ = 0;
  }
  return *this;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
int Skiplist<Key, Score, ScoreCompare, KeyCompare>::RandomLevel() {
  static thread_local std::mt19937 gen(std::random_device{}());
  static thread_local std::uniform_int_distribution<int> dist(0, 0xFFFF);

  int level = 1;
  while (dist(gen) < static_cast<int>(kProbability * 0xFFFF) && level < kMaxLevel) {
    level++;
  }
  return level;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::CompareLess(Score a, const Key& ka, Score b,
                                                                  const Key& kb) const {
  if (score_compare_(a, b)) return true;
  if (score_compare_(b, a)) return false;
  return key_compare_(ka, kb);
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::ScoreLess(Score a, Score b) const {
  return score_compare_(a, b);
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::ScoreEqual(Score a, Score b) const {
  return !score_compare_(a, b) && !score_compare_(b, a);
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::ScoreGreater(Score a, Score b) const {
  return score_compare_(b, a);
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::InRange(Score score,
                                                              const RangeSpec& range) const {
  if (range.min_exclusive) {
    if (!ScoreGreater(score, range.min)) return false;
  } else {
    if (ScoreLess(score, range.min)) return false;
  }
  if (range.max_exclusive) {
    if (!ScoreLess(score, range.max)) return false;
  } else {
    if (ScoreGreater(score, range.max)) return false;
  }
  return true;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
typename Skiplist<Key, Score, ScoreCompare, KeyCompare>::Node*
Skiplist<Key, Score, ScoreCompare, KeyCompare>::Insert(Score score, Key key) {
  Node* update[kMaxLevel];
  size_t rank[kMaxLevel];

  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    rank[i] = (i == max_level_ - 1) ? 0 : rank[i + 1];
    while (x->levels[i].forward &&
           CompareLess(x->levels[i].forward->score, x->levels[i].forward->key, score, key)) {
      rank[i] += x->levels[i].span;
      x = x->levels[i].forward;
    }
    update[i] = x;
  }

  int level = RandomLevel();
  if (level > max_level_) {
    for (int i = max_level_; i < level; i++) {
      rank[i] = 0;
      update[i] = header_.get();
      update[i]->levels[i].span = size_;
    }
    max_level_ = level;
  }

  auto new_node = std::make_unique<Node>();
  new_node->key = std::move(key);
  new_node->score = score;
  new_node->levels.resize(level);
  Node* raw = new_node.get();

  for (int i = 0; i < level; i++) {
    raw->levels[i].forward = update[i]->levels[i].forward;
    update[i]->levels[i].forward = raw;
    raw->levels[i].span = update[i]->levels[i].span - (rank[0] - rank[i]);
    update[i]->levels[i].span = (rank[0] - rank[i]) + 1;
  }

  // Increment spans for levels above the new node's level
  for (int i = level; i < max_level_; i++) {
    update[i]->levels[i].span++;
  }

  // Set backward pointer
  raw->backward = (update[0] == header_.get()) ? nullptr : update[0];
  if (raw->levels[0].forward) {
    raw->levels[0].forward->backward = raw;
  } else {
    tail_ = raw;
  }

  nodes_.push_back(std::move(new_node));
  size_++;
  return raw;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::Delete(Score score, const Key& key) {
  Node* update[kMaxLevel];
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward &&
           CompareLess(x->levels[i].forward->score, x->levels[i].forward->key, score, key)) {
      x = x->levels[i].forward;
    }
    update[i] = x;
  }

  x = x->levels[0].forward;
  if (x && ScoreEqual(x->score, score) && x->key == key) {
    DeleteNode(x);
    return true;
  }
  return false;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::DeleteNode(Node* node) {
  if (!node) return false;

  Node* update[kMaxLevel];
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward &&
           CompareLess(x->levels[i].forward->score, x->levels[i].forward->key, node->score,
                       node->key)) {
      x = x->levels[i].forward;
    }
    update[i] = x;
  }

  int node_level = static_cast<int>(node->levels.size());
  for (int i = 0; i < node_level; i++) {
    if (update[i]->levels[i].forward == node) {
      update[i]->levels[i].span += node->levels[i].span - 1;
      update[i]->levels[i].forward = node->levels[i].forward;
    } else {
      update[i]->levels[i].span--;
    }
  }

  // Decrement spans for levels above the node's level
  for (int i = node_level; i < max_level_; i++) {
    update[i]->levels[i].span--;
  }

  // Update backward pointer
  if (node->levels[0].forward) {
    node->levels[0].forward->backward = node->backward;
  } else {
    tail_ = node->backward;
  }

  // Update max_level_ if needed
  while (max_level_ > 0 && header_->levels[max_level_ - 1].forward == nullptr) {
    max_level_--;
  }

  // Remove node from nodes_ (erase by raw pointer)
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [node](const std::unique_ptr<Node>& p) { return p.get() == node; });
  if (it != nodes_.end()) {
    nodes_.erase(it);
  }

  size_--;
  return true;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
std::optional<size_t> Skiplist<Key, Score, ScoreCompare, KeyCompare>::GetRank(
    Score score, const Key& key) const {
  size_t rank = 0;
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward &&
           CompareLess(x->levels[i].forward->score, x->levels[i].forward->key, score, key)) {
      rank += x->levels[i].span;
      x = x->levels[i].forward;
    }
  }

  x = x->levels[0].forward;
  if (x && ScoreEqual(x->score, score) && x->key == key) {
    return rank + 1;  // 1-based rank
  }
  return std::nullopt;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
typename Skiplist<Key, Score, ScoreCompare, KeyCompare>::Node*
Skiplist<Key, Score, ScoreCompare, KeyCompare>::GetByRank(size_t rank) const {
  if (rank == 0 || rank > size_) return nullptr;

  size_t traversed = 0;
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward && (traversed + x->levels[i].span) <= rank) {
      traversed += x->levels[i].span;
      x = x->levels[i].forward;
    }
    if (traversed == rank) return x;
  }
  return nullptr;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
bool Skiplist<Key, Score, ScoreCompare, KeyCompare>::ScoreInRange(const RangeSpec& range) const {
  if (size_ == 0) return false;
  Node* first = First();
  Node* last = tail_;
  if (!first || !last) return false;

  Score max_first = ScoreLess(first->score, last->score) ? last->score : first->score;
  Score min_last = ScoreLess(first->score, last->score) ? first->score : last->score;

  Score range_max = range.max;
  Score range_min = range.min;

  if (range.min_exclusive) {
    if (!ScoreGreater(max_first, range_min)) return false;
  } else {
    if (ScoreLess(max_first, range_min)) return false;
  }
  if (range.max_exclusive) {
    if (!ScoreLess(min_last, range_max)) return false;
  } else {
    if (ScoreGreater(min_last, range_max)) return false;
  }
  return true;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
typename Skiplist<Key, Score, ScoreCompare, KeyCompare>::Node*
Skiplist<Key, Score, ScoreCompare, KeyCompare>::FirstInRange(const RangeSpec& range) const {
  if (!ScoreInRange(range)) return nullptr;

  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward) {
      Score s = x->levels[i].forward->score;
      if (range.min_exclusive ? ScoreGreater(s, range.min) : !ScoreLess(s, range.min)) {
        break;
      }
      x = x->levels[i].forward;
    }
  }

  x = x->levels[0].forward;
  while (x && !InRange(x->score, range)) {
    x = x->levels[0].forward;
  }
  return x;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
typename Skiplist<Key, Score, ScoreCompare, KeyCompare>::Node*
Skiplist<Key, Score, ScoreCompare, KeyCompare>::LastInRange(const RangeSpec& range) const {
  if (!ScoreInRange(range)) return nullptr;

  // Go to the first node past the range, then step back
  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->levels[i].forward) {
      Score s = x->levels[i].forward->score;
      if (range.max_exclusive ? !ScoreLess(s, range.max) : ScoreGreater(s, range.max)) {
        break;
      }
      x = x->levels[i].forward;
    }
  }

  // x is now the last node before the first out-of-range node
  if (x == header_.get()) return nullptr;

  while (x && !InRange(x->score, range)) {
    x = x->backward;
  }
  return x;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
template <typename OnDelete>
size_t Skiplist<Key, Score, ScoreCompare, KeyCompare>::DeleteRangeByScore(
    const RangeSpec& range, OnDelete on_delete) {
  if (!ScoreInRange(range)) return 0;

  Node* first = FirstInRange(range);
  if (!first) return 0;

  size_t deleted = 0;
  Node* node = first;
  while (node && InRange(node->score, range)) {
    Node* next = node->levels[0].forward;
    on_delete(node->key, node->score);
    DeleteNode(node);
    deleted++;
    node = next;
  }
  return deleted;
}

template <typename Key, typename Score, typename ScoreCompare, typename KeyCompare>
template <typename OnDelete>
size_t Skiplist<Key, Score, ScoreCompare, KeyCompare>::DeleteRangeByRank(
    size_t start, size_t end, OnDelete on_delete) {
  if (start == 0 || start > size_ || end > size_) return 0;
  if (end < start) return 0;

  size_t count = end - start + 1;
  size_t deleted = 0;

  for (size_t i = 0; i < count; i++) {
    Node* node = GetByRank(start);
    if (!node) break;
    on_delete(node->key, node->score);
    DeleteNode(node);
    deleted++;
  }
  return deleted;
}

}  // namespace ds
}  // namespace miniredis
