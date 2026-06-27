#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {
namespace ds {

class Listpack {
public:
  struct Value {
    enum class Type { kString, kInteger };
    Type type;
    std::string_view string;
    int64_t integer;

    std::string ToString() const;
  };

  Listpack();
  static std::optional<Listpack> FromBytes(std::vector<uint8_t> data);

  size_t Size() const;
  size_t TotalBytes() const;
  const uint8_t* Data() const;
  size_t DataSize() const;
  static size_t EncodedEntrySize(std::string_view value);
  static size_t EncodedEntrySize(int64_t value);

  std::optional<Value> Get(size_t index) const;
  std::optional<std::string_view> GetString(size_t index) const;
  std::optional<int64_t> GetInteger(size_t index) const;

  bool IsString(size_t index) const;
  bool IsInteger(size_t index) const;

  bool Insert(size_t index, std::string_view value);
  bool Insert(size_t index, int64_t value);
  bool Replace(size_t index, std::string_view value);
  bool Replace(size_t index, int64_t value);
  bool Delete(size_t index);
  bool Append(std::string_view value);
  bool Append(int64_t value);
  bool Prepend(std::string_view value);
  bool Prepend(int64_t value);

  std::optional<size_t> Find(std::string_view value) const;
  std::optional<size_t> Find(int64_t value) const;

private:
  std::vector<uint8_t> buf_;

  size_t EntryCount() const;
  std::optional<size_t> Seek(size_t index) const;
  size_t SeekInsertPosition(size_t index) const;
  size_t DecodeBacklenEndingAt(size_t pos) const;
  size_t EncodedPayloadSizeAt(size_t pos) const;
  size_t EntrySizeAt(size_t pos) const;
  void UpdateHeader();
  static bool ValidateBytes(std::span<const uint8_t> data);
};

}  // namespace ds
}  // namespace miniredis
