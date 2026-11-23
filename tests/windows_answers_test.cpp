#include "check.h"
#include "osinfo/windows_answers.h"

#include <pugixml.hpp>

int main() {
  CHECK(coffret::windowsKeyboard("de") == "0407:00000407");
  CHECK(coffret::windowsKeyboard("xx") == "0409:00000409");
  CHECK(coffret::windowsLocale("en_US") == "en-US");
  CHECK(coffret::windowsLocale("de_DE.UTF-8") == "de-DE");
  CHECK(coffret::windowsLocale("C") == "en-US");
  CHECK(coffret::windowsTimeZone("Europe/Berlin") == "W. Europe Standard Time");
  CHECK(coffret::windowsTimeZone("Mars/Olympus") == "UTC");
  CHECK(coffret::windowsComputerName("Windows 11") == "WINDOWS-11");
  CHECK(coffret::windowsComputerName("a very long machine name") == "A-VERY-LONG-MAC");
  CHECK(coffret::windowsComputerName("2024") == "WINDOWS");
  CHECK(coffret::windowsSetupKey("Windows 11 Pro", true) == "W269N-WFGWX-YVC9B-4J6C9-4T9TF");
  CHECK(coffret::windowsSetupKey("Windows 11 Pro", false) == "VK7JG-NPHTM-C97JM-9MPGT-3V66T");
  CHECK(coffret::windowsSetupKey("Windows 11 Home", true).empty());

  coffret::WindowsAnswers answers;
  answers.user = "flk";
  answers.realName = "Markus Volk";
  answers.password = "a<b&\"c";
  answers.computerName = "WINDOWS-11";
  answers.keyboard = "de";
  answers.language = "en_US";
  answers.timezone = "Europe/Berlin";
  answers.imageLanguage = "en-GB";
  answers.productKey = coffret::windowsSetupKey(answers.edition, true);
  const std::string xml = coffret::windowsAnswerFile(answers);
  pugi::xml_document document;
  CHECK(document.load_string(xml.c_str()));
  const auto unattend = document.child("unattend");
  CHECK(unattend);
  int passes = 0;
  for (auto settings : unattend.children("settings")) {
    ++passes;
    for (auto component : settings.children("component")) {
      if (std::string_view(component.attribute("name").value()).starts_with("Microsoft-Windows-International-Core")) {
        CHECK(std::string_view(component.child_value("InputLocale")) == "0407:00000407");
        CHECK(std::string_view(component.child_value("UILanguage")) == "en-GB");
        CHECK(std::string_view(component.child_value("UserLocale")) == "en-US");
      }
    }
  }
  CHECK(passes == 3);
  const auto shell = unattend.find_node([](pugi::xml_node node) {
    return std::string_view(node.name()) == "LocalAccount";
  });
  CHECK(std::string_view(shell.child_value("Name")) == "flk");
  CHECK(std::string_view(shell.child("Password").child_value("Value")) == "a<b&\"c");
  CHECK(xml.find("W. Europe Standard Time") != std::string::npos);
  CHECK(xml.find("W269N-WFGWX-YVC9B-4J6C9-4T9TF") != std::string::npos);
  CHECK(xml.find("FirstLogonCommands") == std::string::npos);
  CHECK(xml.find("<AutoLogon>") != std::string::npos);
  CHECK(xml.find("LogonCount") == std::string::npos);
  answers.guestTools = true;
  const std::string withTools = coffret::windowsAnswerFile(answers);
  pugi::xml_document tools;
  CHECK(tools.load_string(withTools.c_str()));
  CHECK(withTools.find("virtio-win-guest-tools.exe /install /quiet") != std::string::npos);
  answers.graphicsDriver = true;
  const std::string withDriver = coffret::windowsAnswerFile(answers);
  pugi::xml_document driver;
  CHECK(driver.load_string(withDriver.c_str()));
  CHECK(withDriver.find("viogpudo.inf") != std::string::npos);
  CHECK(withDriver.find("pnputil /add-driver $i /install") != std::string::npos);
  CHECK(withDriver.find("<Order>2</Order>") != std::string::npos);
  return 0;
}
