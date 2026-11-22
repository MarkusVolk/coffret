#pragma once

#include <expected>
#include <string>

namespace coffret {
  // Asks a QEMU that runs with `-display dbus,p2p=yes` for a connection to its display bus, over
  // its QMP socket. Returns the connected socket; the caller owns it. For a QEMU started by hand;
  // libvirt hands out the same kind of socket with virDomainOpenGraphicsFD. Blocks for a moment.
  std::expected<int, std::string> openDisplayBus(const std::string& qmpSocketPath);
} // namespace coffret
