#include "check.h"
#include "usb/usb_devices.h"

#include <filesystem>
#include <fstream>

namespace {
  void write(const std::filesystem::path& file, const char* text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text << "\n";
  }
} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "coffret-usb-test";
  std::filesystem::remove_all(root);
  // A root hub, a receiver, a stick without a manufacturer string, and an interface.
  write(root / "usb1/idVendor", "1d6b");
  write(root / "usb1/idProduct", "0002");
  write(root / "usb1/bDeviceClass", "09");
  write(root / "1-3/idVendor", "046d");
  write(root / "1-3/idProduct", "c548");
  write(root / "1-3/bDeviceClass", "00");
  write(root / "1-3/busnum", "1");
  write(root / "1-3/devnum", "3");
  write(root / "1-3/manufacturer", "Logitech");
  write(root / "1-3/product", "USB Receiver");
  write(root / "2-2/idVendor", "090c");
  write(root / "2-2/idProduct", "2320");
  write(root / "2-2/bDeviceClass", "00");
  write(root / "2-2/busnum", "2");
  write(root / "2-2/devnum", "12");
  write(root / "1-3:1.0/bInterfaceClass", "03");

  const auto devices = coffret::listUsbDevices(root.string());
  CHECK(devices.size() == 2);
  CHECK((devices[0] == coffret::HostUsbDevice{0x046d, 0xc548, 1, 3, "Logitech USB Receiver"}));
  CHECK((devices[1] == coffret::HostUsbDevice{0x090c, 0x2320, 2, 12, "090c:2320"}));
  CHECK(coffret::usbHostdevXml(devices[1])
        == "<hostdev mode='subsystem' type='usb' managed='yes'><source><vendor id='0x090c'/><product id='0x2320'/>"
           "<address bus='2' device='12'/></source></hostdev>");
  CHECK(coffret::listUsbDevices((root / "none").string()).empty());
  std::filesystem::remove_all(root);
  return 0;
}
