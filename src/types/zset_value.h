#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ds/dict.h"
#include "ds/listpack.h"
#include "ds/skiplist.h"
#include "types/encoding_thresholds.h"
#include "types/operation_result.h"
#include "types/value_fwd.h"

namespace miniredis {

struct ZSetSkiplist {
  using Node = ds::SkiplistNode<std::string, double>;

  ZSetSkiplist() = default;
  ZSetSkiplist(const ZSetSkiplist&) = delete;
  ZSetSkiplist& operator=(const ZSetSkiplist&) = delete;
  ZSetSkiplist(ZSetSkiplist&& other) noexcept;
  ZSetSkiplist& operator=(ZSetSkiplist&& other) noexcept;

  ds::Skiplist<std::string, double> skiplist;
  ds::Dict<std::string, Node*> dict;

  void RebuildDict();
};

class ZSetValue {
 public:
  using Storage = std::variant<ds::Listpack, ZSetSkiplist>;

  explicit ZSetValue(EncodingThresholds thresholds = {});

  ZSetValue(const ZSetValue&) = delete;
  ZSetValue& operator=(const ZSetValue&) = delete;
  ZSetValue(ZSetValue&&) noexcept = default;
  ZSetValue& operator=(ZSetValue&&) noexcept = default;

  TypeResult<bool> Add(std::string_view element, double score);
  bool Remove(std::string_view element);
  TypeResult<double> Update(std::string_view element, double delta);

  std::optional<double> Score(std::string_view element) const;
  std::optional<size_t> Rank(std::string_view element) const;
  std::optional<size_t> RevRank(std::string_view element) const;
  size_t Count() const;
  size_t CountByScore(double min, double max, bool min_ex, bool max_ex) const;
  size_t LexCount(std::string_view min, std::string_view max, bool min_ex,
                  bool max_ex) const;

  struct RangeResult {
    std::string element;
    double score;
  };
  std::vector<RangeResult> Range(long long start, long long stop) const;
  std::vector<RangeResult> RevRange(long long start, long long stop) const;
  std::vector<RangeResult> RangeByScore(double min, double max, bool min_ex,
                                        bool max_ex, long long offset = 0,
                                        long long count = -1) const;
  std::vector<RangeResult> RangeByLex(std::string_view min,
                                      std::string_view max, bool min_ex,
                                      bool max_ex, long long offset = 0,
                                      long long count = -1) const;

  std::optional<RangeResult> PopMin();
  std::optional<RangeResult> PopMax();

  size_t RemoveRangeByRank(long long start, long long stop);
  size_t RemoveRangeByScore(double min, double max, bool min_ex, bool max_ex);
  size_t RemoveRangeByLex(std::string_view min, std::string_view max,
                          bool min_ex, bool max_ex);

  void Union(const ZSetValue& other, std::vector<double> weights,
             const std::string& aggregate);
  void Intersect(const ZSetValue& other, std::vector<double> weights,
                 const std::string& aggregate);

  ValueEncoding Encoding() const;

  std::vector<RangeResult> Scan(size_t cursor, size_t count) const;

 private:
  Storage encoding_;
  EncodingThresholds thresholds_;

  void MaybeUpgrade(size_t new_element_size = 0);
  void ConvertToSkiplist();
};

}  // namespace miniredis
