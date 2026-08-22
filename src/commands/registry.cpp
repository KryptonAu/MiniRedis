#include "commands/registry.h"

#include <algorithm>
#include <array>
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

namespace {

// Command names are short (the longest registered name is well below this),
// so lookups can normalize into a fixed-size stack buffer instead of
// allocating a std::string on every dispatch.
constexpr size_t kMaxCommandNameLength = 64;

// Writes the uppercase form of `name` into `out`. `out` must have room for at
// least name.size() characters; it may alias `name` for in-place
// normalization.
void UpperCaseInto(char* out, std::string_view name) {
  for (size_t i = 0; i < name.size(); ++i) {
    out[i] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
  }
}

}  // namespace

void CommandRegistry::Register(CommandInfo info) {
  // Normalize the name in place — no temporary string.
  UpperCaseInto(info.name.data(), info.name);
  assert(commands_.find(info.name) == commands_.end() &&
         "Duplicate command registration");
  commands_.emplace(info.name, std::move(info));
}

const CommandInfo* CommandRegistry::Find(std::string_view name) const {
  // C++20 heterogeneous lookup: normalize into a stack buffer (command names
  // are short) and pass a string_view to find() — no std::string temporary
  // or heap allocation.
  if (name.size() > kMaxCommandNameLength) {
    return nullptr;  // No registered command name is this long.
  }
  std::array<char, kMaxCommandNameLength> buffer;
  UpperCaseInto(buffer.data(), name);
  auto it = commands_.find(std::string_view(buffer.data(), name.size()));
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
