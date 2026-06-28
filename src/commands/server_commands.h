#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterServerCommands(CommandRegistry& registry);
}  // namespace miniredis
