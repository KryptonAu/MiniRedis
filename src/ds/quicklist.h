#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>

#include "ds/listpack.h"

namespace miniredis {
namespace ds {

struct QuicklistNode {
  Listpack lp;
  size_t count = 0;
};

class Quicklist {
 public:
  Quicklist();

  size_t Size() const;
  size_t NodeCount() const;
  bool Empty() const;

  void PushHead(std::string_view value);
  void PushHead(int64_t value);
  void PushTail(std::string_view value);
  void PushTail(int64_t value);
  std::optional<std::string> PopHead();
  std::optional<std::string> PopTail();

  std::optional<Listpack::Value> GetValue(size_t index) const;
  std::optional<std::string> Get(size_t index) const;
  bool Set(size_t index, std::string_view value);
  bool Set(size_t index, int64_t value);

  bool InsertBefore(size_t index, std::string_view value);
  bool InsertBefore(size_t index, int64_t value);
  bool InsertAfter(size_t index, std::string_view value);
  bool InsertAfter(size_t index, int64_t value);

  bool Delete(size_t index);
  bool DeleteRange(size_t start, size_t count);

  std::optional<size_t> Find(std::string_view value) const;

  class Iterator;
  Iterator Begin();
  Iterator End();
  Iterator IteratorAt(size_t index);

 private:
  using NodeList = std::list<QuicklistNode>;
  NodeList nodes_;
  size_t count_ = 0;
  int fill_ = -2;

  NodeList::iterator NewNodeBefore(NodeList::iterator pos);
  NodeList::iterator NewNodeAfter(NodeList::iterator pos);
  std::pair<NodeList::iterator, size_t> Seek(size_t index);
  std::pair<NodeList::const_iterator, size_t> Seek(size_t index) const;
  bool NodeAllowInsert(const QuicklistNode& node,
                       size_t encoded_value_size) const;
  bool NodeAllowMerge(const QuicklistNode& a, const QuicklistNode& b) const;
  void MaybeSplit(NodeList::iterator node);
  bool MaybeMerge(NodeList::iterator node);
  size_t FillByteLimit() const;
};

class Quicklist::Iterator {
 public:
  std::optional<Listpack::Value> Value() const;
  std::optional<std::string> StringValue() const;
  std::optional<int64_t> IntValue() const;
  bool Next();
  bool Prev();
  size_t Index() const;

 private:
  friend class Quicklist;
  NodeList::iterator node_iter_;
  Listpack::Iterator lp_iter_;
  size_t global_index_ = 0;
  Quicklist* owner_ = nullptr;

  Iterator(Quicklist* owner, NodeList::iterator node_iter,
           Listpack::Iterator lp_iter, size_t global_index);
};

}  // namespace ds
}  // namespace miniredis
