#include "ds/intset.h"

#include <algorithm>
#include <cstring>

#include "ds/ds_common.h"

namespace miniredis {
namespace ds {

namespace {

constexpr size_t kHeaderSize = 8;  // encoding(4) + length(4)

uint32_t LoadEncoding(const uint8_t* p) {
  return LoadLE32(p);
}

uint32_t LoadLength(const uint8_t* p) {
  return LoadLE32(p + 4);
}

void StoreEncoding(uint8_t* p, uint32_t enc) {
  StoreLE32(p, enc);
}

void StoreLength(uint8_t* p, uint32_t len) {
  StoreLE32(p + 4, len);
}

int64_t DecodeInt(const uint8_t* p, uint32_t encoding) {
  switch (encoding) {
    case 2: {
      // int16_t little-endian → sign-extend to int64_t
      int16_t v = static_cast<int16_t>(LoadLE16(p));
      return static_cast<int64_t>(v);
    }
    case 4: {
      // int32_t little-endian → sign-extend to int64_t
      int32_t v = static_cast<int32_t>(LoadLE32(p));
      return static_cast<int64_t>(v);
    }
    case 8: {
      return static_cast<int64_t>(LoadLE64(p));
    }
    default:
      return 0;
  }
}

void EncodeInt(uint8_t* p, uint32_t encoding, int64_t value) {
  switch (encoding) {
    case 2:
      StoreLE16(p, static_cast<uint16_t>(value));
      break;
    case 4:
      StoreLE32(p, static_cast<uint32_t>(value));
      break;
    case 8:
      StoreLE64(p, static_cast<uint64_t>(value));
      break;
  }
}

uint32_t DetermineEncoding(int64_t value) {
  if (value >= INT16_MIN && value <= INT16_MAX) return 2;
  if (value >= INT32_MIN && value <= INT32_MAX) return 4;
  return 8;
}

// Binary search in the encoded contents
bool SearchEncoded(const uint8_t* contents, uint32_t encoding, size_t length,
                   int64_t value, size_t* pos) {
  if (length == 0) {
    *pos = 0;
    return false;
  }

  long long low = 0;
  long long high = static_cast<long long>(length) - 1;

  while (low <= high) {
    long long mid = low + (high - low) / 2;
    int64_t mid_val = DecodeInt(contents + static_cast<size_t>(mid) * encoding, encoding);

    if (mid_val < value) {
      low = mid + 1;
    } else if (mid_val > value) {
      high = mid - 1;
    } else {
      *pos = static_cast<size_t>(mid);
      return true;
    }
  }
  *pos = static_cast<size_t>(low);  // insertion position
  return false;
}

}  // namespace

// ===== Intset public methods =====

Intset::Intset() {
  // Initialize empty int16 set
  buf_.resize(kHeaderSize);
  StoreEncoding(buf_.data(), 2);
  StoreLength(buf_.data(), 0);
}

// static
std::optional<Intset> Intset::FromBytes(std::vector<uint8_t> data) {
  std::span<const uint8_t> span(data);
  if (!ValidateBytes(span)) return std::nullopt;
  Intset is;
  is.buf_ = std::move(data);
  return is;
}

size_t Intset::Size() const { return Length(); }

bool Intset::Contains(int64_t value) const {
  size_t pos;
  return SearchEncoded(buf_.data() + kHeaderSize, Encoding(), Length(), value, &pos);
}

std::optional<int64_t> Intset::Get(size_t index) const {
  if (index >= Length()) return std::nullopt;
  return DecodeInt(buf_.data() + kHeaderSize + index * Encoding(), Encoding());
}

uint32_t Intset::Encoding() const { return LoadEncoding(buf_.data()); }

bool Intset::Add(int64_t value) {
  uint32_t req_enc = RequiredEncoding(value);
  if (req_enc > Encoding()) {
    UpgradeAndAdd(value, req_enc);
    return true;
  }

  size_t pos;
  bool found = SearchEncoded(buf_.data() + kHeaderSize, Encoding(), Length(), value, &pos);
  if (found) return false;

  uint32_t enc = Encoding();
  size_t old_len = Length();
  Resize(enc, old_len + 1);

  // Shift elements after pos right by one encoding slot
  uint8_t* contents = buf_.data() + kHeaderSize;
  size_t bytes_to_move = (old_len - pos) * enc;
  if (bytes_to_move > 0) {
    std::memmove(contents + (pos + 1) * enc, contents + pos * enc, bytes_to_move);
  }
  EncodeInt(contents + pos * enc, enc, value);
  StoreLength(buf_.data(), static_cast<uint32_t>(old_len + 1));
  return true;
}

bool Intset::Remove(int64_t value) {
  if (RequiredEncoding(value) > Encoding()) return false;

  size_t pos;
  bool found = SearchEncoded(buf_.data() + kHeaderSize, Encoding(), Length(), value, &pos);
  if (!found) return false;

  uint32_t enc = Encoding();
  size_t old_len = Length();
  uint8_t* contents = buf_.data() + kHeaderSize;

  // Shift elements after pos left by one encoding slot
  size_t bytes_to_move = (old_len - pos - 1) * enc;
  if (bytes_to_move > 0) {
    std::memmove(contents + pos * enc, contents + (pos + 1) * enc, bytes_to_move);
  }
  Resize(enc, old_len - 1);
  return true;
}

std::optional<size_t> Intset::Find(int64_t value) const {
  if (RequiredEncoding(value) > Encoding()) return std::nullopt;

  size_t pos;
  bool found = SearchEncoded(buf_.data() + kHeaderSize, Encoding(), Length(), value, &pos);
  if (found) return pos;
  return std::nullopt;
}

std::vector<int64_t> Intset::Values() const {
  std::vector<int64_t> result;
  result.reserve(Length());
  for (size_t i = 0; i < Length(); i++) {
    result.push_back(Get(i).value_or(0));
  }
  return result;
}

const uint8_t* Intset::Data() const { return buf_.data(); }

size_t Intset::DataSize() const { return buf_.size(); }

// ===== Private methods =====

uint32_t Intset::Length() const { return LoadLength(buf_.data()); }

int64_t Intset::GetEncoded(size_t index, uint32_t encoding) const {
  return DecodeInt(buf_.data() + kHeaderSize + index * encoding, encoding);
}

void Intset::SetEncoded(size_t index, uint32_t encoding, int64_t value) {
  EncodeInt(buf_.data() + kHeaderSize + index * encoding, encoding, value);
}

uint32_t Intset::RequiredEncoding(int64_t value) const {
  return DetermineEncoding(value);
}

bool Intset::Search(int64_t value, size_t* pos) const {
  return SearchEncoded(buf_.data() + kHeaderSize, Encoding(), Length(), value, pos);
}

void Intset::UpgradeAndAdd(int64_t value, uint32_t new_encoding) {
  uint32_t old_encoding = Encoding();
  size_t len = Length();

  // Create new buffer with new encoding
  std::vector<uint8_t> new_buf;
  new_buf.resize(kHeaderSize + (len + 1) * new_encoding);
  StoreEncoding(new_buf.data(), new_encoding);
  StoreLength(new_buf.data(), static_cast<uint32_t>(len + 1));

  uint8_t* new_contents = new_buf.data() + kHeaderSize;
  const uint8_t* old_contents = buf_.data() + kHeaderSize;

  // Copy existing elements with encoding conversion
  if (value < 0) {
    // Negative values go at the beginning (they're smaller than any existing positive)
    EncodeInt(new_contents, new_encoding, value);
    for (size_t i = 0; i < len; i++) {
      EncodeInt(new_contents + (i + 1) * new_encoding, new_encoding,
                DecodeInt(old_contents + i * old_encoding, old_encoding));
    }
  } else {
    // Positive/zero: find insertion position
    bool inserted = false;
    size_t new_idx = 0;
    for (size_t i = 0; i < len; i++) {
      int64_t elem = DecodeInt(old_contents + i * old_encoding, old_encoding);
      if (!inserted && value <= elem) {
        EncodeInt(new_contents + new_idx * new_encoding, new_encoding, value);
        new_idx++;
        inserted = true;
      }
      EncodeInt(new_contents + new_idx * new_encoding, new_encoding, elem);
      new_idx++;
    }
    if (!inserted) {
      // Value goes at the end
      EncodeInt(new_contents + len * new_encoding, new_encoding, value);
    }
  }

  buf_ = std::move(new_buf);
}

void Intset::Resize(uint32_t encoding, size_t length) {
  size_t new_size = kHeaderSize + length * encoding;
  buf_.resize(new_size);
  StoreEncoding(buf_.data(), encoding);
  StoreLength(buf_.data(), static_cast<uint32_t>(length));
}

// static
bool Intset::ValidateBytes(std::span<const uint8_t> data) {
  if (data.size() < kHeaderSize) return false;

  uint32_t encoding = LoadLE32(data.data());
  uint32_t length = LoadLE32(data.data() + 4);

  if (encoding != 2 && encoding != 4 && encoding != 8) return false;

  size_t expected_size = kHeaderSize + static_cast<size_t>(length) * encoding;
  if (data.size() != expected_size) return false;

  // Validate sorted order
  if (length > 1) {
    const uint8_t* contents = data.data() + kHeaderSize;
    for (size_t i = 1; i < length; i++) {
      int64_t prev = DecodeInt(contents + (i - 1) * encoding, encoding);
      int64_t cur = DecodeInt(contents + i * encoding, encoding);
      if (prev >= cur) return false;  // must be strictly increasing
    }
  }

  return true;
}

}  // namespace ds
}  // namespace miniredis
