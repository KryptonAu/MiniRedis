#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ds/dict.h"
#include "ds/listpack.h"
#include "types/encoding_thresholds.h"
#include "types/operation_result.h"
#include "types/value_fwd.h"

namespace miniredis {

class HashValue {
 public:
  using Hashtable = ds::Dict<std::string, std::string>;
  using Storage = std::variant<ds::Listpack, Hashtable>;

  explicit HashValue(EncodingThresholds thresholds = {});

  HashValue(const HashValue&) = delete;
  HashValue& operator=(const HashValue&) = delete;
  HashValue(HashValue&&) noexcept = default;
  HashValue& operator=(HashValue&&) noexcept = default;

  bool Set(std::string_view field, std::string_view value);
  bool SetNX(std::string_view field, std::string_view value);
  std::optional<std::string> Get(std::string_view field) const;
  bool Delete(std::string_view field);
  bool Exists(std::string_view field) const;

  std::vector<std::optional<std::string>> MGet(
      const std::vector<std::string>& fields) const;

  size_t Size() const;
  size_t FieldLength(std::string_view field) const;
  ValueEncoding Encoding() const;

  std::vector<std::string> Keys() const;
  std::vector<std::string> Values() const;
  std::vector<std::pair<std::string, std::string>> GetAll() const;

  TypeResult<int64_t> IncrementBy(std::string_view field, int64_t delta);
  TypeResult<double> IncrementByFloat(std::string_view field, double delta);

  std::optional<std::string> RandomField() const;
  std::vector<std::string> Scan(size_t cursor, size_t count) const;

 private:
  Storage encoding_;
  EncodingThresholds thresholds_;

  void MaybeUpgrade(size_t new_field_size, size_t new_value_size);
  void ConvertToHashtable();
};

}  // namespace miniredis
