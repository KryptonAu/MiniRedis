#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace miniredis {
namespace ds {

template <typename Member, typename Score>
struct SkiplistNode {
  struct Level {
    SkiplistNode* forward = nullptr;
    size_t span = 0;
  };

  struct Deleter {
    void operator()(SkiplistNode* node) const noexcept;
  };

  using OwnedPtr = std::unique_ptr<SkiplistNode, Deleter>;

  Member member;
  Score score;
  SkiplistNode* backward = nullptr;
  OwnedPtr next;

  SkiplistNode(const SkiplistNode&) = delete;
  SkiplistNode& operator=(const SkiplistNode&) = delete;

  static OwnedPtr Create(int level_count, Score score, Member member);

  int LevelCount() const { return level_count_; }
  Level& LevelAt(int level) { return LevelData()[level]; }
  const Level& LevelAt(int level) const { return LevelData()[level]; }
  SkiplistNode* Next() const { return next.get(); }

 private:
  SkiplistNode(int level_count, Score score_value, Member member_value)
      : member(std::move(member_value)),
        score(score_value),
        level_count_(level_count) {}

  static constexpr size_t LevelOffset() noexcept;
  static constexpr size_t AllocationSize(int level_count) noexcept;
  static constexpr std::align_val_t AllocationAlignment() noexcept;
  static void DestroyChain(SkiplistNode* node) noexcept;
  static void DestroyOne(SkiplistNode* node) noexcept;

  Level* LevelData() {
    auto* bytes = reinterpret_cast<std::byte*>(this);
    return std::launder(reinterpret_cast<Level*>(bytes + LevelOffset()));
  }

  const Level* LevelData() const {
    const auto* bytes = reinterpret_cast<const std::byte*>(this);
    return std::launder(reinterpret_cast<const Level*>(bytes + LevelOffset()));
  }

  int level_count_ = 0;
};

template <typename Member, typename Score>
void SkiplistNode<Member, Score>::Deleter::operator()(
    SkiplistNode* node) const noexcept {
  SkiplistNode::DestroyChain(node);
}

template <typename Member, typename Score>
typename SkiplistNode<Member, Score>::OwnedPtr
SkiplistNode<Member, Score>::Create(int level_count, Score score,
                                    Member member) {
  void* storage =
      ::operator new(AllocationSize(level_count), AllocationAlignment());
  SkiplistNode* node = nullptr;
  int constructed_levels = 0;
  try {
    node = new (storage) SkiplistNode(level_count, score, std::move(member));
    for (; constructed_levels < level_count; constructed_levels++) {
      std::construct_at(node->LevelData() + constructed_levels);
    }
    return OwnedPtr(node);
  } catch (...) {
    if (node) {
      for (int i = 0; i < constructed_levels; i++) {
        std::destroy_at(node->LevelData() + i);
      }
      std::destroy_at(node);
    }
    ::operator delete(storage, AllocationAlignment());
    throw;
  }
}

template <typename Member, typename Score>
constexpr size_t SkiplistNode<Member, Score>::LevelOffset() noexcept {
  constexpr size_t kLevelAlignment = alignof(Level);
  size_t offset = sizeof(SkiplistNode);
  size_t remainder = offset % kLevelAlignment;
  if (remainder != 0) offset += kLevelAlignment - remainder;
  return offset;
}

template <typename Member, typename Score>
constexpr size_t SkiplistNode<Member, Score>::AllocationSize(
    int level_count) noexcept {
  return LevelOffset() + static_cast<size_t>(level_count) * sizeof(Level);
}

template <typename Member, typename Score>
constexpr std::align_val_t
SkiplistNode<Member, Score>::AllocationAlignment() noexcept {
  constexpr size_t kAlignment = alignof(SkiplistNode) > alignof(Level)
                                    ? alignof(SkiplistNode)
                                    : alignof(Level);
  return static_cast<std::align_val_t>(kAlignment);
}

template <typename Member, typename Score>
void SkiplistNode<Member, Score>::DestroyChain(SkiplistNode* node) noexcept {
  while (node) {
    SkiplistNode* next = node->next.release();
    DestroyOne(node);
    node = next;
  }
}

template <typename Member, typename Score>
void SkiplistNode<Member, Score>::DestroyOne(SkiplistNode* node) noexcept {
  for (int i = 0; i < node->level_count_; i++) {
    std::destroy_at(node->LevelData() + i);
  }
  std::destroy_at(node);
  ::operator delete(node, AllocationAlignment());
}

template <typename Member, typename Score,
          typename ScoreCompare = std::less<Score>,
          typename MemberCompare = std::less<Member>>
class Skiplist {
 public:
  using Node = SkiplistNode<Member, Score>;

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

