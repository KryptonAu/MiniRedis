#include "core/server.h"

namespace miniredis {

Server& Server::Instance() {
  static Server instance;
  return instance;
}

bool Server::Init(const MiniRedisConfig& config) {
  if (config.databases < 1) return false;
  config_ = config;
  databases_.clear();
  databases_.resize(static_cast<size_t>(config.databases));
  clients_.Clear();
  running_ = true;
  return true;
}

Database* Server::GetDb(int index) {
  if (index < 0 || index >= static_cast<int>(databases_.size())) return nullptr;
  return &databases_[static_cast<size_t>(index)];
}

const Database* Server::GetDb(int index) const {
  if (index < 0 || index >= static_cast<int>(databases_.size())) return nullptr;
  return &databases_[static_cast<size_t>(index)];
}

Database* Server::GetDbFor(const Client& client) {
  return GetDb(client.CurrentDb());
}

std::optional<size_t> Server::DbSize(int index) {
  auto* db = GetDb(index);
  if (!db) return std::nullopt;
  return db->Size();
}

bool Server::FlushDb(int index) {
  auto* db = GetDb(index);
  if (!db) return false;
  db->Clear();
  return true;
}

void Server::FlushAll() {
  for (auto& db : databases_) db.Clear();
}

int Server::DbCount() const { return static_cast<int>(databases_.size()); }

Client* Server::CreateClient(int fd) {
  if (FindClient(fd)) return nullptr;  // fd already exists
  auto client = std::make_unique<Client>(fd, 0);
  auto* raw = client.get();
  clients_.Set(fd, std::move(client));
  return raw;
}

void Server::RemoveClient(int fd) { clients_.Delete(fd); }

Client* Server::FindClient(int fd) {
  auto* ptr = clients_.Find(fd);
  return ptr ? ptr->get() : nullptr;
}

size_t Server::ClientCount() const { return clients_.Size(); }

const MiniRedisConfig& Server::GetConfig() const { return config_; }

EncodingThresholds Server::GetEncodingThresholds() const {
  EncodingThresholds t;
  t.set_max_intset_entries = config_.set_max_intset_entries;
  t.hash_max_listpack_entries = config_.hash_max_listpack_entries;
  t.hash_max_listpack_value = config_.hash_max_listpack_value;
  t.zset_max_listpack_entries = config_.zset_max_listpack_entries;
  t.zset_max_listpack_value = config_.zset_max_listpack_value;
  return t;
}

bool Server::IsRunning() const { return running_; }

void Server::Shutdown() {
  clients_.Clear();
  databases_.clear();
  running_ = false;
}

}  // namespace miniredis
