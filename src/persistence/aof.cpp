#include "persistence/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/database.h"
#include "core/server.h"
#include "types/value.h"

namespace miniredis {

// --- AofWriter ---

AofWriter::~AofWriter() { Close(); }

bool AofWriter::Open(const std::string& filepath,
                     const std::string& appendfsync) {
  filepath_ = filepath;
  fd_ =
      ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) return false;

  if (appendfsync == "always")
    appendfsync_ = 0;
  else if (appendfsync == "no")
    appendfsync_ = 2;
  else
    appendfsync_ = 1;

  struct stat st {};
  if (::fstat(fd_, &st) == 0) {
    current_size_ = static_cast<size_t>(st.st_size);
  }
  return true;
}

void AofWriter::Close() {
  if (fd_ >= 0) {
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

std::string AofWriter::EncodeArg(const std::string& arg) {
  return "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
}

bool AofWriter::WriteBuf(const std::string& data) {
  if (fd_ < 0) return false;
  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    written += static_cast<size_t>(n);
  }
  current_size_ += data.size();
  return true;
}

bool AofWriter::AppendCommand(const std::vector<std::string>& args) {
  if (fd_ < 0) return false;

  std::string buf = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& arg : args) {
    buf += EncodeArg(arg);
  }

  if (!WriteBuf(buf)) return false;

  if (appendfsync_ == 0) {
    ::fsync(fd_);
  }
  return true;
}

bool AofWriter::AppendCommandForDb(int db_index,
                                   const std::vector<std::string>& args,
                                   int& selected_db) {
  if (db_index < 0) return false;
  if (db_index != selected_db) {
    if (!AppendCommand({"SELECT", std::to_string(db_index)})) return false;
    selected_db = db_index;
  }
  return AppendCommand(args);
}

void AofWriter::FlushIfNeeded(int64_t /*now_ms*/) {
  if (fd_ < 0 || appendfsync_ != 1) return;

  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fsync_);
  if (elapsed.count() >= 1000) {
    ::fsync(fd_);
    last_fsync_ = now;
  }
}

namespace {

void AppendKeyValueToAof(const KeyView& kv, std::string& data) {
  ValueType type = GetType(kv.value);
  std::string key(kv.key);

  auto append_resp = [&](const std::vector<std::string>& args) {
    data += "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
      data += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    }
  };

  switch (type) {
    case ValueType::kString: {
      auto* sv = std::get_if<StringValue>(&kv.value);
      if (!sv) break;
      append_resp({"SET", key, sv->ToString()});
      break;
    }
    case ValueType::kList: {
      auto* lv = std::get_if<ListValue>(&kv.value);
      if (!lv) break;
      auto elems = const_cast<ListValue*>(lv)->Range(0, -1);
      std::vector<std::string> args = {"RPUSH", key};
      args.insert(args.end(), elems.begin(), elems.end());
      append_resp(args);
      break;
    }
    case ValueType::kSet: {
      auto* sv = std::get_if<SetValue>(&kv.value);
      if (!sv) break;
      auto members = const_cast<SetValue*>(sv)->Members();
      std::vector<std::string> args = {"SADD", key};
      args.insert(args.end(), members.begin(), members.end());
      append_resp(args);
      break;
    }
    case ValueType::kHash: {
      auto* hv = std::get_if<HashValue>(&kv.value);
      if (!hv) break;
      auto pairs = const_cast<HashValue*>(hv)->GetAll();
      std::vector<std::string> args = {"HSET", key};
      for (const auto& [f, v] : pairs) {
        args.push_back(f);
        args.push_back(v);
      }
      append_resp(args);
      break;
    }
    case ValueType::kZSet: {
      auto* zv = std::get_if<ZSetValue>(&kv.value);
      if (!zv) break;
      auto range = const_cast<ZSetValue*>(zv)->Range(0, -1);
      std::vector<std::string> args = {"ZADD", key};
      for (const auto& entry : range) {
        args.push_back(std::to_string(entry.score));
        args.push_back(entry.element);
      }
      append_resp(args);
      break;
    }
  }

  // Expire
  if (kv.expire_at_ms.has_value()) {
    append_resp({"PEXPIREAT", key, std::to_string(*kv.expire_at_ms)});
  }
}

}  // namespace

bool AofWriter::RewriteFromSnapshot(Server& server) {
  std::string tmp_path = filepath_ + ".rewrite.tmp";
  std::string data;

  int db_count = server.DbCount();
  for (int i = 0; i < db_count; i++) {
    Database* db = server.GetDb(i);
    if (!db || db->Size() == 0) continue;

    // SELECT db
    if (i != 0) {
      std::string db_str = std::to_string(i);
      data += "*2\r\n$6\r\nSELECT\r\n$" + std::to_string(db_str.size()) +
              "\r\n" + db_str + "\r\n";
    }

    db->ForEachKey(
        [&data](const KeyView& kv) { AppendKeyValueToAof(kv, data); });
  }

  // Write temp file
  {
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file.good()) return false;
  }

  // Atomic rename
  if (::rename(tmp_path.c_str(), filepath_.c_str()) != 0) return false;
  return true;
}

// --- AofReader ---

bool AofReader::ReadCommands(const std::string& filepath,
                             std::vector<std::vector<std::string>>& out) {
  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;
  std::streamsize fsize = file.tellg();
  file.seekg(0, std::ios::beg);
  std::string data(static_cast<size_t>(fsize), '\0');
  if (!file.read(&data[0], fsize)) return false;
  file.close();

  size_t pos = 0;

  while (pos < data.size()) {
    if (data[pos] != '*') return false;
    pos++;

    size_t end = data.find("\r\n", pos);
    if (end == std::string::npos) return false;
    int count = std::stoi(data.substr(pos, end - pos));
    pos = end + 2;

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; i++) {
      if (pos >= data.size() || data[pos] != '$') return false;
      pos++;

      end = data.find("\r\n", pos);
      if (end == std::string::npos) return false;
      int len = std::stoi(data.substr(pos, end - pos));
      pos = end + 2;

      if (pos + static_cast<size_t>(len) + 2 > data.size()) return false;
      args.push_back(data.substr(pos, static_cast<size_t>(len)));
      pos += static_cast<size_t>(len) + 2;
    }

    out.push_back(std::move(args));
  }

  return true;
}

}  // namespace miniredis
