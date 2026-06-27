#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/resp_protocol.h"

namespace miniredis {

class Client {
 public:
  Client(int fd, int db_index = 0);
  ~Client();

  int Fd() const;
  size_t Id() const;

  int CurrentDb() const;
  bool SelectDb(int index, int db_count);

  std::vector<uint8_t>& QueryBuffer();
  const std::vector<uint8_t>& QueryBuffer() const;
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
  std::vector<uint8_t> query_buffer_;
  RespParser parser_;
  std::string reply_buffer_;
  bool authenticated_;
  std::string name_;

  static size_t next_id_;
};

}  // namespace miniredis
