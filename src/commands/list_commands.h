#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterListCommands(CommandRegistry& registry);
}  // namespace miniredis
