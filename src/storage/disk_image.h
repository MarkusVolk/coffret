#pragma once

#include "vm/vm_config.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace coffret {
  struct ImageSnapshot {
    std::string id;
    std::string name;
    // Seconds since the epoch.
    std::int64_t date = 0;
    // The size of the machine's memory in the snapshot; 0 for one taken while it was off.
    std::uint64_t stateSize = 0;
    bool operator==(const ImageSnapshot&) const = default;
  };

  // What `qemu-img info` says about a disk image.
  struct ImageInfo {
    std::string format;
    std::uint64_t virtualSize = 0;
    // What it takes on the host's disk.
    std::uint64_t actualSize = 0;
    // The image this one only holds the changes to; empty for an image that stands alone.
    std::string backingFile;
    std::string backingFormat;
    std::vector<ImageSnapshot> snapshots;
  };

  std::expected<ImageInfo, std::string> parseImageInfo(const std::string& json);

  // The command lines of qemu-img.
  std::vector<std::string> imageInfoCommand(const std::string& path);
  std::vector<std::string> createImageCommand(const std::string& path, std::uint64_t bytes);
  // An image that holds only the changes to `backing`, which must not change from then on.
  std::vector<std::string> overlayImageCommand(const std::string& path, const std::string& backing, const std::string& backingFormat);
  // A full copy as qcow2, whatever the source is.
  std::vector<std::string> convertImageCommand(const std::string& source, const std::string& target);

  // How a raw disk image wants to be booted, read from its partition table. MBR: UEFI when it has
  // an EFI system partition. GPT: UEFI unless the disk says BIOS by a BIOS boot partition or
  // by boot code in its first sector. `start` is the image's first sectors, 17 KiB are
  // enough; nullopt when there is no partition table in it.
  std::optional<Firmware> firmwareOfDisk(std::span<const std::byte> start);

  // A name that can be a file name and a libvirt domain name: letters, digits, '-', '_', '.'.
  std::string safeName(const std::string& name);
} // namespace coffret
