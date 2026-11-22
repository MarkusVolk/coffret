#include "storage/disk_image.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::size_t kSector = 512;
  // C12A7328-F81F-11D2-BA4B-00A0C93EC93B as it lies on disk.
  constexpr std::array<unsigned char, 16> kEfiSystem{0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};

  unsigned char byteAt(std::span<const std::byte> data, std::size_t offset) {
    return offset < data.size() ? static_cast<unsigned char>(data[offset]) : 0;
  }

  std::uint64_t little(std::span<const std::byte> data, std::size_t offset, std::size_t bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = bytes; index-- > 0;) {
      value = value << 8 | byteAt(data, offset + index);
    }
    return value;
  }
} // namespace

std::expected<coffret::ImageInfo, std::string> coffret::parseImageInfo(const std::string& json) {
  const auto root = nlohmann::json::parse(json, nullptr, false);
  if (!root.is_object() || !root.contains("format")) {
    return std::unexpected(std::string("qemu-img gave no description of the image."));
  }
  ImageInfo info;
  info.format = root.value("format", "");
  info.virtualSize = root.value("virtual-size", std::uint64_t{0});
  info.actualSize = root.value("actual-size", std::uint64_t{0});
  info.backingFile = root.value("full-backing-filename", root.value("backing-filename", ""));
  info.backingFormat = root.value("backing-filename-format", "");
  if (const auto snapshots = root.find("snapshots"); snapshots != root.end() && snapshots->is_array()) {
    for (const auto& entry : *snapshots) {
      if (entry.is_object()) {
        info.snapshots.push_back({
            entry.value("id", ""),
            entry.value("name", ""),
            entry.value("date-sec", std::int64_t{0}),
            entry.value("vm-state-size", std::uint64_t{0}),
        });
      }
    }
  }
  return info;
}

std::vector<std::string> coffret::imageInfoCommand(const std::string& path) {
  // -U: also an image a running machine has open.
  return {"qemu-img", "info", "--output=json", "-U", path};
}

std::vector<std::string> coffret::createImageCommand(const std::string& path, std::uint64_t bytes) {
  return {"qemu-img", "create", "-q", "-f", "qcow2", path, std::to_string(bytes)};
}

std::vector<std::string>
coffret::overlayImageCommand(const std::string& path, const std::string& backing, const std::string& backingFormat) {
  return {"qemu-img", "create", "-q", "-f", "qcow2", "-b", backing, "-F", backingFormat, path};
}

std::vector<std::string> coffret::convertImageCommand(const std::string& source, const std::string& target) {
  return {"qemu-img", "convert", "-O", "qcow2", source, target};
}

std::optional<coffret::Firmware> coffret::firmwareOfDisk(std::span<const std::byte> start) {
  if (start.size() < kSector || byteAt(start, 510) != 0x55 || byteAt(start, 511) != 0xaa) {
    return std::nullopt;
  }
  bool protective = false, efi = false, any = false;
  for (std::size_t entry = 0; entry < 4; ++entry) {
    const unsigned char type = byteAt(start, 446 + entry * 16 + 4);
    any = any || type != 0;
    protective = protective || type == 0xee;
    efi = efi || type == 0xef;
  }
  if (!protective) {
    return any ? std::optional(efi ? Firmware::Uefi : Firmware::Bios) : std::nullopt;
  }
  if (start.size() < 2 * kSector || std::memcmp(start.data() + kSector, "EFI PART", 8) != 0) {
    return std::nullopt;
  }
  const std::uint64_t first = little(start, kSector + 72, 8) * kSector;
  const std::uint64_t count = std::min<std::uint64_t>(little(start, kSector + 80, 4), 256);
  const std::uint64_t size = little(start, kSector + 84, 4);
  if (size < 16) {
    return std::nullopt;
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    const std::uint64_t offset = first + index * size;
    if (offset + 16 > start.size()) {
      break;
    }
    if (std::memcmp(start.data() + offset, kEfiSystem.data(), kEfiSystem.size()) == 0) {
      return Firmware::Uefi;
    }
  }
  return Firmware::Bios;
}

std::string coffret::safeName(const std::string& name) {
  std::string safe;
  for (const unsigned char c : name) {
    safe += std::isalnum(c) || c == '-' || c == '_' || c == '.' ? static_cast<char>(c) : '-';
  }
  while (!safe.empty() && (safe.front() == '.' || safe.front() == '-')) {
    safe.erase(safe.begin());
  }
  return safe.empty() ? "machine" : safe;
}
