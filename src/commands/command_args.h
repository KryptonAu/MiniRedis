#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {

using CommandArgs = std::span<const std::string_view>;

inline std::vector<std::string> ToOwnedArgs(CommandArgs args) {
  std::vector<std::string> owned;
  owned.reserve(args.size());
  for (std::string_view arg : args) {
    owned.emplace_back(arg);
  }
  return owned;
}

inline std::vector<std::string_view> ToArgViews(
    const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.emplace_back(arg);
  }
  return views;
}

}  // namespace miniredis
