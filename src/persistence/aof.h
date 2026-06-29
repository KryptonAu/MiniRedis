#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace miniredis {

class Server;

// AOF writer: appends RESP-encoded commands to the AOF file.
class AofWriter {
 public:
  ~AofWriter();

  bool Open(const std::string& filepath, const std::string& appendfsync);
  void Close();

  // Append a single command as a RESP array of bulk strings.
  bool AppendCommand(const std::vector<std::string>& args);

  // Append SELECT first when the logical AOF DB differs from db_index.
  bool AppendCommandForDb(int db_index, const std::vector<std::string>& args,
                          int& selected_db);

  // Flush according to the configured fsync policy.
  void FlushIfNeeded(int64_t now_ms);

  // Test helper: synchronously rewrite AOF from current server state.
  bool RewriteFromSnapshot(Server& server);

  bool IsOpen() const { return fd_ >= 0; }
  size_t GetCurrentSize() const { return current_size_; }
  int GetFd() const { return fd_; }

 private:
  int fd_ = -1;
  std::string filepath_;
  size_t current_size_ = 0;
  std::chrono::steady_clock::time_point last_fsync_;
  int appendfsync_ = 1;  // 0=always, 1=everysec, 2=no

  static std::string EncodeArg(const std::string& arg);
  bool WriteBuf(const std::string& data);
};

// AOF reader: parses RESP commands from an AOF file (no CommandRegistry dep).
class AofReader {
 public:
  bool ReadCommands(const std::string& filepath,
                    std::vector<std::vector<std::string>>& out);
};

}  // namespace miniredis
