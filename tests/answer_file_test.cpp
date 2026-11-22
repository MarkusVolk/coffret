#include "check.h"
#include "osinfo/answer_file.h"

int main() {
  const std::string archive = coffret::newcArchive("preseed.cfg", "abc");
  CHECK(archive.size() % 4 == 0);
  CHECK(archive.starts_with("070701"));
  // Header of 110 bytes, name with its NUL padded to 124, three bytes of data padded to 128.
  CHECK(archive.substr(110, 12) == std::string("preseed.cfg\0", 12));
  CHECK(archive.substr(54, 8) == "00000003");
  CHECK(archive.substr(124, 3) == "abc");
  CHECK(archive.substr(128, 6) == "070701");
  CHECK(archive.find("TRAILER!!!") == 128 + 110);

  CHECK(coffret::guestHostname("My Debian 13") == "my-debian-13");
  CHECK(coffret::guestHostname("  Fedora (linked)  ") == "fedora-linked");
  CHECK(coffret::guestHostname("äöü") == "guest");

  CHECK(coffret::firstKeyboardLayout("de,us", "de-latin1") == "de");
  CHECK(coffret::firstKeyboardLayout("", "fr") == "fr");
  CHECK(coffret::firstKeyboardLayout("", "") == "us");

  CHECK(coffret::localeLanguage("de_DE.UTF-8") == "de_DE");
  CHECK(coffret::localeLanguage("C.UTF-8") == "en_US");
  CHECK(coffret::localeLanguage("") == "en_US");
  CHECK(coffret::localeLanguage("sr_RS@latin") == "sr_RS");
  return 0;
}
