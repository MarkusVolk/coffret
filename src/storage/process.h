#pragma once

#include <expected>
#include <string>
#include <vector>

namespace coffret {
  // Runs a program to its end and hands back what it wrote; its complaint when it failed. Blocks:
  // for worker threads.
  std::expected<std::string, std::string> runCommand(const std::vector<std::string>& command);
} // namespace coffret
