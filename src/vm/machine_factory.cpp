#include "vm/machine_factory.h"

#include "fs/xdg_dirs.h"
#include "osinfo/answer_file.h"
#include "storage/disk_image.h"
#include "storage/process.h"
#include "vm/firmware_store.h"
#include "vm/host_info.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
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

std::expected<coffret::VmConfig, std::string> coffret::prepareNew(const NewMachine& request) {
  namespace fs = std::filesystem;
  std::error_code error;
  if (request.name.empty()) {
    return std::unexpected(std::string("The machine needs a name."));
  }
  const fs::path iso = fs::absolute(request.iso, error);
  if (!fs::is_regular_file(iso, error)) {
    return std::unexpected(std::format("{} is not an installation medium.", request.iso));
  }
  const fs::path folder = machineFolder(request.name);
  if (fs::exists(folder, error)) {
    return std::unexpected(std::format("There is a machine folder named {} already.", folder.filename().string()));
  }
  // Windows 11 installs only on a firmware that can do secure boot, with a TPM.
  const bool windows = request.kind == GuestKind::Windows;
  const auto firmware = prepareFirmware(windows);
  if (!firmware) {
    return std::unexpected(firmware.error());
  }
  if (!fs::create_directories(folder, error)) {
    return std::unexpected(std::format("{} cannot be created: {}", folder.string(), error.message()));
  }
  const fs::path disk = folder / "disk.qcow2";
  if (const auto made = runCommand(createImageCommand(disk.string(), request.diskBytes)); !made) {
    fs::remove_all(folder, error);
    return std::unexpected(made.error());
  }
  VmConfig config;
  config.name = request.name;
  config.kind = request.kind;
  config.osId = request.osId;
  config.memoryMiB = request.memoryMiB;
  config.cpus = request.cpus;
  config.firmware = windows ? Firmware::UefiSecureBoot : Firmware::Uefi;
  config.firmwareCode = firmware->code;
  config.nvramTemplate = firmware->variables;
  config.nvramPath = (folder / "vars.qcow2").string();
  // An empty disk does not boot: the firmware goes on to the medium, and to the disk once the system is on it.
  config.disks.push_back({.path = disk.string(), .bootOrder = 1});
  config.disks.push_back({.path = iso.string(), .format = "raw", .bus = DiskBus::Sata, .cdrom = true, .readOnly = true, .bootOrder = 2});
  config.nics.emplace_back();
  config.renderNode = defaultRenderNode();
  // Windows has no driver for it out of the box.
  config.accel3d = !config.renderNode.empty() && request.kind != GuestKind::Windows;
  config.tpm = request.kind == GuestKind::Windows;
  if (request.unattended) {
    GuestSetup setup = *request.unattended;
    if (setup.hostname.empty()) {
      setup.hostname = guestHostname(request.name);
    }
    const auto boot = prepareUnattended(iso.string(), setup, folder.string());
    if (!boot) {
      fs::remove_all(folder, error);
      return std::unexpected(boot.error());
    }
    config.install = *boot;
  }
  return config;
}

std::expected<coffret::VmConfig, std::string> coffret::runClone(const ClonePlan& plan, const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code error;
  if (fs::exists(folder, error)) {
    return std::unexpected(std::format("There is a machine folder named {} already.", fs::path(folder).filename().string()));
  }
  if (!fs::create_directories(folder, error)) {
    return std::unexpected(std::format("{} cannot be created: {}", folder, error.message()));
  }
  std::size_t renamed = 0;
  // Back to how it was: the clone's folder goes, and the source's disks get their names back.
  const auto undo = [&](std::string why) -> std::unexpected<std::string> {
    ClonePlan done = plan;
    done.renames.resize(renamed);
    undoClone(done, folder);
    return std::unexpected(std::move(why));
  };
  for (const auto& [disk, base] : plan.renames) {
    if (fs::exists(base, error)) {
      return undo(std::format("{} is in the way.", base));
    }
    fs::rename(disk, base, error);
    if (error) {
      return undo(std::format("{} cannot be renamed: {}", disk, error.message()));
    }
    ++renamed;
  }
  for (const auto& command : plan.commands) {
    if (const auto done = runCommand(command); !done) {
      return undo(done.error());
    }
  }
  for (const auto& [from, to] : plan.copies) {
    if (!fs::exists(from, error)) {
      continue;
    }
    if (const auto done = runCommand(convertImageCommand(from, to)); !done) {
      return undo(done.error());
    }
  }
  for (const auto& file : plan.readOnly) {
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, error);
  }
  return plan.clone;
}

void coffret::undoClone(const ClonePlan& plan, const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code ignored;
  fs::remove_all(folder, ignored);
  for (std::size_t index = plan.renames.size(); index-- > 0;) {
    const auto& [disk, base] = plan.renames[index];
    if (!fs::exists(base, ignored)) {
      continue;
    }
    fs::permissions(base, fs::perms::owner_write, fs::perm_options::add, ignored);
    // The overlay made in the disk's place holds nothing yet.
    fs::remove(disk, ignored);
    fs::rename(base, disk, ignored);
  }
}

std::string coffret::ownFolder(const VmConfig& config) {
  namespace fs = std::filesystem;
  const fs::path root = dataHome() / "coffret" / "machines";
  for (const auto& disk : config.disks) {
    if (disk.cdrom || disk.path.empty()) {
      continue;
    }
    const fs::path folder = fs::path(disk.path).parent_path();
    return folder.parent_path() == root ? folder.string() : std::string();
  }
  return {};
}

std::vector<std::string> coffret::machinesBuiltOn(const std::string& folder, const std::vector<VmConfig>& others) {
  std::vector<std::string> names;
  const std::string prefix = folder + "/";
  for (const auto& other : others) {
    for (const auto& disk : other.disks) {
      if (disk.cdrom || disk.path.empty() || disk.path.starts_with(prefix)) {
        continue;
      }
      const auto info = runCommand(imageInfoCommand(disk.path)).and_then(parseImageInfo);
      if (info && info->backingFile.starts_with(prefix)) {
        names.push_back(other.name);
        break;
      }
    }
  }
  return names;
}

coffret::SourceKind coffret::classifySource(const std::string& path) {
  // "CD001" at the start of the 17th sector of 2048 bytes; installers that also boot from a USB
  // stick carry a partition table too and are media all the same.
  {
    std::ifstream file(path, std::ios::binary);
    char magic[5] = {};
    file.seekg(32769);
    file.read(magic, sizeof(magic));
    if (file.gcount() == static_cast<std::streamsize>(sizeof(magic)) && std::string_view(magic, sizeof(magic)) == "CD001") {
      return SourceKind::InstallMedium;
    }
  }
  const auto described = runCommand(imageInfoCommand(path)).and_then(parseImageInfo);
  if (!described) {
    return SourceKind::Unknown;
  }
  if (described->format != "raw") {
    return SourceKind::DiskImage;
  }
  std::ifstream file(path, std::ios::binary);
  std::vector<char> bytes(34 * 512);
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(file.gcount()));
  return firmwareOfDisk(std::as_bytes(std::span(bytes))) ? SourceKind::DiskImage : SourceKind::Unknown;
}
