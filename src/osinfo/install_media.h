#pragma once

#include "vm/vm_config.h"

#include <cstdint>
#include <optional>
#include <string>

namespace coffret {
  // What libosinfo knows about the system on an installation medium.
  struct InstallMedia {
    // The libosinfo id, e.g. "http://fedoraproject.org/fedora/45".
    std::string id;
    std::string name;
    GuestKind kind = GuestKind::Other;
    // What the system's makers recommend; 0 where they say nothing.
    unsigned memoryMiB = 0;
    unsigned cpus = 0;
    std::uint64_t diskBytes = 0;
  };

  // Looks at an ISO image; nullopt for one libosinfo does not know. Reads the database on first
  // use, which takes a moment: for worker threads.
  std::optional<InstallMedia> detectInstallMedia(const std::string& isoPath);
} // namespace coffret
