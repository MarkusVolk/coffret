#include "vm/clone_plan.h"

#include "storage/disk_image.h"

#include <filesystem>
#include <format>
#include <set>

std::expected<coffret::ClonePlan, std::string> coffret::planClone(
    const VmConfig& source, const std::string& name, const std::string& folder, bool linked, bool sourceHasSnapshots
) {
  namespace fs = std::filesystem;
  if (name.empty() || name == source.name) {
    return std::unexpected(std::string("The clone needs a name of its own."));
  }
  if (linked && sourceHasSnapshots) {
    return std::unexpected(std::string("A machine with snapshots can only be cloned in full."));
  }
  ClonePlan plan;
  plan.clone = source;
  plan.clone.name = name;
  plan.clone.uuid.clear();
  for (auto& nic : plan.clone.nics) {
    nic.mac.clear();
  }
  // A device can be given to one machine only.
  plan.clone.usbDevices.clear();

  std::set<std::string> names;
  for (auto& disk : plan.clone.disks) {
    if (disk.cdrom || disk.readOnly || disk.path.empty()) {
      continue;
    }
    const fs::path original(disk.path);
    // Two disks of the same file name from different folders must not end up as one.
    std::string file = original.stem().string() + ".qcow2";
    for (unsigned number = 2; !names.insert(file).second; ++number) {
      file = std::format("{}-{}.qcow2", original.stem().string(), number);
    }
    const std::string target = (fs::path(folder) / file).string();
    if (linked) {
      if (disk.format != "qcow2") {
        return std::unexpected(std::format("{} is not a qcow2 image; the machine can only be cloned in full.", original.filename().string()));
      }
      const std::string base = (original.parent_path() / (original.stem().string() + ".base.qcow2")).string();
      plan.renames.emplace_back(disk.path, base);
      plan.readOnly.push_back(base);
      plan.commands.push_back(overlayImageCommand(disk.path, base, "qcow2"));
      plan.commands.push_back(overlayImageCommand(target, base, "qcow2"));
    } else {
      plan.commands.push_back(convertImageCommand(disk.path, target));
    }
    disk.path = target;
    disk.format = "qcow2";
  }
  if (!source.nvramPath.empty()) {
    plan.clone.nvramPath = (fs::path(folder) / fs::path(source.nvramPath).filename()).string();
    plan.copies.emplace_back(source.nvramPath, plan.clone.nvramPath);
  }
  return plan;
}
