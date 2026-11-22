#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coffret {
  // A USB device plugged into the host.
  struct HostUsbDevice {
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    // Where it is plugged in right now; both change when it is plugged in again.
    unsigned bus = 0;
    unsigned device = 0;
    // What it calls itself, e.g. "Logitech USB Receiver"; its ids when it says nothing.
    std::string name;
    bool operator==(const HostUsbDevice&) const = default;
  };

  // The devices under a sysfs tree, /sys/bus/usb/devices on a real system, without hubs: a guest
  // has no use for them, and taking one away takes everything behind it.
  std::vector<HostUsbDevice> listUsbDevices(const std::string& sysfs = "/sys/bus/usb/devices");

  // What libvirt needs to give the device as it is plugged in now to a machine, or take it back.
  std::string usbHostdevXml(const HostUsbDevice& device);
} // namespace coffret
