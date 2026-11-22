#pragma once

#include "vm/clone_plan.h"
#include "vm/vm_config.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace coffret {
  // A machine made from a disk image that is there already: a cloud image, a Yocto .wic, a disk of
  // another hypervisor.
  struct ImageImport {
    std::string name;
    std::string image;
    // A copy of its own, or only the changes with the image kept as it is (it must then stay where
    // it is, unchanged).
    bool copy = true;
    GuestKind kind = GuestKind::Linux;
    unsigned memoryMiB = 4096;
    unsigned cpus = 2;
    // An ISO to put into the drive, e.g. a cloud-init seed; may be empty.
    std::string cdrom;
  };

  // A new machine with an empty disk and an installation medium in its drive.
  struct NewMachine {
    std::string name;
    std::string iso;
    GuestKind kind = GuestKind::Linux;
    // The libosinfo id of the system on the medium; may be empty.
    std::string osId;
    unsigned memoryMiB = 4096;
    unsigned cpus = 2;
    std::uint64_t diskBytes = 40ULL << 30;
  };

  // What a file is that a machine could be made from.
  enum class SourceKind : std::uint8_t {
    // An ISO 9660 image: something to install from, into a new disk.
    InstallMedium,
    // A disk with a system on it: a cloud image, a Yocto .wic, a disk of another hypervisor.
    DiskImage,
    Unknown,
  };
  // Looks into the file; runs qemu-img: for worker threads.
  SourceKind classifySource(const std::string& path);

  // Where a machine's own files live: <data>/coffret/machines/<name>.
  std::string machineFolder(const std::string& name);

  // Sets up the machine's folder and disk and says how to define it. Blocks while the image is
  // copied: for worker threads.
  std::expected<VmConfig, std::string> prepareImport(const ImageImport& request);
  // Carries out a clone plan for a machine that is off, into a folder that is not there yet, and
  // undoes what it did when a step fails. Blocks while disks are copied.
  std::expected<VmConfig, std::string> runClone(const ClonePlan& plan, const std::string& folder);
  // Takes a clone back that was carried out in full: for when the new machine cannot be defined.
  void undoClone(const ClonePlan& plan, const std::string& folder);

  // The folder under <data>/coffret/machines that holds the machine's first disk; empty for a
  // machine whose disks live elsewhere.
  std::string ownFolder(const VmConfig& config);
  // The names of the machines whose disks build on an image in `folder`: linked clones of the
  // machine that lives there. Runs qemu-img for every disk: for worker threads.
  std::vector<std::string> machinesBuiltOn(const std::string& folder, const std::vector<VmConfig>& others);

  // The same for a machine that is installed from a medium. Quick, but runs qemu-img.
  std::expected<VmConfig, std::string> prepareNew(const NewMachine& request);
} // namespace coffret
