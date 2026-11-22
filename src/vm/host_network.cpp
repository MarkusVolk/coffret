#include "vm/host_network.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
  std::optional<std::string> readFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
      return std::nullopt;
    }
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
  }

  // What the config files say about the bridge: +1 allowed, -1 denied, 0 nothing.
  int verdict(
      std::string_view config, std::string_view bridge, const std::function<std::optional<std::string>(const std::string&)>& read,
      int depth
  ) {
    bool allowed = false;
    std::istringstream lines{std::string(config)};
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream words(line);
      std::string what, name;
      words >> what >> name;
      if (what.empty() || what.starts_with('#')) {
        continue;
      }
      const bool matches = name == "all" || name == bridge;
      if (what == "deny" && matches) {
        return -1;
      }
      if (what == "allow" && matches) {
        allowed = true;
      } else if (what == "include" && depth < 8) {
        if (const auto included = read(name)) {
          const int inner = verdict(*included, bridge, read, depth + 1);
          if (inner < 0) {
            return -1;
          }
          allowed = allowed || inner > 0;
        }
      }
    }
    return allowed ? 1 : 0;
  }

  bool noNewPrivileges() {
    const auto status = readFile("/proc/self/status");
    return status && status->find("NoNewPrivs:\t1") != std::string::npos;
  }
} // namespace

std::vector<std::string> coffret::hostBridges(const std::string& sysfs) {
  std::vector<std::string> bridges;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(sysfs, error)) {
    if (std::filesystem::is_directory(entry.path() / "bridge", error)) {
      bridges.push_back(entry.path().filename().string());
    }
  }
  std::ranges::sort(bridges);
  return bridges;
}

bool coffret::bridgeAllowed(
    std::string_view config, std::string_view bridge, const std::function<std::optional<std::string>(const std::string&)>& read
) {
  return verdict(config, bridge, read, 0) > 0;
}

std::string coffret::bridgeProblem(const std::string& bridge) {
  std::string helper;
  for (const char* candidate : {"/usr/libexec/qemu-bridge-helper", "/usr/lib/qemu/qemu-bridge-helper"}) {
    if (::access(candidate, X_OK) == 0) {
      helper = candidate;
      break;
    }
  }
  if (helper.empty()) {
    return "QEMU's qemu-bridge-helper is not installed; it sets up the machine's card on the bridge.";
  }
  struct stat info {};
  const bool setuid = ::stat(helper.c_str(), &info) == 0 && info.st_uid == 0 && (info.st_mode & S_ISUID) != 0;
  const bool capable = ::getxattr(helper.c_str(), "security.capability", nullptr, 0) > 0;
  if (!setuid && !capable) {
    return std::format("{} must be setuid root to set up the machine's card on the bridge.", helper);
  }
  if (noNewPrivileges()) {
    return "coffret runs where programs cannot gain privileges (no_new_privs), so qemu-bridge-helper cannot work.";
  }
  const std::string config = "/etc/qemu/bridge.conf";
  const auto text = readFile(config);
  if (!text || !bridgeAllowed(*text, bridge, readFile)) {
    return std::format("{} must allow the bridge, with a line \"allow {}\".", config, bridge);
  }
  return {};
}
