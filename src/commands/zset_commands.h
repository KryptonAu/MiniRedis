#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterZSetCommands(CommandRegistry& registry);
}  // namespace miniredis
