#include "osinfo/windows_answers.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace {
  std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
      switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
      }
    }
    return out;
  }

  constexpr std::array kKeyboards{
      std::pair{"de", "0407:00000407"}, std::pair{"us", "0409:00000409"}, std::pair{"gb", "0809:00000809"},
      std::pair{"fr", "040c:0000040c"}, std::pair{"ch", "0807:00000807"}, std::pair{"at", "0c07:00000407"},
      std::pair{"es", "040a:0000040a"}, std::pair{"it", "0410:00000410"}, std::pair{"nl", "0413:00020409"},
      std::pair{"be", "080c:0000080c"}, std::pair{"se", "041d:0000041d"}, std::pair{"no", "0414:00000414"},
      std::pair{"dk", "0406:00000406"}, std::pair{"fi", "040b:0000040b"}, std::pair{"pl", "0415:00000415"},
      std::pair{"cz", "0405:00000405"}, std::pair{"pt", "0816:00000816"}, std::pair{"br", "0416:00000416"},
      std::pair{"ru", "0419:00000419"}, std::pair{"tr", "041f:0000041f"}, std::pair{"hu", "040e:0000040e"},
  };

  constexpr std::array kTimeZones{
      std::pair{"Europe/Berlin", "W. Europe Standard Time"},    std::pair{"Europe/Vienna", "W. Europe Standard Time"},
      std::pair{"Europe/Zurich", "W. Europe Standard Time"},    std::pair{"Europe/Amsterdam", "W. Europe Standard Time"},
      std::pair{"Europe/Rome", "W. Europe Standard Time"},      std::pair{"Europe/Stockholm", "W. Europe Standard Time"},
      std::pair{"Europe/Oslo", "W. Europe Standard Time"},      std::pair{"Europe/Luxembourg", "W. Europe Standard Time"},
      std::pair{"Europe/Paris", "Romance Standard Time"},       std::pair{"Europe/Brussels", "Romance Standard Time"},
      std::pair{"Europe/Madrid", "Romance Standard Time"},      std::pair{"Europe/Copenhagen", "Romance Standard Time"},
      std::pair{"Europe/Warsaw", "Central European Standard Time"}, std::pair{"Europe/Prague", "Central Europe Standard Time"},
      std::pair{"Europe/Budapest", "Central Europe Standard Time"}, std::pair{"Europe/London", "GMT Standard Time"},
      std::pair{"Europe/Dublin", "GMT Standard Time"},          std::pair{"Europe/Lisbon", "GMT Standard Time"},
      std::pair{"Europe/Helsinki", "FLE Standard Time"},        std::pair{"Europe/Athens", "GTB Standard Time"},
      std::pair{"Europe/Istanbul", "Turkey Standard Time"},     std::pair{"Europe/Moscow", "Russian Standard Time"},
      std::pair{"America/New_York", "Eastern Standard Time"},   std::pair{"America/Chicago", "Central Standard Time"},
      std::pair{"America/Denver", "Mountain Standard Time"},    std::pair{"America/Los_Angeles", "Pacific Standard Time"},
      std::pair{"Asia/Tokyo", "Tokyo Standard Time"},           std::pair{"Australia/Sydney", "AUS Eastern Standard Time"},
      std::pair{"UTC", "UTC"},                                  std::pair{"Etc/UTC", "UTC"},
  };

  constexpr std::array kGenericKeys{
      std::pair{"Windows 11 Home", "YTMG3-N6DKC-DKB77-7M9GH-8HVX7"},
      std::pair{"Windows 11 Pro", "VK7JG-NPHTM-C97JM-9MPGT-3V66T"},
      std::pair{"Windows 10 Home", "YTMG3-N6DKC-DKB77-7M9GH-8HVX7"},
      std::pair{"Windows 10 Pro", "VK7JG-NPHTM-C97JM-9MPGT-3V66T"},
  };

  constexpr std::array kKmsKeys{
      std::pair{"Windows 11 Pro", "W269N-WFGWX-YVC9B-4J6C9-4T9TF"},
      std::pair{"Windows 11 Pro N", "MH37W-N47XK-V7XM9-C7227-GCQG9"},
      std::pair{"Windows 11 Education", "NW6C2-QMPVW-D7KKK-3GKT6-VCFB2"},
      std::pair{"Windows 11 Enterprise", "NPPR9-FWDCX-D2C8J-H872K-2YT43"},
      std::pair{"Windows 10 Pro", "W269N-WFGWX-YVC9B-4J6C9-4T9TF"},
  };

  template <typename Table>
  std::string lookUp(const Table& table, std::string_view key, std::string_view otherwise) {
    const auto it = std::ranges::find_if(table, [key](const auto& entry) { return key == entry.first; });
    return std::string(it == table.end() ? otherwise : it->second);
  }

  constexpr std::string_view kComponent =
      R"(processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS")";
} // namespace

