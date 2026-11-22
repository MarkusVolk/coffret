#include "app/application.h"
#include "coffret_build_info.h"
#include "core/application_identity.h"

#include <print>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
  setApplicationIdentity({.name = "coffret", .envPrefix = "COFFRET"});
  coffret::Launch launch;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--version") {
      std::println("coffret {}", COFFRET_VERSION);
      return 0;
    }
    if (argument == "--help") {
      std::println(
          "Usage: coffret --qmp SOCKET\n"
          "       coffret --action NAME [PARAMETER]\n\n"
          "--qmp shows the console of a QEMU started by hand with `-display dbus,p2p=yes` and a QMP\n"
          "socket at SOCKET.\n"
          "--action runs a named action in the running coffret; the README lists them."
      );
      return 0;
    }
    if (argument == "--qmp" && index + 1 < argc) {
      launch.qmpSocket = argv[++index];
      continue;
    }
    if (argument == "--action" && index + 1 < argc) {
      launch.action = argv[++index];
      if (index + 1 < argc && argv[index + 1][0] != '-') {
        launch.parameter = argv[++index];
      }
      continue;
    }
    std::println(stderr, "coffret: unknown argument {}", argument);
    return 2;
  }
  return coffret::runApplication(launch);
}
