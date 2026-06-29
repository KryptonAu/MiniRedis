#include "types/list_value.h"

#include <algorithm>

namespace miniredis {

ListValue::ListValue() = default;

void ListValue::PushHead(std::string_view value) { list_.PushHead(value); }

void ListValue::PushTail(std::string_view value) { list_.PushTail(value); }

std::optional<std::string> ListValue::PopHead() { return list_.PopHead(); }

std::optional<std::string> ListValue::PopTail() { return list_.PopTail(); }

std::optional<std::string> ListValue::Get(long long index) const {
  if (Size() == 0) return std::nullopt;
  long long sz = static_cast<long long>(Size());
  if (index < 0) index += sz;
  if (index < 0 || index >= sz) return std::nullopt;
  return list_.Get(static_cast<size_t>(index));
}

bool ListValue::Set(long long index, std::string_view value) {
  if (Size() == 0) return false;
  long long sz = static_cast<long long>(Size());
  if (index < 0) index += sz;
  if (index < 0 || index >= sz) return false;
  return list_.Set(static_cast<size_t>(index), value);
}

std::optional<size_t> ListValue::Find(std::string_view value) const {
  return list_.Find(value);
}

std::vector<std::string> ListValue::Range(long long start, long long stop) {
  long long sz = static_cast<long long>(Size());
  if (sz == 0) return {};

  if (start < 0) start = std::max(start + sz, 0LL);
  if (stop < 0) stop = std::max(stop + sz, 0LL);
  if (start > stop || start >= sz) return {};

  stop = std::min(stop, sz - 1);
  size_t range_len = static_cast<size_t>(stop - start + 1);
  std::vector<std::string> result;
  result.reserve(range_len);

  // Use iterator for sequential O(K) access
  auto it = list_.IteratorAt(static_cast<size_t>(start));
  for (long long i = start; i <= stop; i++) {
    auto val = it.Value();
    if (val) result.push_back(val->ToString());
    it.Next();
  }
  return result;
}

std::vector<std::string> ListValue::Range(long long start,
                                          long long stop) const {
  long long sz = static_cast<long long>(Size());
  if (sz == 0) return {};

  if (start < 0) start = std::max(start + sz, 0LL);
  if (stop < 0) stop = std::max(stop + sz, 0LL);
  if (start > stop || start >= sz) return {};

  stop = std::min(stop, sz - 1);
  std::vector<std::string> result;
  result.reserve(static_cast<size_t>(stop - start + 1));
  for (long long i = start; i <= stop; i++) {
    auto val = list_.Get(static_cast<size_t>(i));
    if (val) result.push_back(*val);
  }
  return result;
}

bool ListValue::Trim(long long start, long long stop) {
  long long sz = static_cast<long long>(Size());
  if (sz == 0) return true;

  if (start < 0) start = std::max(start + sz, 0LL);
  if (stop < 0) stop = std::max(stop + sz, 0LL);
  if (start > stop || start >= sz) {
    // Redis: trim to empty
    while (!list_.Empty()) list_.PopHead();
    return true;
  }
  stop = std::min(stop, sz - 1);

  // Remove from tail first (to avoid index shifts)
  long long to_remove_tail = sz - 1 - stop;
  for (long long i = 0; i < to_remove_tail; i++) {
    list_.PopTail();
  }
  // Remove from head
  for (long long i = 0; i < start; i++) {
    list_.PopHead();
  }
  return true;
}

bool ListValue::InsertBefore(std::string_view pivot, std::string_view value) {
  auto idx = list_.Find(pivot);
  if (!idx) return false;
  return list_.InsertBefore(*idx, value);
}

bool ListValue::InsertAfter(std::string_view pivot, std::string_view value) {
  auto idx = list_.Find(pivot);
  if (!idx) return false;
  return list_.InsertAfter(*idx, value);
}

size_t ListValue::Remove(long long count, std::string_view value) {
  // Single pass: collect all matching indices using iterator
  std::vector<size_t> matches;
  auto it = list_.Begin();
  while (it.Index() < list_.Size()) {
    auto val = it.StringValue();
    if (val && *val == value) {
      matches.push_back(it.Index());
    }
    it.Next();
  }

  if (matches.empty()) return 0;

  size_t to_remove;
  size_t start_offset;
  if (count == 0) {
    to_remove = matches.size();
    start_offset = 0;
  } else if (count > 0) {
    to_remove = std::min(static_cast<size_t>(count), matches.size());
    start_offset = 0;
  } else {
    to_remove = std::min(static_cast<size_t>(-count), matches.size());
    start_offset = matches.size() - to_remove;
  }

  // Delete from end to preserve earlier indices
  for (size_t i = start_offset + to_remove; i > start_offset; i--) {
    list_.Delete(matches[i - 1]);
  }
  return to_remove;
}

size_t ListValue::Size() const { return list_.Size(); }

ValueEncoding ListValue::Encoding() const { return ValueEncoding::kQuicklist; }

bool ListValue::Empty() const { return list_.Empty(); }

}  // namespace miniredis
