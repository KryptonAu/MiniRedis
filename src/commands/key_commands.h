#pragma once
namespace miniredis {
class CommandRegistry;
void RegisterKeyCommands(CommandRegistry& registry);
}  // namespace miniredis
