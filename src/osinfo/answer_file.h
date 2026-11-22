#pragma once

#include <string>
#include <string_view>

namespace coffret {
  // A newc cpio archive with one file at its root, as an installer finds its answer file in the
  // initrd: appended to a compressed initrd, the kernel unpacks it on top.
  std::string newcArchive(std::string_view name, std::string_view data);

  // A host name for a guest made from the machine's name: "My Debian 13" -> "my-debian-13".
  std::string guestHostname(std::string_view machineName);

  // The first of the host's X11 layouts, "de" of "de,us"; the console keymap when there is none,
  // "us" when there is neither.
  std::string firstKeyboardLayout(std::string_view x11Layouts, std::string_view consoleKeymap);

  // The language of a locale as installers take it, "de_DE" of "de_DE.UTF-8"; "en_US" for C and
  // POSIX.
  std::string localeLanguage(std::string_view locale);
} // namespace coffret
