#include "commands/registry.h"

#include <algorithm>
#include <cassert>
#include <cctype>

#include "commands/hash_commands.h"
#include "commands/key_commands.h"
#include "commands/list_commands.h"
#include "commands/server_commands.h"
#include "commands/set_commands.h"
#include "commands/string_commands.h"
#include "commands/zset_commands.h"

namespace miniredis {

static std::string NormalizeName(std::string_view name) {
  std::string result;
  result.reserve(name.size());
  for (char c : name)
    result.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return result;
}

void CommandRegistry::Register(CommandInfo info) {
  std::string key = NormalizeName(info.name);
  assert(commands_.find(key) == commands_.end() &&
         "Duplicate command registration");
  commands_.emplace(std::move(key), std::move(info));
}

const CommandInfo* CommandRegistry::Find(std::string_view name) const {
  std::string key = NormalizeName(name);
  auto it = commands_.find(key);
  return it != commands_.end() ? &it->second : nullptr;
}

size_t CommandRegistry::Size() const { return commands_.size(); }

std::vector<std::string> CommandRegistry::CommandNames() const {
  std::vector<std::string> names;
  names.reserve(commands_.size());
  for (const auto& [name, _] : commands_) names.push_back(name);
  std::sort(names.begin(), names.end());
  return names;
}

CommandRegistry CreateDefaultCommandRegistry() {
  CommandRegistry registry;
  RegisterAllCommands(registry);
  return registry;
}

void RegisterAllCommands(CommandRegistry& registry) {
  RegisterServerCommands(registry);
  RegisterKeyCommands(registry);
  RegisterStringCommands(registry);
  RegisterListCommands(registry);
  RegisterSetCommands(registry);
  RegisterHashCommands(registry);
  RegisterZSetCommands(registry);
}

}  // namespace miniredis
