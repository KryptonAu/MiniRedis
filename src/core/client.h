#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/resp_protocol.h"

namespace miniredis {

class QueryBuffer {
 public:
  static constexpr size_t kDefaultReadSize = 64 * 1024;

  QueryBuffer() = default;

  std::span<char> PrepareWrite(size_t min_writable = kDefaultReadSize);
  void CommitWrite(size_t bytes);
  std::string_view Readable() const;
  void Consume(size_t bytes);
  void Clear();

  size_t ReadableSize() const { return write_pos_ - read_pos_; }
  size_t Capacity() const { return buffer_.size(); }
  bool empty() const { return ReadableSize() == 0; }

 private:
  std::vector<char> buffer_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
};

class Client {
 public:
  Client(int fd, int db_index = 0);
  ~Client();

  int Fd() const;
  size_t Id() const;

  int CurrentDb() const;
  bool SelectDb(int index, int db_count);

  miniredis::QueryBuffer& QueryBuffer();
  const miniredis::QueryBuffer& QueryBuffer() const;
  RespParser& Parser();
  const RespParser& Parser() const;

  std::string& ReplyBuffer();
  const std::string& ReplyBuffer() const;

  bool IsAuthenticated() const;
  void SetAuthenticated(bool auth);

  std::string Name() const;
  void SetName(std::string_view name);

 private:
  int fd_;
  size_t id_;
  int db_index_;
  miniredis::QueryBuffer query_buffer_;
  RespParser parser_;
  std::string reply_buffer_;
  bool authenticated_;
  std::string name_;

  static size_t next_id_;
};

}  // namespace miniredis
