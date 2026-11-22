#pragma once

#include "vm/vm_config.h"

#include <cstdint>
#include <expected>
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
    // libosinfo can answer its installer's questions, so it installs without anybody at it.
    bool unattended = false;
  };

  // What an unattended installation sets up in the guest.
  struct GuestSetup {
    std::string user;
    std::string realName;
    std::string password;
    // An X11 layout ("de"), a language ("en_US") and a time zone ("Europe/Berlin").
    std::string keyboard;
    std::string language;
    std::string timezone;
    std::string hostname;
  };

  // The user, keyboard, language and time zone of this computer; no password.
  GuestSetup hostGuestSetup();

  // Writes the installer's answers for the medium and its kernel and initrd, with the answers in
  // it, into `folder`, and says how to boot them. Runs xorriso: for worker threads.
  std::expected<DirectBoot, std::string>
  prepareUnattended(const std::string& isoPath, const GuestSetup& setup, const std::string& folder);

  // Looks at an ISO image; nullopt for one libosinfo does not know. Reads the database on first
  // use, which takes a moment: for worker threads.
  std::optional<InstallMedia> detectInstallMedia(const std::string& isoPath);
} // namespace coffret
