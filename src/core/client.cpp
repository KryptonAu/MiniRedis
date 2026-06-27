#include "core/client.h"

namespace miniredis {

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

std::vector<uint8_t>& Client::QueryBuffer() { return query_buffer_; }

const std::vector<uint8_t>& Client::QueryBuffer() const {
  return query_buffer_;
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
