#include "osinfo/answer_file.h"

#include <format>

namespace {
  void pad(std::string& archive) {
    archive.append((4 - archive.size() % 4) % 4, '\0');
  }

  void entry(std::string& archive, std::string_view name, unsigned mode, std::string_view data) {
    archive += std::format(
        "070701{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}", 0, mode, 0, 0,
        1, 0, data.size(), 0, 0, 0, 0, name.size() + 1, 0
    );
    archive += name;
    archive += '\0';
    pad(archive);
    archive += data;
    pad(archive);
  }
} // namespace

std::string coffret::newcArchive(std::string_view name, std::string_view data) {
  std::string archive;
  entry(archive, name, 0100644, data);
  entry(archive, "TRAILER!!!", 0, "");
  return archive;
}

std::string coffret::guestHostname(std::string_view machineName) {
  std::string name;
  for (const char c : machineName) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      name += c;
    } else if (c >= 'A' && c <= 'Z') {
      name += static_cast<char>(c - 'A' + 'a');
    } else if (!name.empty() && name.back() != '-') {
      name += '-';
    }
  }
  while (!name.empty() && name.back() == '-') {
    name.pop_back();
  }
  if (name.size() > 63) {
    name.resize(63);
  }
  return name.empty() ? "guest" : name;
}

std::string coffret::firstKeyboardLayout(std::string_view x11Layouts, std::string_view consoleKeymap) {
  const std::string_view first = x11Layouts.substr(0, x11Layouts.find(','));
  if (!first.empty()) {
    return std::string(first);
  }
  return consoleKeymap.empty() ? "us" : std::string(consoleKeymap);
}

std::string coffret::localeLanguage(std::string_view locale) {
  const std::string_view language = locale.substr(0, locale.find_first_of(".@"));
  if (language.empty() || language == "C" || language == "POSIX") {
    return "en_US";
  }
  return std::string(language);
}
