#include "check.h"
#include "vm/host_network.h"

#include <filesystem>
#include <fstream>
#include <map>

int main() {
  const auto root = std::filesystem::temp_directory_path() / "coffret-net-test";
  std::filesystem::remove_all(root);
  for (const char* name : {"lo", "eth0", "wlan0"}) {
    std::filesystem::create_directories(root / name);
  }
  std::filesystem::create_directories(root / "br0/bridge");
  std::filesystem::create_directories(root / "virbr1/bridge");
  CHECK((coffret::hostBridges(root.string()) == std::vector<std::string>{"br0", "virbr1"}));
  CHECK(coffret::hostBridges((root / "none").string()).empty());
  std::filesystem::remove_all(root);

  const std::map<std::string, std::string> files = {{"/etc/qemu/user.conf", "allow br1\n"}, {"/etc/qemu/no.conf", "deny br0\n"}};
  const auto read = [&files](const std::string& path) -> std::optional<std::string> {
    const auto it = files.find(path);
    return it == files.end() ? std::nullopt : std::optional<std::string>(it->second);
  };
  CHECK(coffret::bridgeAllowed("allow br0\n", "br0", read));
  CHECK(!coffret::bridgeAllowed("allow br0\n", "br1", read));
  CHECK(coffret::bridgeAllowed("# all of them\nallow all\n", "br7", read));
  CHECK(!coffret::bridgeAllowed("allow all\ndeny br0\n", "br0", read));
  CHECK(coffret::bridgeAllowed("include /etc/qemu/user.conf\n", "br1", read));
  CHECK(!coffret::bridgeAllowed("allow all\ninclude /etc/qemu/no.conf\n", "br0", read));
  CHECK(!coffret::bridgeAllowed("include /etc/qemu/missing.conf\n", "br0", read));
  CHECK(!coffret::bridgeAllowed("", "br0", read));
  return 0;
}