  Node* Insert(Score score, Member member);
  Node* InsertView(Score score, std::string_view member);
  bool Delete(Score score, const Member& member);
  bool DeleteView(Score score, std::string_view member);
  bool DeleteNode(Node* node);

  std::optional<size_t> GetRank(Score score, const Member& member) const;
  std::optional<size_t> GetRankView(Score score, std::string_view member) const;
  Node* GetByRank(size_t rank) const;

  bool ScoreInRange(const RangeSpec& range) const;
  Node* FirstInRange(const RangeSpec& range) const;
  Node* LastInRange(const RangeSpec& range) const;

  size_t Size() const { return size_; }
  static int RandomLevel();

  Node* First() const { return header_ ? header_->Next() : nullptr; }
  Node* Tail() const { return tail_; }

  template <typename OnDelete>
  size_t DeleteRangeByScore(const RangeSpec& range, OnDelete on_delete);

  template <typename OnDelete>
  size_t DeleteRangeByRank(size_t start, size_t end, OnDelete on_delete);

 private:
  static constexpr int kMaxLevel = 32;
  static constexpr double kProbability = 0.25;

  typename Node::OwnedPtr header_;
  Node* tail_ = nullptr;
  size_t size_ = 0;
  int max_level_ = 0;
  ScoreCompare score_compare_;
  MemberCompare member_compare_;

