#include "ds/quicklist.h"

#include <algorithm>

namespace miniredis {
namespace ds {

// ===== Construction =====
Quicklist::Quicklist() = default;

size_t Quicklist::Size() const { return count_; }

size_t Quicklist::NodeCount() const { return nodes_.size(); }

bool Quicklist::Empty() const { return count_ == 0; }

// ===== fill factor =====
size_t Quicklist::FillByteLimit() const {
  switch (fill_) {
    case -1:
      return 4096;
    case -2:
      return 8192;
    case -3:
      return 16384;
    case -4:
      return 32768;
    case -5:
      return 65536;
    default:
      return static_cast<size_t>(fill_) > 0 ? static_cast<size_t>(fill_) : 8192;
  }
}

// ===== Node management =====
Quicklist::NodeList::iterator Quicklist::NewNodeBefore(NodeList::iterator pos) {
  QuicklistNode node;
  return nodes_.insert(pos, std::move(node));
}

Quicklist::NodeList::iterator Quicklist::NewNodeAfter(NodeList::iterator pos) {
  QuicklistNode node;
  return nodes_.insert(std::next(pos), std::move(node));
}

bool Quicklist::NodeAllowInsert(const QuicklistNode& node,
                                size_t encoded_value_size) const {
  if (fill_ > 0) {
    // Positive fill: entry count limit
    return node.count < static_cast<size_t>(fill_);
  }
  // Negative fill: byte size limit
  return node.lp.TotalBytes() + encoded_value_size <= FillByteLimit();
}

bool Quicklist::NodeAllowMerge(const QuicklistNode& a,
                               const QuicklistNode& b) const {
  size_t combined =
      a.lp.TotalBytes() + b.lp.TotalBytes() - 1;  // subtract 1 EOF overlap
  return combined <= FillByteLimit() / 2;
}

// ===== Seek =====
std::pair<Quicklist::NodeList::iterator, size_t> Quicklist::Seek(size_t index) {
  // Optimize: start from head or tail
  if (index < count_ / 2) {
    auto it = nodes_.begin();
    size_t offset = index;
    while (it != nodes_.end() && offset >= it->count) {
      offset -= it->count;
      ++it;
    }
    return {it, offset};
  } else {
    auto it = nodes_.end();
    if (it != nodes_.begin()) {
      --it;
      size_t offset = count_ - 1 - index;
      while (offset >= it->count) {
        offset -= it->count;
        if (it == nodes_.begin()) break;
        --it;
      }
      return {it, it->count - 1 - offset};
    }
    return {it, 0};
  }
}

std::pair<Quicklist::NodeList::const_iterator, size_t> Quicklist::Seek(
    size_t index) const {
  if (index < count_ / 2) {
    auto it = nodes_.begin();
    size_t offset = index;
    while (it != nodes_.end() && offset >= it->count) {
      offset -= it->count;
      ++it;
    }
    return {it, offset};
  } else {
    auto it = nodes_.end();
    if (it != nodes_.begin()) {
      --it;
      size_t offset = count_ - 1 - index;
      while (offset >= it->count) {
        offset -= it->count;
        if (it == nodes_.begin()) break;
        --it;
      }
      return {it, it->count - 1 - offset};
    }
    return {it, 0};
  }
}

// ===== Push/Pop =====
void Quicklist::PushHead(std::string_view value) {
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (nodes_.empty() || !NodeAllowInsert(nodes_.front(), encoded_size)) {
    NewNodeBefore(nodes_.begin());
  }
  nodes_.front().lp.Prepend(value);
  nodes_.front().count++;
  count_++;
}

void Quicklist::PushHead(int64_t value) {
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (nodes_.empty() || !NodeAllowInsert(nodes_.front(), encoded_size)) {
    NewNodeBefore(nodes_.begin());
  }
  nodes_.front().lp.Prepend(value);
  nodes_.front().count++;
  count_++;
}

void Quicklist::PushTail(std::string_view value) {
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (nodes_.empty()) {
    nodes_.emplace_back();
  } else if (!NodeAllowInsert(nodes_.back(), encoded_size)) {
    NewNodeAfter(std::prev(nodes_.end()));
  }
  nodes_.back().lp.Append(value);
  nodes_.back().count++;
  count_++;
}

void Quicklist::PushTail(int64_t value) {
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (nodes_.empty()) {
    nodes_.emplace_back();
  } else if (!NodeAllowInsert(nodes_.back(), encoded_size)) {
    NewNodeAfter(std::prev(nodes_.end()));
  }
  nodes_.back().lp.Append(value);
  nodes_.back().count++;
  count_++;
}

std::optional<std::string> Quicklist::PopHead() {
  if (nodes_.empty()) return std::nullopt;

  auto val = nodes_.front().lp.Get(0);
  std::string result = val.has_value() ? val->ToString() : "";

  nodes_.front().lp.Delete(0);
  nodes_.front().count--;
  count_--;

  if (nodes_.front().count == 0) {
    nodes_.pop_front();
  }
  return result;
}

std::optional<std::string> Quicklist::PopTail() {
  if (nodes_.empty()) return std::nullopt;

  auto& back_node = nodes_.back();
  size_t last_idx = back_node.lp.Size() - 1;
  auto val = back_node.lp.Get(last_idx);
  std::string result = val.has_value() ? val->ToString() : "";

  back_node.lp.Delete(last_idx);
  back_node.count--;
  count_--;

  if (back_node.count == 0) {
    nodes_.pop_back();
  }
  return result;
}

// ===== Get/Set =====
std::optional<Listpack::Value> Quicklist::GetValue(size_t index) const {
  if (index >= count_) return std::nullopt;
  auto [it, offset] = Seek(index);
  if (it == nodes_.end()) return std::nullopt;
  return it->lp.Get(offset);
}

std::optional<std::string> Quicklist::Get(size_t index) const {
  auto val = GetValue(index);
  if (!val) return std::nullopt;
  return val->ToString();
}

bool Quicklist::Set(size_t index, std::string_view value) {
  if (index >= count_) return false;
  auto [it, offset] = Seek(index);
  return it->lp.Replace(offset, value);
}

bool Quicklist::Set(size_t index, int64_t value) {
  if (index >= count_) return false;
  auto [it, offset] = Seek(index);
  return it->lp.Replace(offset, value);
}

// ===== Insert =====
bool Quicklist::InsertBefore(size_t index, std::string_view value) {
  if (index > count_) return false;
  if (index == 0) {
    PushHead(value);
    return true;
  }
  // Insert before index = insert at index-1 in listpack
  auto [it, offset] = Seek(index - 1);
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (!NodeAllowInsert(*it, encoded_size)) {
    MaybeSplit(it);
    // Re-seek after split
    auto [new_it, new_offset] = Seek(index - 1);
    it = new_it;
    offset = new_offset;
  }
  it->lp.Insert(offset + 1, value);
  it->count++;
  count_++;
  return true;
}

bool Quicklist::InsertBefore(size_t index, int64_t value) {
  if (index > count_) return false;
  if (index == 0) {
    PushHead(value);
    return true;
  }
  auto [it, offset] = Seek(index - 1);
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (!NodeAllowInsert(*it, encoded_size)) {
    MaybeSplit(it);
    auto [new_it, new_offset] = Seek(index - 1);
    it = new_it;
    offset = new_offset;
  }
  it->lp.Insert(offset + 1, value);
  it->count++;
  count_++;
  return true;
}

bool Quicklist::InsertAfter(size_t index, std::string_view value) {
  if (index >= count_) return false;
  auto [it, offset] = Seek(index);
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (!NodeAllowInsert(*it, encoded_size)) {
    MaybeSplit(it);
    auto [new_it, new_offset] = Seek(index);
    it = new_it;
    offset = new_offset;
  }
  it->lp.Insert(offset + 1, value);
  it->count++;
  count_++;
  return true;
}

bool Quicklist::InsertAfter(size_t index, int64_t value) {
  if (index >= count_) return false;
  auto [it, offset] = Seek(index);
  size_t encoded_size = Listpack::EncodedEntrySize(value);
  if (!NodeAllowInsert(*it, encoded_size)) {
    MaybeSplit(it);
    auto [new_it, new_offset] = Seek(index);
    it = new_it;
    offset = new_offset;
  }
  it->lp.Insert(offset + 1, value);
  it->count++;
  count_++;
  return true;
}

// ===== Delete =====
bool Quicklist::Delete(size_t index) {
  if (index >= count_) return false;
  auto [it, offset] = Seek(index);
  it->lp.Delete(offset);
  it->count--;
  count_--;
  if (it->count == 0) {
    nodes_.erase(it);
  } else {
    MaybeMerge(it);
  }
  return true;
}

bool Quicklist::DeleteRange(size_t start, size_t count) {
  if (start >= count_ || count == 0) return false;
  count = std::min(count, count_ - start);

  auto [it, offset] = Seek(start);
  size_t remaining = count;

  while (remaining > 0 && it != nodes_.end()) {
    size_t node_avail = it->count - offset;
    size_t to_delete = std::min(remaining, node_avail);

    if (to_delete >= it->count) {
      // Whole node to be deleted
      it = nodes_.erase(it);  // erase returns next iterator
      count_ -= to_delete;
      remaining -= to_delete;
    } else {
      // Partial node delete: delete 'to_delete' entries at 'offset'
      // Delete from the end of the range to avoid re-Seek each time
      for (size_t i = 0; i < to_delete; i++) {
        it->lp.Delete(offset);  // offset stays same as elements shift left
      }
      it->count -= to_delete;
      count_ -= to_delete;
      remaining -= to_delete;
      ++it;
      offset = 0;
    }
  }
  return true;
}

// ===== Find =====
std::optional<size_t> Quicklist::Find(std::string_view value) const {
  size_t global_idx = 0;
  for (const auto& node : nodes_) {
    auto idx = node.lp.Find(value);
    if (idx.has_value()) {
      return global_idx + *idx;
    }
    global_idx += node.count;
  }
  return std::nullopt;
}

// ===== Split/Merge =====
void Quicklist::MaybeSplit(NodeList::iterator node) {
  // Split a full node by moving half its entries to a new node
  if (node->count < 2) return;

  // Create a new node after the current one
  auto new_node_iter = NewNodeAfter(node);
  size_t move_count = node->count / 2;
  size_t start_idx = node->count - move_count;

  // Move entries from the back of old node to the front of new node
  std::vector<std::string> moved;
  for (size_t i = start_idx; i < node->count; i++) {
    auto val = node->lp.Get(i);
    if (val) moved.push_back(val->ToString());
  }
  // Delete from back to front
  for (size_t i = 0; i < move_count; i++) {
    node->lp.Delete(node->lp.Size() - 1);
  }
  node->count -= move_count;

  for (const auto& s : moved) {
    new_node_iter->lp.Append(std::string_view(s));
  }
  new_node_iter->count = move_count;
}

bool Quicklist::MaybeMerge(NodeList::iterator node) {
  if (node == nodes_.end()) return false;

  // Try merge with next
  if (auto next = std::next(node); next != nodes_.end()) {
    if (NodeAllowMerge(*node, *next)) {
      // Move all entries from next into node
      for (size_t i = 0; i < next->count; i++) {
        auto val = next->lp.Get(i);
        if (val) node->lp.Append(std::string_view(val->ToString()));
      }
      node->count += next->count;
      nodes_.erase(next);
      return false;  // node is still valid
    }
  }
  // Try merge with prev
  if (node != nodes_.begin()) {
    auto prev = std::prev(node);
    if (NodeAllowMerge(*prev, *node)) {
      for (size_t i = 0; i < node->count; i++) {
        auto val = node->lp.Get(i);
        if (val) prev->lp.Append(std::string_view(val->ToString()));
      }
      prev->count += node->count;
      nodes_.erase(node);
      return true;  // node was erased
    }
  }
  return false;
}

// ===== Iterator =====
Quicklist::Iterator Quicklist::IteratorAt(size_t index) {
  if (index >= count_) return End();
  auto [node_iter, offset] = Seek(index);
  auto lp_iter = node_iter->lp.begin();
  for (size_t i = 0; i < offset; i++) {
    ++lp_iter;
  }
  return Iterator(this, node_iter, lp_iter, index);
}

Quicklist::Iterator Quicklist::Begin() {
  if (nodes_.empty())
    return Iterator(this, nodes_.end(), Listpack::Iterator(), 0);
  return Iterator(this, nodes_.begin(), nodes_.begin()->lp.begin(), 0);
}

Quicklist::Iterator Quicklist::End() {
  return Iterator(this, nodes_.end(), Listpack::Iterator(), count_);
}

Quicklist::Iterator::Iterator(Quicklist* owner, NodeList::iterator node_iter,
                              Listpack::Iterator lp_iter, size_t global_index)
    : node_iter_(node_iter),
      lp_iter_(lp_iter),
      global_index_(global_index),
      owner_(owner) {}

std::optional<Listpack::Value> Quicklist::Iterator::Value() const {
  if (node_iter_ == owner_->nodes_.end()) return std::nullopt;
  return *lp_iter_;
}

std::optional<std::string> Quicklist::Iterator::StringValue() const {
  auto val = Value();
  if (!val) return std::nullopt;
  return val->ToString();
}

std::optional<int64_t> Quicklist::Iterator::IntValue() const {
  if (node_iter_ == owner_->nodes_.end()) return std::nullopt;
  auto val = *lp_iter_;
  if (val.type == Listpack::Value::Type::kInteger) return val.integer;
  return std::nullopt;
}

bool Quicklist::Iterator::Next() {
  if (node_iter_ == owner_->nodes_.end()) return false;
  ++lp_iter_;
  global_index_++;
  // Check if we've exhausted the current listpack
  if (!lp_iter_.Valid() || lp_iter_ == node_iter_->lp.end()) {
    ++node_iter_;
    if (node_iter_ != owner_->nodes_.end()) {
      lp_iter_ = node_iter_->lp.begin();
    }
  }
  return node_iter_ != owner_->nodes_.end();
}

bool Quicklist::Iterator::Prev() {
  if (global_index_ == 0) return false;
  if (lp_iter_ == node_iter_->lp.begin()) {
    // Move to previous node's last entry
    --node_iter_;
    lp_iter_ = node_iter_->lp.end();
  }
  --lp_iter_;
  global_index_--;
  return true;
}

size_t Quicklist::Iterator::Index() const { return global_index_; }

}  // namespace ds
}  // namespace miniredis
