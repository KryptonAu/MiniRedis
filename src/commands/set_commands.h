#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterSetCommands(CommandRegistry& registry);
}  // namespace miniredis
