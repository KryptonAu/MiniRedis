#include "ds/listpack.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "ds/ds_common.h"

namespace miniredis {
namespace ds {

// ===== Encoding constants (matching Redis listpack.c) =====
namespace {

constexpr size_t kHdrSize = 6;
constexpr uint8_t kEof = 0xFF;

// Encoding type markers
constexpr uint8_t kEnc7BitUintMask = 0x80;
constexpr uint8_t kEnc6BitStrMask = 0xC0;
constexpr uint8_t kEnc13BitIntMask = 0xE0;
constexpr uint8_t kEnc12BitStrMask = 0xF0;

constexpr uint8_t kEnc6BitStr = 0x80;
constexpr uint8_t kEnc13BitInt = 0xC0;
constexpr uint8_t kEnc12BitStr = 0xE0;
constexpr uint8_t kEnc32BitStr = 0xF0;
constexpr uint8_t kEnc16BitInt = 0xF1;
constexpr uint8_t kEnc24BitInt = 0xF2;
constexpr uint8_t kEnc32BitInt = 0xF3;
constexpr uint8_t kEnc64BitInt = 0xF4;

// Backlen constants
constexpr size_t kMaxBacklenSize = 5;

// Determine if value can be encoded as an integer
bool CanEncodeAsInt(std::string_view value, int64_t* out) {
  if (value.empty()) return false;
  const char* p = value.data();
  size_t len = value.size();

  if (len == 1 && p[0] == '0') {
    *out = 0;
    return true;
  }

  int negative = 0;
  size_t pos = 0;
  if (p[0] == '-') {
    negative = 1;
    pos++;
    if (pos == len) return false;
  }

  if (p[pos] < '1' || p[pos] > '9') return false;

  uint64_t v = static_cast<uint64_t>(p[pos] - '0');
  pos++;

  while (pos < len && p[pos] >= '0' && p[pos] <= '9') {
    if (v > (UINT64_MAX / 10)) return false;
    v *= 10;
    uint64_t digit = static_cast<uint64_t>(p[pos] - '0');
    if (v > (UINT64_MAX - digit)) return false;
    v += digit;
    pos++;
  }

  if (pos != len) return false;

  if (negative) {
    if (v > (static_cast<uint64_t>(INT64_MAX) + 1)) return false;
    *out = -static_cast<int64_t>(v);
  } else {
    if (v > static_cast<uint64_t>(INT64_MAX)) return false;
    *out = static_cast<int64_t>(v);
  }
  return true;
}

// Encode an integer value, returning number of bytes used
size_t EncodeInteger(uint8_t* buf, int64_t v) {
  if (v >= 0 && v <= 127) {
    buf[0] = static_cast<uint8_t>(v);
    return 1;
  } else if (v >= -4096 && v <= 4095) {
    if (v < 0) v = (static_cast<int64_t>(1) << 13) + v;
    buf[0] = static_cast<uint8_t>((v >> 8) | kEnc13BitInt);
    buf[1] = static_cast<uint8_t>(v & 0xFF);
    return 2;
  } else if (v >= -32768 && v <= 32767) {
    if (v < 0) v = (static_cast<int64_t>(1) << 16) + v;
    buf[0] = kEnc16BitInt;
    buf[1] = static_cast<uint8_t>(v & 0xFF);
    buf[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    return 3;
  } else if (v >= -8388608 && v <= 8388607) {
    if (v < 0) v = (static_cast<int64_t>(1) << 24) + v;
    buf[0] = kEnc24BitInt;
    buf[1] = static_cast<uint8_t>(v & 0xFF);
    buf[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>((v >> 16) & 0xFF);
    return 4;
  } else if (v >= -2147483648LL && v <= 2147483647LL) {
    if (v < 0) v = (static_cast<int64_t>(1) << 32) + v;
    buf[0] = kEnc32BitInt;
    buf[1] = static_cast<uint8_t>(v & 0xFF);
    buf[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[4] = static_cast<uint8_t>((v >> 24) & 0xFF);
    return 5;
  } else {
    uint64_t uv = static_cast<uint64_t>(v);
    buf[0] = kEnc64BitInt;
    buf[1] = static_cast<uint8_t>(uv & 0xFF);
    buf[2] = static_cast<uint8_t>((uv >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>((uv >> 16) & 0xFF);
    buf[4] = static_cast<uint8_t>((uv >> 24) & 0xFF);
    buf[5] = static_cast<uint8_t>((uv >> 32) & 0xFF);
    buf[6] = static_cast<uint8_t>((uv >> 40) & 0xFF);
    buf[7] = static_cast<uint8_t>((uv >> 48) & 0xFF);
    buf[8] = static_cast<uint8_t>((uv >> 56) & 0xFF);
    return 9;
  }
}

// Encode a string value, returns payload size
size_t EncodeString(uint8_t* buf, std::string_view value) {
  size_t len = value.size();
  if (len < 64) {
    buf[0] = static_cast<uint8_t>(len) | kEnc6BitStr;
    std::memcpy(buf + 1, value.data(), len);
    return 1 + len;
  } else if (len < 4096) {
    buf[0] = static_cast<uint8_t>((len >> 8) | kEnc12BitStr);
    buf[1] = static_cast<uint8_t>(len & 0xFF);
    std::memcpy(buf + 2, value.data(), len);
    return 2 + len;
  } else {
    buf[0] = kEnc32BitStr;
    StoreLE32(buf + 1, static_cast<uint32_t>(len));
    std::memcpy(buf + 5, value.data(), len);
    return 5 + len;
  }
}

// Decode integer from encoded entry
int64_t DecodeInteger(const uint8_t* p, size_t* payload_len) {
  uint8_t first = p[0];

  if ((first & kEnc7BitUintMask) == 0) {
    *payload_len = 1;
    return static_cast<int64_t>(first & 0x7F);
  } else if ((first & kEnc13BitIntMask) == kEnc13BitInt) {
    *payload_len = 2;
    int64_t v = ((static_cast<int64_t>(first & 0x1F)) << 8) | p[1];
    // 13-bit sign extension
    if (v & (static_cast<int64_t>(1) << 12))
      v -= (static_cast<int64_t>(1) << 13);
    return v;
  } else if (first == kEnc16BitInt) {
    *payload_len = 3;
    int64_t v = static_cast<int64_t>(p[1]) | (static_cast<int64_t>(p[2]) << 8);
    // 16-bit sign extension
    if (v & (static_cast<int64_t>(1) << 15))
      v -= (static_cast<int64_t>(1) << 16);
    return v;
  } else if (first == kEnc24BitInt) {
    *payload_len = 4;
    int64_t v = static_cast<int64_t>(p[1]) | (static_cast<int64_t>(p[2]) << 8) |
                (static_cast<int64_t>(p[3]) << 16);
    // 24-bit sign extension
    if (v & (static_cast<int64_t>(1) << 23))
      v -= (static_cast<int64_t>(1) << 24);
    return v;
  } else if (first == kEnc32BitInt) {
    *payload_len = 5;
    int64_t v = static_cast<int64_t>(p[1]) | (static_cast<int64_t>(p[2]) << 8) |
                (static_cast<int64_t>(p[3]) << 16) |
                (static_cast<int64_t>(p[4]) << 24);
    // 32-bit sign extension
    if (v & (static_cast<int64_t>(1) << 31))
      v -= (static_cast<int64_t>(1) << 32);
    return v;
  } else {
    // kEnc64BitInt
    *payload_len = 9;
    uint64_t uv = static_cast<uint64_t>(p[1]) |
                  (static_cast<uint64_t>(p[2]) << 8) |
                  (static_cast<uint64_t>(p[3]) << 16) |
                  (static_cast<uint64_t>(p[4]) << 24) |
                  (static_cast<uint64_t>(p[5]) << 32) |
                  (static_cast<uint64_t>(p[6]) << 40) |
                  (static_cast<uint64_t>(p[7]) << 48) |
                  (static_cast<uint64_t>(p[8]) << 56);
    return static_cast<int64_t>(uv);
  }
}

// Encode backlen of payload_size into buf (or compute size if buf is nullptr)
size_t EncodeBacklen(uint8_t* buf, uint64_t payload_size) {
  if (payload_size <= 127) {
    if (buf) buf[0] = static_cast<uint8_t>(payload_size);
    return 1;
  } else if (payload_size < 16383) {
    if (buf) {
      buf[0] = static_cast<uint8_t>(payload_size >> 7);
      buf[1] = static_cast<uint8_t>((payload_size & 127) | 128);
    }
    return 2;
  } else if (payload_size < 2097151) {
    if (buf) {
      buf[0] = static_cast<uint8_t>(payload_size >> 14);
      buf[1] = static_cast<uint8_t>(((payload_size >> 7) & 127) | 128);
      buf[2] = static_cast<uint8_t>((payload_size & 127) | 128);
    }
    return 3;
  } else if (payload_size < 268435455) {
    if (buf) {
      buf[0] = static_cast<uint8_t>(payload_size >> 21);
      buf[1] = static_cast<uint8_t>(((payload_size >> 14) & 127) | 128);
      buf[2] = static_cast<uint8_t>(((payload_size >> 7) & 127) | 128);
      buf[3] = static_cast<uint8_t>((payload_size & 127) | 128);
    }
    return 4;
  } else {
    if (buf) {
      buf[0] = static_cast<uint8_t>(payload_size >> 28);
      buf[1] = static_cast<uint8_t>(((payload_size >> 21) & 127) | 128);
      buf[2] = static_cast<uint8_t>(((payload_size >> 14) & 127) | 128);
      buf[3] = static_cast<uint8_t>(((payload_size >> 7) & 127) | 128);
      buf[4] = static_cast<uint8_t>((payload_size & 127) | 128);
    }
    return 5;
  }
}

// Decode backlen starting from the last byte (p points to last byte of backlen)
uint64_t DecodeBacklen(const uint8_t* p) {
  uint64_t val = 0;
  uint64_t shift = 0;
  while (true) {
    val |= static_cast<uint64_t>(p[0] & 127) << shift;
    if (!(p[0] & 128)) break;
    shift += 7;
    p--;
    if (shift > 28) return UINT64_MAX;
  }
  return val;
}

bool IsIntegerEncoding(uint8_t first_byte) {
  return (first_byte & kEnc7BitUintMask) == 0 ||             // 7-bit uint
         (first_byte & kEnc13BitIntMask) == kEnc13BitInt ||  // 13-bit int
         first_byte == kEnc16BitInt || first_byte == kEnc24BitInt ||
         first_byte == kEnc32BitInt || first_byte == kEnc64BitInt;
}

}  // namespace

// ===== Listpack::Value =====
std::string Listpack::Value::ToString() const {
  if (type == Type::kInteger) {
    return std::to_string(integer);
  }
  return std::string(string);
}

// ===== Listpack public methods =====
Listpack::Listpack() {
  // Initialize with 6-byte header + 1-byte EOF
  buf_.resize(kHdrSize + 1);
  buf_[kHdrSize] = kEof;
  SetHeader(buf_.size(), 0);
}

// static
std::optional<Listpack> Listpack::FromBytes(std::vector<uint8_t> data) {
  std::span<const uint8_t> span(data);
  if (!ValidateBytes(span)) return std::nullopt;
  Listpack lp;
  lp.buf_ = std::move(data);
  return lp;
}

// static
size_t Listpack::EncodedEntrySize(std::string_view value) {
  int64_t int_val;
  if (CanEncodeAsInt(value, &int_val)) {
    return EncodedEntrySize(int_val);
  }
  size_t len = value.size();
  size_t payload;
  if (len < 64)
    payload = 1 + len;
  else if (len < 4096)
    payload = 2 + len;
  else
    payload = 5 + len;
  return payload + EncodeBacklen(nullptr, payload);
}

// static
size_t Listpack::EncodedEntrySize(int64_t value) {
  size_t payload = 0;
  if (value >= 0 && value <= 127) {
    payload = 1;
  } else if (value >= -4096 && value <= 4095) {
    payload = 2;
  } else if (value >= -32768 && value <= 32767) {
    payload = 3;
  } else if (value >= -8388608 && value <= 8388607) {
    payload = 4;
  } else if (value >= -2147483648LL && value <= 2147483647LL) {
    payload = 5;
  } else {
    payload = 9;
  }
  return payload + EncodeBacklen(nullptr, payload);
}

size_t Listpack::Size() const {
  if (buf_.size() < kHdrSize + 1) return 0;
  uint16_t count = LoadLE16(buf_.data() + 4);
  if (count != UINT16_MAX) return count;
  // 溢出回退 — quicklist 节点受 fill 限制不会触发
  return EntryCount();
}

size_t Listpack::TotalBytes() const {
  if (buf_.size() < kHdrSize) return 0;
  return LoadLE32(buf_.data());
}

const uint8_t* Listpack::Data() const { return buf_.data(); }

size_t Listpack::DataSize() const { return buf_.size(); }

std::optional<Listpack::Value> Listpack::Get(size_t index) const {
  if (index >= Size()) return std::nullopt;

  auto pos_opt = Seek(index);
  if (!pos_opt) return std::nullopt;
  size_t pos = *pos_opt;

  if (pos + 1 > buf_.size()) return std::nullopt;

  return DecodeValueAt(pos);
}

Listpack::Value Listpack::DecodeValueAt(size_t pos) const {
  uint8_t first = buf_[pos];

  if (IsIntegerEncoding(first)) {
    size_t payload_len;
    int64_t val = DecodeInteger(buf_.data() + pos, &payload_len);
    return Value{Value::Type::kInteger, {}, val};
  } else {
    // String encoding
    size_t str_len;
    size_t hdr_len;
    if ((first & kEnc6BitStrMask) == kEnc6BitStr) {
      str_len = first & 0x3F;
      hdr_len = 1;
    } else if ((first & kEnc12BitStrMask) == kEnc12BitStr) {
      str_len = ((static_cast<size_t>(first & 0x0F)) << 8) | buf_[pos + 1];
      hdr_len = 2;
    } else {
      // 32-bit str
      str_len = LoadLE32(buf_.data() + pos + 1);
      hdr_len = 5;
    }
    const char* str_data =
        reinterpret_cast<const char*>(buf_.data() + pos + hdr_len);
    return Value{Value::Type::kString, std::string_view(str_data, str_len), 0};
  }
}

std::optional<std::string_view> Listpack::GetString(size_t index) const {
  auto val = Get(index);
  if (!val || val->type != Value::Type::kString) return std::nullopt;
  return val->string;
}

std::optional<int64_t> Listpack::GetInteger(size_t index) const {
  auto val = Get(index);
  if (!val || val->type != Value::Type::kInteger) return std::nullopt;
  return val->integer;
}

bool Listpack::IsString(size_t index) const {
  auto val = Get(index);
  return val && val->type == Value::Type::kString;
}

bool Listpack::IsInteger(size_t index) const {
  auto val = Get(index);
  return val && val->type == Value::Type::kInteger;
}

bool Listpack::Append(std::string_view value) { return Insert(Size(), value); }

bool Listpack::Append(int64_t value) { return Insert(Size(), value); }

bool Listpack::Prepend(std::string_view value) { return Insert(0, value); }

bool Listpack::Prepend(int64_t value) { return Insert(0, value); }

bool Listpack::Insert(size_t index, std::string_view value) {
  // Check if this string can be encoded as integer
  int64_t int_val;
  if (CanEncodeAsInt(value, &int_val)) {
    return Insert(index, int_val);
  }

  // String encoding: compute payload (encoding header + data)
  size_t len = value.size();
  size_t payload_size;
  if (len < 64)
    payload_size = 1 + len;
  else if (len < 4096)
    payload_size = 2 + len;
  else
    payload_size = 5 + len;

  size_t pos = SeekInsertPosition(index);
  if (pos > buf_.size()) return false;

  size_t backlen_size = EncodeBacklen(nullptr, payload_size);
  size_t entry_size = payload_size + backlen_size;

  // Insert entry before EOF
  size_t insert_pos = pos;
  buf_.insert(buf_.begin() + static_cast<long>(insert_pos), entry_size, 0);

  // Encode string at insert_pos
  EncodeString(buf_.data() + insert_pos, value);

  // Write backlen
  EncodeBacklen(buf_.data() + insert_pos + payload_size, payload_size);

  SetTotalBytesFromBuffer();
  AdjustHeaderCount(1);
  return true;
}

bool Listpack::Insert(size_t index, int64_t value) {
  size_t pos = SeekInsertPosition(index);
  if (pos > buf_.size()) return false;

  size_t payload_size = 1;  // default for 7-bit uint
  if (value >= 0 && value <= 127) {
    payload_size = 1;
  } else if (value >= -4096 && value <= 4095) {
    payload_size = 2;
  } else if (value >= -32768 && value <= 32767) {
    payload_size = 3;
  } else if (value >= -8388608 && value <= 8388607) {
    payload_size = 4;
  } else if (value >= -2147483648LL && value <= 2147483647LL) {
    payload_size = 5;
  } else {
    payload_size = 9;
  }

  size_t backlen_size = EncodeBacklen(nullptr, payload_size);
  size_t entry_size = payload_size + backlen_size;

  size_t insert_pos = pos;
  buf_.insert(buf_.begin() + static_cast<long>(insert_pos), entry_size, 0);

  // Encode integer at insert_pos
  EncodeInteger(buf_.data() + insert_pos, value);

  // Write backlen
  EncodeBacklen(buf_.data() + insert_pos + payload_size, payload_size);

  SetTotalBytesFromBuffer();
  AdjustHeaderCount(1);
  return true;
}

bool Listpack::Delete(size_t index) {
  if (index >= Size()) return false;

  auto pos_opt = Seek(index);
  if (!pos_opt) return false;
  size_t pos = *pos_opt;
  size_t entry_size = EntrySizeAt(pos);

  buf_.erase(buf_.begin() + static_cast<long>(pos),
             buf_.begin() + static_cast<long>(pos + entry_size));
  SetTotalBytesFromBuffer();
  AdjustHeaderCount(-1);
  return true;
}

std::optional<std::string> Listpack::PopBack() {
  auto pos = SeekLast();
  if (!pos) return std::nullopt;

  // Own the value before invalidating its view or changing the container.
  // If allocation fails, the listpack is still unchanged.
  std::string result = DecodeValueAt(*pos).ToString();
  buf_.resize(*pos + 1);
  buf_[*pos] = kEof;
  SetTotalBytesFromBuffer();
  AdjustHeaderCount(-1);
  return result;
}

bool Listpack::Replace(size_t index, std::string_view value) {
  if (index >= Size()) return false;

  int64_t int_val;
  if (CanEncodeAsInt(value, &int_val)) {
    return Replace(index, int_val);
  }

  // Delete old and insert new (since sizes may differ)
  Delete(index);
  return Insert(index, value);
}

bool Listpack::Replace(size_t index, int64_t value) {
  if (index >= Size()) return false;

  Delete(index);
  return Insert(index, value);
}

std::optional<size_t> Listpack::Find(std::string_view value) const {
  // Check if value can be encoded as integer - if so, search both encodings
  int64_t int_val;
  if (CanEncodeAsInt(value, &int_val)) {
    return Find(int_val);
  }

  // Linear scan for string match
  size_t pos = kHdrSize;
  size_t idx = 0;
  size_t count = Size();
  while (idx < count) {
    if (buf_[pos] == kEof) break;
    uint8_t first = buf_[pos];
    if (!IsIntegerEncoding(first)) {
      size_t str_len;
      size_t hdr_len;
      if ((first & kEnc6BitStrMask) == kEnc6BitStr) {
        str_len = first & 0x3F;
        hdr_len = 1;
      } else if ((first & kEnc12BitStrMask) == kEnc12BitStr) {
        if (pos + 1 >= buf_.size()) break;
        str_len = ((static_cast<size_t>(first & 0x0F)) << 8) | buf_[pos + 1];
        hdr_len = 2;
      } else {
        if (pos + 4 >= buf_.size()) break;
        str_len = LoadLE32(buf_.data() + pos + 1);
        hdr_len = 5;
      }
      if (str_len == value.size() && std::memcmp(buf_.data() + pos + hdr_len,
                                                 value.data(), str_len) == 0) {
        return idx;
      }
    }
    pos += EntrySizeAt(pos);
    idx++;
  }
  return std::nullopt;
}

std::optional<size_t> Listpack::Find(int64_t value) const {
  size_t pos = kHdrSize;
  size_t idx = 0;
  size_t count = Size();
  while (idx < count) {
    if (buf_[pos] == kEof) break;
    uint8_t first = buf_[pos];
    if (IsIntegerEncoding(first)) {
      size_t payload_len;
      int64_t decoded = DecodeInteger(buf_.data() + pos, &payload_len);
      if (decoded == value) return idx;
    }
    pos += EntrySizeAt(pos);
    idx++;
  }
  return std::nullopt;
}

// ===== Private methods =====
size_t Listpack::EntryCount() const {
  if (buf_.size() < kHdrSize + 1) return 0;

  // Scan to count (always accurate)
  size_t count = 0;
  size_t pos = kHdrSize;
  while (pos < buf_.size() && buf_[pos] != kEof) {
    count++;
    pos += EntrySizeAt(pos);
  }
  return count;
}

std::optional<size_t> Listpack::Seek(size_t index) const {
  size_t pos = kHdrSize;
  size_t count = Size();
  for (size_t i = 0; i < index && i < count; i++) {
    pos += EntrySizeAt(pos);
    if (pos >= buf_.size()) return std::nullopt;
  }
  if (pos >= buf_.size()) return std::nullopt;
  return pos;
}

std::optional<size_t> Listpack::SeekLast() const {
  // Size() can scan when the header count is unknown. The byte length is
  // sufficient to distinguish an empty listpack and locate its EOF.
  if (buf_.size() <= kHdrSize + 1) return std::nullopt;
  const size_t eof_pos = buf_.size() - 1;
  const size_t payload = DecodeBacklenEndingAt(eof_pos - 1);
  return eof_pos - payload - EncodeBacklen(nullptr, payload);
}

size_t Listpack::SeekInsertPosition(size_t index) const {
  if (index >= Size()) {
    // Append: insert before EOF
    if (buf_.size() >= 1) return buf_.size() - 1;  // position of EOF
    return buf_.size();
  }
  auto pos_opt = Seek(index);
  return pos_opt.value_or(buf_.size());
}

size_t Listpack::EncodedPayloadSizeAt(size_t pos) const {
  if (pos >= buf_.size()) return 0;
  uint8_t first = buf_[pos];

  if ((first & kEnc7BitUintMask) == 0) return 1;
  if ((first & kEnc13BitIntMask) == kEnc13BitInt) return 2;
  if (first == kEnc16BitInt) return 3;
  if (first == kEnc24BitInt) return 4;
  if (first == kEnc32BitInt) return 5;
  if (first == kEnc64BitInt) return 9;

  // String
  if ((first & kEnc6BitStrMask) == kEnc6BitStr) return 1 + (first & 0x3F);
  if ((first & kEnc12BitStrMask) == kEnc12BitStr) {
    if (pos + 1 >= buf_.size()) return 0;
    return 2 + ((static_cast<size_t>(first & 0x0F) << 8) | buf_[pos + 1]);
  }
  // 32-bit string
  if (pos + 4 >= buf_.size()) return 0;
  return 5 + LoadLE32(buf_.data() + pos + 1);
}

size_t Listpack::DecodeBacklenEndingAt(size_t pos) const {
  if (pos >= buf_.size()) return 0;
  return static_cast<size_t>(DecodeBacklen(buf_.data() + pos));
}

size_t Listpack::EntrySizeAt(size_t pos) const {
  size_t payload = EncodedPayloadSizeAt(pos);
  if (payload == 0) return 0;
  size_t backlen_size = EncodeBacklen(nullptr, payload);
  return payload + backlen_size;
}

void Listpack::SetHeader(size_t total_bytes, uint16_t count_header) {
  if (buf_.size() < kHdrSize + 1) return;
  StoreLE32(buf_.data(), static_cast<uint32_t>(total_bytes));
  StoreLE16(buf_.data() + 4, count_header);
}

void Listpack::SetTotalBytesFromBuffer() {
  if (buf_.size() < kHdrSize) return;
  StoreLE32(buf_.data(), static_cast<uint32_t>(buf_.size()));
}

void Listpack::AdjustHeaderCount(int delta) {
  if (buf_.size() < kHdrSize + 1) return;

  uint16_t count = LoadLE16(buf_.data() + 4);
  if (count == UINT16_MAX) return;

  if (delta > 0) {
    size_t next = static_cast<size_t>(count) + static_cast<size_t>(delta);
    StoreLE16(buf_.data() + 4,
              next >= UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(next));
    return;
  }

  size_t decrement = static_cast<size_t>(-delta);
  StoreLE16(buf_.data() + 4,
            decrement >= count ? 0 : static_cast<uint16_t>(count - decrement));
}

// static
bool Listpack::ValidateBytes(std::span<const uint8_t> data) {
  if (data.size() < kHdrSize + 1) return false;

  uint32_t total_bytes = LoadLE32(data.data());
  if (total_bytes != data.size()) return false;

  // Check EOF marker
  if (data[data.size() - 1] != kEof) return false;

  // Validate each entry and count
  size_t pos = kHdrSize;
  size_t actual_count = 0;
  while (pos < data.size() - 1) {  // -1 to account for EOF
    if (pos >= data.size()) return false;
    if (data[pos] == kEof) return false;

    size_t payload = 0;
    uint8_t first = data[pos];

    if ((first & kEnc7BitUintMask) == 0) {
      payload = 1;
    } else if ((first & kEnc13BitIntMask) == kEnc13BitInt) {
      payload = 2;
    } else if (first == kEnc16BitInt) {
      payload = 3;
    } else if (first == kEnc24BitInt) {
      payload = 4;
    } else if (first == kEnc32BitInt) {
      payload = 5;
    } else if (first == kEnc64BitInt) {
      payload = 9;
    } else if ((first & kEnc6BitStrMask) == kEnc6BitStr) {
      payload = 1 + (first & 0x3F);
    } else if ((first & kEnc12BitStrMask) == kEnc12BitStr) {
      if (pos + 1 >= data.size()) return false;
      payload = 2 + ((static_cast<size_t>(first & 0x0F) << 8) | data[pos + 1]);
    } else if (first == kEnc32BitStr) {
      if (pos + 4 >= data.size()) return false;
      payload = 5 + LoadLE32(data.data() + pos + 1);
    } else {
      return false;  // Unknown encoding
    }

    size_t backlen_size = EncodeBacklen(nullptr, payload);
    if (pos + payload + backlen_size > data.size()) return false;

    // Validate backlen
    uint64_t decoded =
        DecodeBacklen(data.data() + pos + payload + backlen_size - 1);
    if (decoded != payload) return false;

    pos += payload + backlen_size;
    actual_count++;
  }

  // Should end at EOF
  if (pos != data.size() - 1) return false;

  // Validate numele matches actual count (or is UINT16_MAX = unknown)
  uint16_t header_numele = LoadLE16(data.data() + 4);
  if (header_numele != UINT16_MAX &&
      header_numele != static_cast<uint16_t>(actual_count)) {
    return false;
  }

  return true;
}

// ===== Iterator =====

Listpack::Iterator::Iterator(const Listpack* lp, size_t pos, size_t index)
    : lp_(lp), pos_(pos), index_(index) {}

Listpack::Value Listpack::Iterator::operator*() const {
  return lp_->DecodeValueAt(pos_);
}

bool Listpack::Iterator::Valid() const {
  return lp_ != nullptr && pos_ < lp_->buf_.size() && lp_->buf_[pos_] != kEof;
}

Listpack::Iterator& Listpack::Iterator::operator++() {
  if (!Valid()) return *this;
  size_t entry_size = lp_->EntrySizeAt(pos_);
  if (entry_size == 0) {
    pos_ = lp_->buf_.size();  // past end
    return *this;
  }
  pos_ += entry_size;
  index_++;
  return *this;
}

Listpack::Iterator Listpack::Iterator::operator++(int) {
  Iterator tmp = *this;
  ++(*this);
  return tmp;
}

Listpack::Iterator& Listpack::Iterator::operator--() {
  if (pos_ <= kHdrSize) return *this;  // already at first
  // pos_ - 1 is last byte of previous entry's backlen
  uint64_t prev_payload = DecodeBacklen(lp_->buf_.data() + pos_ - 1);
  size_t prev_backlen_size = EncodeBacklen(nullptr, prev_payload);
  pos_ = pos_ - prev_payload - prev_backlen_size;
  index_--;
  return *this;
}

Listpack::Iterator Listpack::Iterator::operator--(int) {
  Iterator tmp = *this;
  --(*this);
  return tmp;
}

bool Listpack::Iterator::operator==(const Iterator& other) const {
  return lp_ == other.lp_ && pos_ == other.pos_;
}

bool Listpack::Iterator::operator!=(const Iterator& other) const {
  return !(*this == other);
}

Listpack::Iterator Listpack::begin() const {
  return Iterator(this, kHdrSize, 0);
}

Listpack::Iterator Listpack::end() const {
  size_t eof_pos = (buf_.size() >= 1) ? buf_.size() - 1 : 0;
  return Iterator(this, eof_pos, Size());
}

}  // namespace ds
}  // namespace miniredis
