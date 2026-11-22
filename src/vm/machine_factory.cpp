#include "vm/machine_factory.h"

#include "fs/xdg_dirs.h"
#include "storage/disk_image.h"
#include "storage/process.h"
#include "vm/firmware_store.h"
#include "vm/host_info.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace {
  // The partition table of an image of any format, through qemu-img.
  std::optional<coffret::Firmware> firmwareOfImage(const std::string& image, const std::filesystem::path& scratch) {
    const auto head = scratch / ".head";
    const auto copied = coffret::runCommand(
        {"qemu-img", "dd", "-O", "raw", "bs=512", "count=34", "if=" + image, "of=" + head.string()}
    );
    std::optional<coffret::Firmware> firmware;
    if (copied) {
      std::ifstream file(head, std::ios::binary);
      std::vector<char> bytes(34 * 512);
      file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(file.gcount()));
      firmware = coffret::firmwareOfDisk(std::as_bytes(std::span(bytes)));
    }
    std::error_code error;
    std::filesystem::remove(head, error);
    return firmware;
  }
} // namespace

std::string coffret::machineFolder(const std::string& name) {
  return (dataHome() / "coffret" / "machines" / safeName(name)).string();
}

std::expected<coffret::VmConfig, std::string> coffret::prepareImport(const ImageImport& request) {
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path image = fs::absolute(request.image, error);
  if (request.name.empty()) {
    return std::unexpected(std::string("The machine needs a name."));
  }
  if (!fs::is_regular_file(image, error)) {
    return std::unexpected(std::format("{} is not a disk image.", request.image));
  }
  const fs::path folder = machineFolder(request.name);
  if (fs::exists(folder, error)) {
    return std::unexpected(std::format("There is a machine folder named {} already.", folder.filename().string()));
  }
  const auto described = runCommand(imageInfoCommand(image.string())).and_then(parseImageInfo);
  if (!described) {
    return std::unexpected(described.error());
  }
  if (!fs::create_directories(folder, error)) {
    return std::unexpected(std::format("{} cannot be created: {}", folder.string(), error.message()));
  }
  const fs::path disk = folder / "disk.qcow2";
  const auto made = request.copy ? runCommand(convertImageCommand(image.string(), disk.string()))
                                 : runCommand(overlayImageCommand(disk.string(), image.string(), described->format));
  if (!made) {
    fs::remove_all(folder, error);
    return std::unexpected(made.error());
  }

  VmConfig config;
  config.name = request.name;
  config.kind = request.kind;
  config.memoryMiB = request.memoryMiB;
  config.cpus = request.cpus;
  config.firmware = firmwareOfImage(disk.string(), folder).value_or(Firmware::Uefi);
  if (config.firmware != Firmware::Bios) {
    const auto firmware = prepareFirmware(false);
    if (!firmware) {
      fs::remove_all(folder, error);
      return std::unexpected(firmware.error());
    }
    config.firmwareCode = firmware->code;
    config.nvramTemplate = firmware->variables;
    config.nvramPath = (folder / "vars.qcow2").string();
  }
  config.disks.push_back({.path = disk.string(), .bootOrder = 1});
  if (!request.cdrom.empty()) {
    config.disks.push_back({
        .path = fs::absolute(request.cdrom, error).string(),
        .format = "raw",
        .bus = DiskBus::Sata,
        .cdrom = true,
        .readOnly = true,
    });
  }
  config.nics.emplace_back();
  config.renderNode = defaultRenderNode();
  config.accel3d = !config.renderNode.empty();
  return config;
}
