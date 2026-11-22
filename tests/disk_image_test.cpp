#include "check.h"
#include "storage/disk_image.h"

#include <cstring>
#include <vector>

namespace {
  std::vector<std::byte> blank(std::size_t sectors) {
    std::vector<std::byte> disk(sectors * 512);
    disk[510] = std::byte{0x55};
    disk[511] = std::byte{0xaa};
    return disk;
  }

  void put(std::vector<std::byte>& disk, std::size_t offset, std::uint64_t value, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index) {
      disk[offset + index] = static_cast<std::byte>(value >> (8 * index) & 0xff);
    }
  }
} // namespace

int main() {
  using coffret::Firmware;
  // No partition table at all.
  CHECK(!coffret::firmwareOfDisk(std::vector<std::byte>(4096)).has_value());
  CHECK(!coffret::firmwareOfDisk(blank(2)).has_value());
  CHECK(!coffret::firmwareOfDisk({}).has_value());

  // MBR: an EFI system partition, or none.
  auto mbr = blank(2);
  mbr[446 + 4] = std::byte{0x83};
  CHECK(coffret::firmwareOfDisk(mbr) == Firmware::Bios);
  mbr[446 + 16 + 4] = std::byte{0xef};
  CHECK(coffret::firmwareOfDisk(mbr) == Firmware::Uefi);

  // GPT: the protective entry leads to the real table.
  auto gpt = blank(34);
  gpt[446 + 4] = std::byte{0xee};
  CHECK(!coffret::firmwareOfDisk(gpt).has_value());
  std::memcpy(gpt.data() + 512, "EFI PART", 8);
  put(gpt, 512 + 72, 2, 8);
  put(gpt, 512 + 80, 128, 4);
  put(gpt, 512 + 84, 128, 4);
  // A Linux filesystem partition only, as image builders that boot through UEFI make them.
  const unsigned char linuxData[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
  std::memcpy(gpt.data() + 1024, linuxData, 16);
  CHECK(coffret::firmwareOfDisk(gpt) == Firmware::Uefi);
  // The mark for legacy boot does not make it a BIOS disk; boot code in the first sector does.
  auto marked = gpt;
  put(marked, 1024 + 48, 4, 8);
  CHECK(coffret::firmwareOfDisk(marked) == Firmware::Uefi);
  auto coded = gpt;
  coded[0] = std::byte{0xeb};
  CHECK(coffret::firmwareOfDisk(coded) == Firmware::Bios);
  // A BIOS boot partition, for GRUB.
  auto grub = gpt;
  std::memcpy(grub.data() + 1024 + 128, "Hah!IdontNeedEFI", 16);
  CHECK(coffret::firmwareOfDisk(grub) == Firmware::Bios);
  // An EFI system partition decides it, whatever else there is.
  const unsigned char efiSystem[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  std::memcpy(coded.data() + 1024 + 128, efiSystem, 16);
  CHECK(coffret::firmwareOfDisk(coded) == Firmware::Uefi);

  const auto info = coffret::parseImageInfo(R"({
    "virtual-size": 21474836480, "filename": "disk.qcow2", "format": "qcow2", "actual-size": 1638400,
    "backing-filename": "base.qcow2", "full-backing-filename": "/vm/base.qcow2", "backing-filename-format": "qcow2",
    "snapshots": [
      {"id": "1", "name": "clean", "vm-state-size": 0, "date-sec": 1795000000, "date-nsec": 0, "vm-clock-sec": 0},
      {"id": "2", "name": "running", "vm-state-size": 734003200, "date-sec": 1795000100}
    ]})");
  CHECK(info.has_value());
  CHECK(info->format == "qcow2" && info->virtualSize == 21474836480ULL && info->actualSize == 1638400);
  CHECK(info->backingFile == "/vm/base.qcow2" && info->backingFormat == "qcow2");
  CHECK(info->snapshots.size() == 2);
  CHECK((info->snapshots[1] == coffret::ImageSnapshot{"2", "running", 1795000100, 734003200}));
  const auto raw = coffret::parseImageInfo(R"({"virtual-size": 1048576, "format": "raw", "actual-size": 4096})");
  CHECK(raw.has_value() && raw->backingFile.empty() && raw->snapshots.empty());
  CHECK(!coffret::parseImageInfo("qemu-img: Could not open").has_value());
  CHECK(!coffret::parseImageInfo("[]").has_value());

  CHECK((coffret::createImageCommand("/vm/a.qcow2", 1024) == std::vector<std::string>{"qemu-img", "create", "-q", "-f", "qcow2", "/vm/a.qcow2", "1024"}));
  CHECK(coffret::overlayImageCommand("/vm/b.qcow2", "/vm/a.img", "raw").size() == 10);

  CHECK(coffret::safeName("Fedora 45 (test)") == "Fedora-45--test-");
  CHECK(coffret::safeName("../etc") == "etc");
  CHECK(coffret::safeName("///") == "machine");
  return 0;
}
