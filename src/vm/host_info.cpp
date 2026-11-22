#include "vm/host_info.h"

#include <algorithm>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

std::string coffret::defaultRenderNode() {
  std::vector<std::string> nodes;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("renderD")) {
      continue;
    }
    const auto driver = std::filesystem::read_symlink(entry.path() / "device" / "driver", error).filename().string();
    if (!error && driver != "vgem" && driver != "vkms" && ::access(("/dev/dri/" + name).c_str(), R_OK | W_OK) == 0) {
      nodes.push_back("/dev/dri/" + name);
    }
    error.clear();
  }
  std::ranges::sort(nodes);
  return nodes.empty() ? std::string() : nodes.front();
}

unsigned coffret::hostProcessors() { return std::max(1U, std::thread::hardware_concurrency()); }

unsigned coffret::hostMemoryMiB() {
  const long pages = ::sysconf(_SC_PHYS_PAGES), size = ::sysconf(_SC_PAGE_SIZE);
  return pages > 0 && size > 0 ? static_cast<unsigned>(static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(size) >> 20) : 0;
}
