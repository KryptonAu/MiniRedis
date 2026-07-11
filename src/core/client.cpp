#include "core/client.h"

#include <cassert>
#include <cstring>

namespace miniredis {

std::span<char> QueryBuffer::PrepareWrite(size_t min_writable) {
  assert(read_pos_ <= write_pos_);
  assert(write_pos_ <= buffer_.size());

  if (min_writable == 0 && write_pos_ == buffer_.size()) return {};

  size_t tail = buffer_.size() - write_pos_;
  if (tail < min_writable && read_pos_ > 0) {
    size_t readable = ReadableSize();
    if (readable > 0) {
      std::memmove(buffer_.data(), buffer_.data() + read_pos_, readable);
    }
    read_pos_ = 0;
    write_pos_ = readable;
    tail = buffer_.size() - write_pos_;
  }

  if (tail < min_writable) {
    buffer_.resize(write_pos_ + min_writable);
  }

  return std::span<char>(buffer_.data() + write_pos_,
                         buffer_.size() - write_pos_);
}

void QueryBuffer::CommitWrite(size_t bytes) {
  assert(bytes <= buffer_.size() - write_pos_);
  write_pos_ += bytes;
}

std::string_view QueryBuffer::Readable() const {
  if (ReadableSize() == 0) return {};
  return std::string_view(buffer_.data() + read_pos_, ReadableSize());
}

void QueryBuffer::Consume(size_t bytes) {
  assert(bytes <= ReadableSize());
  read_pos_ += bytes;
  if (read_pos_ == write_pos_) {
    read_pos_ = 0;
    write_pos_ = 0;
  }
}

void QueryBuffer::Clear() {
  read_pos_ = 0;
  write_pos_ = 0;
}

size_t Client::next_id_ = 1;

Client::Client(int fd, int db_index)
    : fd_(fd), id_(next_id_++), db_index_(db_index), authenticated_(false) {}

Client::~Client() = default;

int Client::Fd() const { return fd_; }

size_t Client::Id() const { return id_; }

int Client::CurrentDb() const { return db_index_; }

bool Client::SelectDb(int index, int db_count) {
  if (index < 0 || index >= db_count) return false;
  db_index_ = index;
  return true;
}

miniredis::QueryBuffer& Client::QueryBuffer() { return query_buffer_; }

const miniredis::QueryBuffer& Client::QueryBuffer() const {
  return query_buffer_;
}

CommandArgStorage& Client::ArgStorage() { return command_arg_storage_; }

const CommandArgStorage& Client::ArgStorage() const {
  return command_arg_storage_;
}

RespParser& Client::Parser() { return parser_; }

const RespParser& Client::Parser() const { return parser_; }

std::string& Client::ReplyBuffer() { return reply_buffer_; }

const std::string& Client::ReplyBuffer() const { return reply_buffer_; }

bool Client::IsAuthenticated() const { return authenticated_; }

void Client::SetAuthenticated(bool auth) { authenticated_ = auth; }

std::string Client::Name() const { return name_; }

void Client::SetName(std::string_view name) { name_ = std::string(name); }

}  // namespace miniredis
