#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterStringCommands(CommandRegistry& registry);
}  // namespace miniredis
