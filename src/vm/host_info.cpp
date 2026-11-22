#include "vm/host_info.h"

#include "coffret_build_info.h"
#include "storage/process.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
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
    // vgem and vkms sit on no real bus: "faux" on newer kernels, "platform" with their own driver on older ones.
    const auto driver = std::filesystem::read_symlink(entry.path() / "device" / "driver", error).filename().string();
    const auto bus = std::filesystem::read_symlink(entry.path() / "device" / "subsystem", error).filename().string();
    if (!error && bus != "faux" && driver != "vgem" && driver != "vkms" && ::access(("/dev/dri/" + name).c_str(), R_OK | W_OK) == 0) {
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

std::string coffret::findVirtiofsd() {
  std::vector<std::string> places{"/usr/libexec/virtiofsd", "/usr/lib/virtiofsd", "/usr/lib/qemu/virtiofsd"};
  const char* path = std::getenv("PATH");
  for (std::string_view rest = path ? path : ""; !rest.empty();) {
    const auto colon = rest.find(':');
    places.push_back(std::string(rest.substr(0, colon)) + "/virtiofsd");
    rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
  }
  for (const auto& place : places) {
    if (::access(place.c_str(), X_OK) == 0) {
      return place;
    }
  }
  return {};
}

std::string coffret::direct3dDriverFolder() {
  static const std::string folder = [] {
    const char* override = std::getenv("COFFRET_VIOGPU3D");
    const std::filesystem::path path = override && *override ? override : COFFRET_INSTALL_DATADIR "/viogpu3d";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path / "viogpu3d.inf", error)) {
      return std::string();
    }
    const auto help = runCommand({"qemu-system-x86_64", "-device", "virtio-vga-gl,help"});
    return help && help->find("neptune=") != std::string::npos ? path.string() : std::string();
  }();
  return folder;
}
