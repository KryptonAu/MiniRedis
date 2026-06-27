#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace miniredis {
namespace ds {

class Intset {
public:
  Intset();
  static std::optional<Intset> FromBytes(std::vector<uint8_t> data);

  size_t Size() const;
  bool Contains(int64_t value) const;
  std::optional<int64_t> Get(size_t index) const;
  uint32_t Encoding() const;

  bool Add(int64_t value);
  bool Remove(int64_t value);

  std::optional<size_t> Find(int64_t value) const;

  std::vector<int64_t> Values() const;

  const uint8_t* Data() const;
  size_t DataSize() const;

private:
  std::vector<uint8_t> buf_;

  uint32_t Length() const;
  int64_t GetEncoded(size_t index, uint32_t encoding) const;
  void SetEncoded(size_t index, uint32_t encoding, int64_t value);
  uint32_t RequiredEncoding(int64_t value) const;
  bool Search(int64_t value, size_t* pos) const;
  void UpgradeAndAdd(int64_t value, uint32_t new_encoding);
  void Resize(uint32_t encoding, size_t length);
  static bool ValidateBytes(std::span<const uint8_t> data);
};

}  // namespace ds
}  // namespace miniredis
