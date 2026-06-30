#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "commands/command_args.h"

namespace miniredis {

struct CommandContext;

enum class CommandFlag : uint32_t {
  kReadOnly = 1u << 0,
  kWrite = 1u << 1,
  kAdmin = 1u << 2,
  kMayCreateKey = 1u << 3,
};

inline uint32_t operator|(CommandFlag a, CommandFlag b) {
  return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}
inline uint32_t operator|(uint32_t a, CommandFlag b) {
  return a | static_cast<uint32_t>(b);
}

struct CommandInfo {
  using Func = std::string (*)(CommandContext&, CommandArgs);
  std::string name;
  int arity;
  uint32_t flags = 0;
  Func func = nullptr;
};

class CommandRegistry {
 public:
  void Register(CommandInfo info);
  const CommandInfo* Find(std::string_view name) const;
  size_t Size() const;
  std::vector<std::string> CommandNames() const;

 private:
  std::unordered_map<std::string, CommandInfo> commands_;
};

CommandRegistry CreateDefaultCommandRegistry();
void RegisterAllCommands(CommandRegistry& registry);

}  // namespace miniredis
