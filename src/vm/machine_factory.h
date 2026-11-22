#pragma once

#include "vm/vm_config.h"

#include <cstdint>
#include <expected>
#include <string>

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

  // Where a machine's own files live: <data>/coffret/machines/<name>.
  std::string machineFolder(const std::string& name);

  // Sets up the machine's folder and disk and says how to define it. Blocks while the image is
  // copied: for worker threads.
  std::expected<VmConfig, std::string> prepareImport(const ImageImport& request);
  // The same for a machine that is installed from a medium. Quick, but runs qemu-img.
  std::expected<VmConfig, std::string> prepareNew(const NewMachine& request);
} // namespace coffret