std::string coffret::windowsKeyboard(std::string_view x11Layout) { return lookUp(kKeyboards, x11Layout, "0409:00000409"); }

std::string coffret::windowsTimeZone(std::string_view iana) { return lookUp(kTimeZones, iana, "UTC"); }

std::string coffret::windowsSetupKey(std::string_view edition, bool volumeMedia) {
  return volumeMedia ? lookUp(kKmsKeys, edition, "") : lookUp(kGenericKeys, edition, "");
}

std::string coffret::windowsLocale(std::string_view language) {
  const std::string_view base = language.substr(0, language.find_first_of(".@"));
  if (base.size() != 5 || base[2] != '_') {
    return "en-US";
  }
  return std::format("{}-{}", base.substr(0, 2), base.substr(3, 2));
}

std::string coffret::windowsComputerName(std::string_view machineName) {
  std::string name;
  for (const char c : machineName) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      name += c;
    } else if (c >= 'a' && c <= 'z') {
      name += static_cast<char>(c - 'a' + 'A');
    } else if (!name.empty() && name.back() != '-') {
      name += '-';
    }
  }
  if (name.size() > 15) {
    name.resize(15);
  }
  while (!name.empty() && name.back() == '-') {
    name.pop_back();
  }
  // A name of digits alone is no name to Windows.
  if (name.empty() || std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || c == '-'; })) {
    name = "WINDOWS";
  }
  return name;
}

std::string coffret::windowsAnswerFile(const WindowsAnswers& a) {
  const std::string keyboard = windowsKeyboard(a.keyboard);
  const std::string locale = windowsLocale(a.language);
  const std::string zone = windowsTimeZone(a.timezone);
  const std::string ui = a.imageLanguage.empty() ? "en-US" : a.imageLanguage;
  const std::string user = escape(a.user);
  const std::string password = escape(a.password);
  const std::string international = std::format(
      "      <InputLocale>{}</InputLocale>\n      <SystemLocale>{}</SystemLocale>\n      <UILanguage>{}</UILanguage>\n"
      "      <UserLocale>{}</UserLocale>\n",
      keyboard, locale, ui, locale
  );
  return std::format(
      R"(<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="windowsPE">
    <component name="Microsoft-Windows-International-Core-WinPE" {0}>
      <SetupUILanguage><UILanguage>{1}</UILanguage></SetupUILanguage>
{2}    </component>
    <component name="Microsoft-Windows-Setup" {0}>
      <DiskConfiguration>
        <Disk wcm:action="add">
          <DiskID>0</DiskID>
          <WillWipeDisk>true</WillWipeDisk>
          <CreatePartitions>
            <CreatePartition wcm:action="add"><Order>1</Order><Type>EFI</Type><Size>300</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>
          </CreatePartitions>
          <ModifyPartitions>
            <ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>
            <ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>2</PartitionID></ModifyPartition>
            <ModifyPartition wcm:action="add"><Order>3</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>
          </ModifyPartitions>
        </Disk>
      </DiskConfiguration>
      <ImageInstall>
        <OSImage>
          <InstallFrom><MetaData wcm:action="add"><Key>/IMAGE/NAME</Key><Value>{3}</Value></MetaData></InstallFrom>
          <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>
        </OSImage>
      </ImageInstall>
      <UserData>
        <ProductKey><Key>{4}</Key><WillShowUI>OnError</WillShowUI></ProductKey>
        <AcceptEula>true</AcceptEula>
      </UserData>
    </component>
  </settings>
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" {0}>
      <ComputerName>{5}</ComputerName>
      <TimeZone>{6}</TimeZone>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" {0}>
{2}    </component>
    <component name="Microsoft-Windows-Shell-Setup" {0}>
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>{7}</Name>
            <DisplayName>{8}</DisplayName>
            <Group>Administrators</Group>
            <Password><Value>{9}</Value><PlainText>true</PlainText></Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Username>{7}</Username>
        <Enabled>true</Enabled>
        <LogonCount>1</LogonCount>
        <Password><Value>{9}</Value><PlainText>true</PlainText></Password>
      </AutoLogon>
{10}      <TimeZone>{6}</TimeZone>
    </component>
  </settings>
</unattend>
)",
      kComponent, escape(ui), international, escape(a.edition), escape(a.productKey), escape(a.computerName), escape(zone), user,
      escape(a.realName.empty() ? a.user : a.realName), password,
      a.guestTools ? R"(      <FirstLogonCommands>
        <SynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Description>virtio-win guest tools</Description>
          <CommandLine>cmd /c for %d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do if exist %d:\virtio-win-guest-tools.exe start /wait %d:\virtio-win-guest-tools.exe /install /quiet /norestart</CommandLine>
        </SynchronousCommand>
      </FirstLogonCommands>
)"
                   : ""
  );
}
