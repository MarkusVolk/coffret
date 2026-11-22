#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffret {
  enum class GuestKind : std::uint8_t { Linux, Windows, Yocto, Other };
  enum class Firmware : std::uint8_t { Bios, Uefi, UefiSecureBoot };
  enum class DiskBus : std::uint8_t { Virtio, Sata, Nvme, Scsi, Usb, Ide };
  enum class NetworkKind : std::uint8_t {
    // Through passt: the guest reaches out like any program of the user, no privileges needed.
    User,
    // On a bridge of the host: the guest is a machine of its own in the local network.
    Bridge,
  };

  struct Disk {
    std::string path;
    // "qcow2" or "raw".
    std::string format = "qcow2";
    DiskBus bus = DiskBus::Virtio;
    bool cdrom = false;
    bool readOnly = false;
    // Its place in the boot order, from 1; 0 for a disk that is not booted from.
    unsigned bootOrder = 0;
    bool operator==(const Disk&) const = default;
  };

  struct PortForward {
    bool udp = false;
    std::uint16_t hostPort = 0;
    std::uint16_t guestPort = 0;
    bool operator==(const PortForward&) const = default;
  };

  struct Nic {
    NetworkKind kind = NetworkKind::User;
    // The host's bridge, for NetworkKind::Bridge.
    std::string bridge;
    // Empty for one libvirt makes up.
    std::string mac;
    std::vector<PortForward> forwards;
    // The card the guest sees: virtio, or e1000e for a system without virtio drivers, as Windows.
    std::string model = "virtio";
    bool operator==(const Nic&) const = default;
  };

  struct SharedFolder {
    std::string path;
    // The name the guest mounts it by.
    std::string tag;
    bool readOnly = false;
    bool operator==(const SharedFolder&) const = default;
  };

  // A device of the host given to the guest for good, found again by its ids at every start.
  struct UsbDevice {
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    bool operator==(const UsbDevice&) const = default;
  };

  // The installer's kernel booted with an answer file, until the installation is done.
  struct DirectBoot {
    std::string kernel;
    std::string initrd;
    std::string cmdline;
    bool operator==(const DirectBoot&) const = default;
  };

  // A virtual machine as coffret sets it up. Everything else in a libvirt domain is left as it is.
  struct VmConfig {
    std::string name;
    // Empty for one libvirt makes up.
    std::string uuid;
    std::string description;

    GuestKind kind = GuestKind::Linux;
    // The libosinfo id of the guest's system, e.g. "http://fedoraproject.org/fedora/45"; may be empty.
    std::string osId;

    unsigned memoryMiB = 4096;
    unsigned cpus = 2;
    Firmware firmware = Firmware::Uefi;
    // The firmware's code and the file new variables are copied from, both qcow2; with them empty
    // libvirt picks a firmware by itself, which needs one that comes as qcow2.
    std::string firmwareCode;
    std::string nvramTemplate;
    // Where the firmware keeps its variables; needed for UEFI, a qcow2 file so snapshots include it.
    std::string nvramPath;
    bool tpm = false;

    std::vector<Disk> disks;
    std::vector<Nic> nics;
    std::vector<SharedFolder> folders;
    std::vector<UsbDevice> usbDevices;
    // Set while an unattended installation runs: the machine then stops instead of rebooting, so
    // coffret sees the end and boots it from its disk.
    DirectBoot install;

    // The guest renders on the host's GPU (virgl); its picture reaches the window without a copy.
    bool accel3d = true;
    // Vulkan for the guest on top of that; needs a Linux guest that is recent enough.
    bool venus = false;
    // Direct3D for a Windows guest with the viogpu3d driver, through virglrenderer's Neptune.
    bool neptune = false;
    // The GPU to render on, e.g. "/dev/dri/renderD129"; required with accel3d.
    std::string renderNode;
    bool sound = true;
    // The guest hears the host's microphone; off unless asked for.
    bool microphone = false;
    // Copy and paste between host and guest, for guests that run spice-vdagent.
    bool clipboard = true;
    bool guestAgent = true;

    // What only coffret knows about the machine.
    std::string folder;
    bool favourite = false;

    bool operator==(const VmConfig&) const = default;
  };

  // Port forwards as text, "tcp:2222:22, udp:5353:53" (protocol, the host's port, the guest's), and
  // back; nullopt for text that is not that.
  std::string formatPortForwards(const std::vector<PortForward>& forwards);
  std::optional<std::vector<PortForward>> parsePortForwards(std::string_view text);

  const char* toString(GuestKind kind);
  const char* toString(DiskBus bus);
} // namespace coffret