  bool CompareLess(Score a, const Member& member_a, Score b,
                   const Member& member_b) const;
  template <typename LookupMember, typename MemberLess>
  bool CompareLessForLookup(Score a, const Member& member_a, Score b,
                            const LookupMember& member_b,
                            MemberLess member_less) const;
  bool ScoreLess(Score a, Score b) const;
  bool ScoreEqual(Score a, Score b) const;
  bool ScoreGreater(Score a, Score b) const;
  bool InRange(Score score, const RangeSpec& range) const;
  bool UnlinkNode(Node* node, Node* update[kMaxLevel]);
  template <typename LookupMember, typename MemberLess,
            typename MakeStoredMember>
  Node* InsertWithLookup(Score score, const LookupMember& member,
                         MemberLess member_less, MakeStoredMember make_member);
  template <typename LookupMember, typename MemberLess, typename MemberEqual>
  bool DeleteWithLookup(Score score, const LookupMember& member,
                        MemberLess member_less, MemberEqual member_equal);
  template <typename LookupMember, typename MemberLess, typename MemberEqual>
  std::optional<size_t> GetRankWithLookup(Score score,
                                          const LookupMember& member,
                                          MemberLess member_less,
                                          MemberEqual member_equal) const;
};

// ===== Implementation =====

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
Skiplist<Member, Score, ScoreCompare, MemberCompare>::Skiplist() {
  header_ = Node::Create(kMaxLevel, Score{}, Member{});
  for (int i = 0; i < kMaxLevel; i++) {
    header_->LevelAt(i).forward = nullptr;
    header_->LevelAt(i).span = 0;
  }
  header_->backward = nullptr;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
Skiplist<Member, Score, ScoreCompare, MemberCompare>::Skiplist(
    Skiplist&& other) noexcept
    : header_(std::move(other.header_)),
      tail_(other.tail_),
      size_(other.size_),
      max_level_(other.max_level_),
      score_compare_(std::move(other.score_compare_)),
      member_compare_(std::move(other.member_compare_)) {
  other.tail_ = nullptr;
  other.size_ = 0;
  other.max_level_ = 0;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
Skiplist<Member, Score, ScoreCompare, MemberCompare>&
Skiplist<Member, Score, ScoreCompare, MemberCompare>::operator=(
    Skiplist&& other) noexcept {
  if (this != &other) {
    header_ = std::move(other.header_);
    tail_ = other.tail_;
    size_ = other.size_;
    max_level_ = other.max_level_;
    score_compare_ = std::move(other.score_compare_);
    member_compare_ = std::move(other.member_compare_);
    other.tail_ = nullptr;
    other.size_ = 0;
    other.max_level_ = 0;
  }
  return *this;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
int Skiplist<Member, Score, ScoreCompare, MemberCompare>::RandomLevel() {
  static thread_local std::mt19937 gen(std::random_device{}());
  static thread_local std::uniform_int_distribution<int> dist(0, 0xFFFF);

  int level = 1;
  while (dist(gen) < static_cast<int>(kProbability * 0xFFFF) &&
         level < kMaxLevel) {
    level++;
  }
  return level;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::CompareLess(
    Score a, const Member& member_a, Score b, const Member& member_b) const {
  if (score_compare_(a, b)) return true;
  if (score_compare_(b, a)) return false;
  return member_compare_(member_a, member_b);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename LookupMember, typename MemberLess>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::CompareLessForLookup(
    Score a, const Member& member_a, Score b, const LookupMember& member_b,
    MemberLess member_less) const {
  if (score_compare_(a, b)) return true;
  if (score_compare_(b, a)) return false;
  return member_less(member_a, member_b);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::ScoreLess(
    Score a, Score b) const {
  return score_compare_(a, b);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::ScoreEqual(
    Score a, Score b) const {
  return !score_compare_(a, b) && !score_compare_(b, a);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::ScoreGreater(
    Score a, Score b) const {
  return score_compare_(b, a);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::InRange(
    Score score, const RangeSpec& range) const {
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

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::Insert(Score score,
                                                             Member member) {
  auto member_less = [this](const Member& stored, const Member& lookup) {
    return member_compare_(stored, lookup);
  };
  return InsertWithLookup(score, member, member_less,
                          [&member]() { return std::move(member); });
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::InsertView(
    Score score, std::string_view member) {
  static_assert(std::is_same_v<Member, std::string>,
                "string_view lookup is only available for string members");
  auto member_less = [](const Member& stored, std::string_view lookup) {
    return std::string_view(stored) < lookup;
  };
  return InsertWithLookup(score, member, member_less,
                          [member]() { return std::string(member); });
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename LookupMember, typename MemberLess, typename MakeStoredMember>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::InsertWithLookup(
    Score score, const LookupMember& member, MemberLess member_less,
    MakeStoredMember make_member) {
  Node* update[kMaxLevel];
  size_t rank[kMaxLevel];

  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    rank[i] = (i == max_level_ - 1) ? 0 : rank[i + 1];
    while (x->LevelAt(i).forward &&
           CompareLessForLookup(x->LevelAt(i).forward->score,
                                x->LevelAt(i).forward->member, score, member,
                                member_less)) {
      rank[i] += x->LevelAt(i).span;
      x = x->LevelAt(i).forward;
    }
    update[i] = x;
  }

  int level = RandomLevel();
  if (level > max_level_) {
    for (int i = max_level_; i < level; i++) {
      rank[i] = 0;
      update[i] = header_.get();
      update[i]->LevelAt(i).span = size_;
    }
    max_level_ = level;
  }

  typename Node::OwnedPtr new_node = Node::Create(level, score, make_member());
  Node* raw = new_node.get();

  for (int i = 0; i < level; i++) {
    raw->LevelAt(i).forward = update[i]->LevelAt(i).forward;
    update[i]->LevelAt(i).forward = raw;
    raw->LevelAt(i).span = update[i]->LevelAt(i).span - (rank[0] - rank[i]);
    update[i]->LevelAt(i).span = (rank[0] - rank[i]) + 1;
  }

  // Increment spans for levels above the new node's level.
  for (int i = level; i < max_level_; i++) {
    update[i]->LevelAt(i).span++;
  }

  raw->backward = (update[0] == header_.get()) ? nullptr : update[0];
  if (raw->LevelAt(0).forward) {
    raw->LevelAt(0).forward->backward = raw;
  } else {
    tail_ = raw;
  }

  raw->next = std::move(update[0]->next);
  update[0]->next = std::move(new_node);
  update[0]->LevelAt(0).forward = raw;

  size_++;
  return raw;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::Delete(
    Score score, const Member& member) {
  auto member_less = [this](const Member& stored, const Member& lookup) {
    return member_compare_(stored, lookup);
  };
  auto member_equal = [](const Member& stored, const Member& lookup) {
    return stored == lookup;
  };
  return DeleteWithLookup(score, member, member_less, member_equal);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::DeleteView(
    Score score, std::string_view member) {
  static_assert(std::is_same_v<Member, std::string>,
                "string_view lookup is only available for string members");
  auto member_less = [](const Member& stored, std::string_view lookup) {
    return std::string_view(stored) < lookup;
  };
  auto member_equal = [](const Member& stored, std::string_view lookup) {
    return std::string_view(stored) == lookup;
  };
  return DeleteWithLookup(score, member, member_less, member_equal);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename LookupMember, typename MemberLess, typename MemberEqual>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::DeleteWithLookup(
    Score score, const LookupMember& member, MemberLess member_less,
    MemberEqual member_equal) {
  if (size_ == 0) return false;

  Node* update[kMaxLevel];
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward &&
           CompareLessForLookup(x->LevelAt(i).forward->score,
                                x->LevelAt(i).forward->member, score, member,
                                member_less)) {
      x = x->LevelAt(i).forward;
    }
    update[i] = x;
  }

  x = update[0]->LevelAt(0).forward;
  if (x && ScoreEqual(x->score, score) && member_equal(x->member, member)) {
    return UnlinkNode(x, update);
  }
  return false;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::DeleteNode(
    Node* node) {
  if (!node || size_ == 0) return false;

  Node* update[kMaxLevel];
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward && CompareLess(x->LevelAt(i).forward->score,
                                                x->LevelAt(i).forward->member,
                                                node->score, node->member)) {
      x = x->LevelAt(i).forward;
    }
    update[i] = x;
  }

  if (update[0]->LevelAt(0).forward != node) return false;
  return UnlinkNode(node, update);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::UnlinkNode(
    Node* node, Node* update[kMaxLevel]) {
  if (!node || !update[0] || update[0]->next.get() != node) return false;

  for (int i = 0; i < max_level_; i++) {
    if (update[i]->LevelAt(i).forward == node) {
      update[i]->LevelAt(i).span += node->LevelAt(i).span - 1;
      update[i]->LevelAt(i).forward = node->LevelAt(i).forward;
    } else {
      update[i]->LevelAt(i).span--;
    }
  }

  if (node->LevelAt(0).forward) {
    node->LevelAt(0).forward->backward = node->backward;
  } else {
    tail_ = node->backward;
  }

  typename Node::OwnedPtr removed = std::move(update[0]->next);
  update[0]->next = std::move(removed->next);
  update[0]->LevelAt(0).forward = update[0]->next.get();

  while (max_level_ > 0 &&
         header_->LevelAt(max_level_ - 1).forward == nullptr) {
    max_level_--;
  }

  size_--;
  return true;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
std::optional<size_t>
Skiplist<Member, Score, ScoreCompare, MemberCompare>::GetRank(
    Score score, const Member& member) const {
  auto member_less = [this](const Member& stored, const Member& lookup) {
    return member_compare_(stored, lookup);
  };
  auto member_equal = [](const Member& stored, const Member& lookup) {
    return stored == lookup;
  };
  return GetRankWithLookup(score, member, member_less, member_equal);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
std::optional<size_t>
Skiplist<Member, Score, ScoreCompare, MemberCompare>::GetRankView(
    Score score, std::string_view member) const {
  static_assert(std::is_same_v<Member, std::string>,
                "string_view lookup is only available for string members");
  auto member_less = [](const Member& stored, std::string_view lookup) {
    return std::string_view(stored) < lookup;
  };
  auto member_equal = [](const Member& stored, std::string_view lookup) {
    return std::string_view(stored) == lookup;
  };
  return GetRankWithLookup(score, member, member_less, member_equal);
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename LookupMember, typename MemberLess, typename MemberEqual>
std::optional<size_t>
Skiplist<Member, Score, ScoreCompare, MemberCompare>::GetRankWithLookup(
    Score score, const LookupMember& member, MemberLess member_less,
    MemberEqual member_equal) const {
  size_t rank = 0;
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward &&
           CompareLessForLookup(x->LevelAt(i).forward->score,
                                x->LevelAt(i).forward->member, score, member,
                                member_less)) {
      rank += x->LevelAt(i).span;
      x = x->LevelAt(i).forward;
    }
  }

  x = x->LevelAt(0).forward;
  if (x && ScoreEqual(x->score, score) && member_equal(x->member, member)) {
    return rank + 1;  // 1-based rank
  }
  return std::nullopt;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::GetByRank(
    size_t rank) const {
  if (rank == 0 || rank > size_) return nullptr;

  size_t traversed = 0;
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward && (traversed + x->LevelAt(i).span) <= rank) {
      traversed += x->LevelAt(i).span;
      x = x->LevelAt(i).forward;
    }
    if (traversed == rank) return x;
  }
  return nullptr;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
bool Skiplist<Member, Score, ScoreCompare, MemberCompare>::ScoreInRange(
    const RangeSpec& range) const {
  if (size_ == 0) return false;
  Node* first = First();
  Node* last = tail_;
  if (!first || !last) return false;

  Score max_first =
      ScoreLess(first->score, last->score) ? last->score : first->score;
  Score min_last =
      ScoreLess(first->score, last->score) ? first->score : last->score;

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

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::FirstInRange(
    const RangeSpec& range) const {
  if (!ScoreInRange(range)) return nullptr;

  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward) {
      Score s = x->LevelAt(i).forward->score;
      if (range.min_exclusive ? ScoreGreater(s, range.min)
                              : !ScoreLess(s, range.min)) {
        break;
      }
      x = x->LevelAt(i).forward;
    }
  }

  x = x->LevelAt(0).forward;
  while (x && !InRange(x->score, range)) {
    x = x->LevelAt(0).forward;
  }
  return x;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
typename Skiplist<Member, Score, ScoreCompare, MemberCompare>::Node*
Skiplist<Member, Score, ScoreCompare, MemberCompare>::LastInRange(
    const RangeSpec& range) const {
  if (!ScoreInRange(range)) return nullptr;

  // Go to the first node past the range, then step back.
  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward) {
      Score s = x->LevelAt(i).forward->score;
      if (range.max_exclusive ? !ScoreLess(s, range.max)
                              : ScoreGreater(s, range.max)) {
        break;
      }
      x = x->LevelAt(i).forward;
    }
  }

  // x is now the last node before the first out-of-range node.
  if (x == header_.get()) return nullptr;

  while (x && !InRange(x->score, range)) {
    x = x->backward;
  }
  return x;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename OnDelete>
size_t Skiplist<Member, Score, ScoreCompare, MemberCompare>::DeleteRangeByScore(
    const RangeSpec& range, OnDelete on_delete) {
  if (!ScoreInRange(range)) return 0;

  Node* update[kMaxLevel];
  Node* x = header_.get();
  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward) {
      Score s = x->LevelAt(i).forward->score;
      if (range.min_exclusive ? ScoreGreater(s, range.min)
                              : !ScoreLess(s, range.min)) {
        break;
      }
      x = x->LevelAt(i).forward;
    }
    update[i] = x;
  }

  size_t deleted = 0;
  x = update[0]->LevelAt(0).forward;
  while (x && InRange(x->score, range)) {
    on_delete(x->member, x->score);
    UnlinkNode(x, update);
    deleted++;
    x = update[0]->LevelAt(0).forward;
  }
  return deleted;
}

template <typename Member, typename Score, typename ScoreCompare,
          typename MemberCompare>
template <typename OnDelete>
size_t Skiplist<Member, Score, ScoreCompare, MemberCompare>::DeleteRangeByRank(
    size_t start, size_t end, OnDelete on_delete) {
  if (start == 0 || start > size_ || end > size_) return 0;
  if (end < start) return 0;

  Node* update[kMaxLevel];
  size_t traversed = 0;
  Node* x = header_.get();

  for (int i = max_level_ - 1; i >= 0; i--) {
    while (x->LevelAt(i).forward && (traversed + x->LevelAt(i).span) < start) {
      traversed += x->LevelAt(i).span;
      x = x->LevelAt(i).forward;
    }
    update[i] = x;
  }

  traversed++;
  size_t deleted = 0;
  x = update[0]->LevelAt(0).forward;
  while (x && traversed <= end) {
    on_delete(x->member, x->score);
    UnlinkNode(x, update);
    deleted++;
    traversed++;
    x = update[0]->LevelAt(0).forward;
  }
  return deleted;
}

}  // namespace ds
}  // namespace miniredis
