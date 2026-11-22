#pragma once

#include <expected>
#include <string>

namespace coffret {
  // A UEFI firmware as two qcow2 files: its code, and the variables a new machine starts with.
  struct FirmwareFiles {
    std::string code;
    std::string variables;
  };

  // The firmware for x86_64 machines that QEMU's firmware descriptions name, with or without
  // secure boot. libvirt wants the variables as qcow2 for snapshots and does not convert a raw
  // template, so a firmware that comes raw is converted once into <data>/coffret/firmware.
  // Blocks: for worker threads.
  std::expected<FirmwareFiles, std::string> prepareFirmware(bool secureBoot);
} // namespace coffret
