#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffret {
  // The network bridges of the host, e.g. "br0", sorted; under /sys/class/net on a real system.
  std::vector<std::string> hostBridges(const std::string& sysfs = "/sys/class/net");

  // Whether QEMU's bridge.conf lets a user put a machine on `bridge`: "allow <name>" or "allow all",
  // where no "deny" matches; "include <file>" reads another file through `read`, which gives
  // nullopt for a file that is not there.
  bool bridgeAllowed(
      std::string_view config, std::string_view bridge, const std::function<std::optional<std::string>(const std::string&)>& read
  );

  // Why a machine of this user cannot be put on `bridge`, in words for the user; empty when it can.
  // QEMU's bridge helper sets the card up, which takes root.
  std::string bridgeProblem(const std::string& bridge);
} // namespace coffret
