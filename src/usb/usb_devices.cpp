#include "usb/usb_devices.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>

namespace {
  std::string line(const std::filesystem::path& file) {
    std::ifstream stream(file);
    std::string text;
    std::getline(stream, text);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) {
      text.pop_back();
    }
    return text;
  }

  unsigned number(const std::string& text, int base) {
    unsigned value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value, base);
    return value;
  }
} // namespace

std::vector<coffret::HostUsbDevice> coffret::listUsbDevices(const std::string& sysfs) {
  std::vector<HostUsbDevice> devices;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(sysfs, error)) {
    const auto& path = entry.path();
    // Interfaces ("1-2:1.0") have no ids of their own; hubs are class 09.
    if (!std::filesystem::exists(path / "idVendor", error) || line(path / "bDeviceClass") == "09") {
      continue;
    }
    HostUsbDevice device;
    device.vendor = static_cast<std::uint16_t>(number(line(path / "idVendor"), 16));
    device.product = static_cast<std::uint16_t>(number(line(path / "idProduct"), 16));
    device.bus = number(line(path / "busnum"), 10);
    device.device = number(line(path / "devnum"), 10);
    const std::string maker = line(path / "manufacturer"), product = line(path / "product");
    device.name = product.empty() ? maker : maker.empty() || product.starts_with(maker) ? product : maker + " " + product;
    if (device.name.empty()) {
      device.name = std::format("{:04x}:{:04x}", device.vendor, device.product);
    }
    devices.push_back(std::move(device));
  }
  std::ranges::sort(devices, [](const HostUsbDevice& a, const HostUsbDevice& b) {
    return a.bus != b.bus ? a.bus < b.bus : a.device < b.device;
  });
  return devices;
}

std::string coffret::usbHostdevXml(const HostUsbDevice& device) {
  return std::format(
      "<hostdev mode='subsystem' type='usb' managed='yes'><source><vendor id='{:#06x}'/><product id='{:#06x}'/>"
      "<address bus='{}' device='{}'/></source></hostdev>",
      device.vendor, device.product, device.bus, device.device
  );
}
