#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/quicklist.h"
#include "types/value_fwd.h"

namespace miniredis {

class ListValue {
 public:
  ListValue();

  ListValue(const ListValue&) = delete;
  ListValue& operator=(const ListValue&) = delete;
  ListValue(ListValue&&) noexcept = default;
  ListValue& operator=(ListValue&&) noexcept = default;

  void PushHead(std::string_view value);
  void PushTail(std::string_view value);
  std::optional<std::string> PopHead();
  std::optional<std::string> PopTail();

  std::optional<std::string> Get(long long index) const;
  bool Set(long long index, std::string_view value);
  std::optional<size_t> Find(std::string_view value) const;

  std::vector<std::string> Range(long long start, long long stop);
  std::vector<std::string> Range(long long start, long long stop) const;
  bool Trim(long long start, long long stop);

  bool InsertBefore(std::string_view pivot, std::string_view value);
  bool InsertAfter(std::string_view pivot, std::string_view value);
  size_t Remove(long long count, std::string_view value);

  size_t Size() const;
  ValueEncoding Encoding() const;
  bool Empty() const;

 private:
  ds::Quicklist list_;
};

}  // namespace miniredis
