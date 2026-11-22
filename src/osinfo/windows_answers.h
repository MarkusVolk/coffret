#pragma once

#include <string>
#include <string_view>

namespace coffret {
  // What Windows Setup is told to install without anybody at it.
  struct WindowsAnswers {
    std::string user;
    std::string realName;
    std::string password;
    std::string computerName;
    // X11 layout, language and IANA time zone of the host: "de", "en_US", "Europe/Berlin".
    std::string keyboard;
    std::string language;
    std::string timezone;
    // The language the image comes in, "en-GB"; Setup knows no other for its own pages.
    std::string imageLanguage;
    // The edition to install and a key for it.
    std::string edition = "Windows 11 Pro";
    std::string productKey;
    // virtio-win-guest-tools.exe lies next to the answers: installed at the first logon, it brings
    // the virtio drivers, the SPICE agent for clipboard and window size, and the QEMU guest agent.
    bool guestTools = false;
    // A folder viogpu3d with viogpu3d.inf lies next to the answers: pnputil installs it at the
    // first logon, for Direct3D on the host's GPU.
    bool graphicsDriver = false;
  };

  // An autounattend.xml for Windows 10 and 11 on amd64 with UEFI: the first disk wiped and
  // partitioned, a local account in the Administrators group, no online account pages.
  std::string windowsAnswerFile(const WindowsAnswers& answers);

  // PowerShell that installs viogpu3d from `inf` (a PowerShell expression for its path) as the
  // graphics driver. The guest tools bring viogpudo with a higher version, which Windows would
  // keep for the same device: it goes first.
  std::string windowsGraphicsDriverScript(std::string_view inf);
  // PowerShell that exits with 0 when Windows draws with viogpu3d.
  std::string windowsGraphicsDriverCheck();

  // Windows' names for the host's settings: "0407:00000407" for "de", "en-US" for "en_US",
  // "W. Europe Standard Time" for "Europe/Berlin"; the US layout, en-US and UTC where none is known.
  std::string windowsKeyboard(std::string_view x11Layout);
  std::string windowsLocale(std::string_view language);
  std::string windowsTimeZone(std::string_view iana);
  // A NetBIOS name: letters, digits and dashes, at most 15, upper case.
  std::string windowsComputerName(std::string_view machineName);

  // A key that installs an edition and does not activate it: Microsoft's generic key on retail
  // media (volume id CCCOMA_...), its KMS client setup key on volume licensing media.
  std::string windowsSetupKey(std::string_view edition, bool volumeMedia);
} // namespace coffret
