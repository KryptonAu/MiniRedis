#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterHashCommands(CommandRegistry& registry);
}  // namespace miniredis
