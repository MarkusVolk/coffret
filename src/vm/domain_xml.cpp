#include "vm/domain_xml.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <pugixml.hpp>
#include <set>
#include <sstream>

namespace {
  using coffret::DiskBus;
  using coffret::Firmware;
  using coffret::GuestKind;
  using coffret::NetworkKind;
  using pugi::xml_node;

  constexpr const char* kMetadataNamespace = "https://github.com/MarkusVolk/coffret/xmlns/1";
  constexpr const char* kQemuNamespace = "http://libvirt.org/schemas/domain/qemu/1.0";
  constexpr const char* kVideoAlias = "ua-coffret-video";
  constexpr const char* kAgentChannel = "org.qemu.guest_agent.0";
  constexpr const char* kVdagentChannel = "com.redhat.spice.0";

  // The fixed part of a new machine; apply() fills in what the config decides.
  constexpr const char* kSkeleton = R"(<domain type='kvm'>
  <name/>
  <memory unit='MiB'>0</memory>
  <memoryBacking><source type='memfd'/><access mode='shared'/></memoryBacking>
  <vcpu placement='static'>1</vcpu>
  <os><type arch='x86_64' machine='q35'>hvm</type></os>
  <features><acpi/><apic/><vmport state='off'/></features>
  <cpu mode='host-passthrough' check='none' migratable='on'/>
  <clock offset='utc'>
    <timer name='rtc' tickpolicy='catchup'/><timer name='pit' tickpolicy='delay'/><timer name='hpet' present='no'/>
  </clock>
  <on_poweroff>destroy</on_poweroff>
  <on_reboot>restart</on_reboot>
  <on_crash>destroy</on_crash>
  <pm><suspend-to-mem enabled='no'/><suspend-to-disk enabled='no'/></pm>
  <devices>
    <controller type='usb' model='qemu-xhci' ports='15'/>
    <controller type='virtio-serial'/>
    <serial type='pty'><target type='isa-serial' port='0'/></serial>
    <console type='pty'><target type='serial' port='0'/></console>
    <input type='tablet' bus='usb'/>
    <graphics type='dbus' p2p='yes'/>
    <video><model type='virtio' heads='1' primary='yes'/></video>
    <memballoon model='virtio'/>
    <rng model='virtio'><backend model='random'>/dev/urandom</backend></rng>
  </devices>
</domain>)";

  const char* busName(DiskBus bus) {
    switch (bus) {
    case DiskBus::Virtio:
      return "virtio";
    case DiskBus::Sata:
      return "sata";
    case DiskBus::Nvme:
      return "nvme";
    case DiskBus::Scsi:
      return "scsi";
    case DiskBus::Usb:
      return "usb";
    case DiskBus::Ide:
      return "ide";
    }
    return "virtio";
  }

  DiskBus busFromName(std::string_view name) {
    for (const auto bus : {DiskBus::Virtio, DiskBus::Sata, DiskBus::Nvme, DiskBus::Scsi, DiskBus::Usb, DiskBus::Ide}) {
      if (name == busName(bus)) {
        return bus;
      }
    }
    return DiskBus::Sata;
  }

  // "vda", "sdb", "nvme0n1": the names of one bus family are numbered together.
  std::string targetPrefix(DiskBus bus) {
    switch (bus) {
    case DiskBus::Virtio:
      return "vd";
    case DiskBus::Ide:
      return "hd";
    case DiskBus::Nvme:
      return "nvme";
    default:
      return "sd";
    }
  }

  std::string freeTarget(DiskBus bus, std::set<std::string>& used) {
    for (unsigned index = 0;; ++index) {
      std::string name = bus == DiskBus::Nvme ? std::format("nvme{}n1", index)
                                              : targetPrefix(bus) + static_cast<char>('a' + index % 26);
      if (bus != DiskBus::Nvme && index >= 26) {
        name = targetPrefix(bus) + static_cast<char>('a' + index / 26 - 1) + static_cast<char>('a' + index % 26);
      }
      if (used.insert(name).second) {
        return name;
      }
    }
  }

  xml_node ensure(xml_node parent, const char* name) {
    xml_node child = parent.child(name);
    return child ? child : parent.append_child(name);
  }

  void setAttribute(xml_node node, const char* name, std::string_view value) {
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) {
      attribute = node.append_attribute(name);
    }
    attribute.set_value(std::string(value).c_str());
  }

  void setText(xml_node node, std::string_view value) { node.text().set(std::string(value).c_str()); }

  // The element kept when `wanted`, removed when not.
  xml_node present(xml_node parent, const char* name, bool wanted) {
    xml_node child = parent.child(name);
    if (!wanted) {
      while (child) {
        parent.remove_child(child);
        child = parent.child(name);
      }
      return {};
    }
    return child ? child : parent.append_child(name);
  }

  xml_node feature(xml_node firmware, const char* name) {
    for (xml_node node : firmware.children("feature")) {
      if (std::string_view(node.attribute("name").value()) == name) {
        return node;
      }
    }
    xml_node node = firmware.append_child("feature");
    setAttribute(node, "name", name);
    return node;
  }

  std::string serialize(const pugi::xml_document& document) {
    std::ostringstream out;
    document.save(out, "  ", pugi::format_indent | pugi::format_no_declaration);
    return out.str();
  }

  unsigned number(const char* text, unsigned fallback = 0) {
    const std::string_view view(text);
    unsigned value = fallback;
    std::from_chars(view.data(), view.data() + view.size(), value);
    return value;
  }

  std::uint16_t hexId(const char* text) {
    std::string_view view(text);
    if (view.starts_with("0x")) {
      view.remove_prefix(2);
    }
    unsigned value = 0;
    std::from_chars(view.data(), view.data() + view.size(), value, 16);
    return static_cast<std::uint16_t>(value);
  }

  xml_node channel(xml_node devices, std::string_view targetName) {
    for (xml_node node : devices.children("channel")) {
      if (targetName == node.child("target").attribute("name").value()) {
        return node;
      }
    }
    return {};
  }

  // ── Writing ──────────────────────────────────────────────────────────────────

  void applyMetadata(xml_node domain, const coffret::VmConfig& config) {
    xml_node metadata = ensure(domain, "metadata");
    if (xml_node old = metadata.child("coffret:vm")) {
      metadata.remove_child(old);
    }
    xml_node vm = metadata.append_child("coffret:vm");
    setAttribute(vm, "xmlns:coffret", kMetadataNamespace);
    xml_node guest = vm.append_child("coffret:guest");
    setAttribute(guest, "kind", coffret::toString(config.kind));
    if (!config.osId.empty()) {
      setAttribute(guest, "os", config.osId);
    }
    if (!config.folder.empty()) {
      setText(vm.append_child("coffret:folder"), config.folder);
    }
    if (config.favourite) {
      vm.append_child("coffret:favourite");
    }
  }

  void applyFirmware(xml_node domain, const coffret::VmConfig& config) {
    xml_node os = ensure(domain, "os");
    const bool uefi = config.firmware != Firmware::Bios;
    const bool secure = config.firmware == Firmware::UefiSecureBoot;
    if (uefi && !config.firmwareCode.empty()) {
      // A firmware named outright.
      os.remove_attribute("firmware");
      os.remove_child("firmware");
      xml_node loader = ensure(os, "loader");
      setAttribute(loader, "readonly", "yes");
      setAttribute(loader, "secure", secure ? "yes" : "no");
      setAttribute(loader, "type", "pflash");
      setAttribute(loader, "format", "qcow2");
      setText(loader, config.firmwareCode);
      xml_node nvram = ensure(os, "nvram");
      if (!config.nvramTemplate.empty()) {
        setAttribute(nvram, "template", config.nvramTemplate);
        setAttribute(nvram, "templateFormat", "qcow2");
      } else {
        nvram.remove_attribute("template");
        nvram.remove_attribute("templateFormat");
      }
      // qcow2, so an internal snapshot holds the firmware's variables with the disks.
      setAttribute(nvram, "format", "qcow2");
      setText(nvram, config.nvramPath);
    } else if (uefi) {
      setAttribute(os, "firmware", "efi");
      // The loader libvirt picked for the old setting would win over a changed one.
      os.remove_child("loader");
      xml_node firmware = ensure(os, "firmware");
      setAttribute(feature(firmware, "secure-boot"), "enabled", secure ? "yes" : "no");
      setAttribute(feature(firmware, "enrolled-keys"), "enabled", secure ? "yes" : "no");
      xml_node nvram = present(os, "nvram", !config.nvramPath.empty());
      if (nvram) {
        setAttribute(nvram, "format", "qcow2");
        nvram.remove_attribute("template");
        nvram.remove_attribute("templateFormat");
        setText(nvram, config.nvramPath);
      }
    } else {
      os.remove_attribute("firmware");
      os.remove_child("firmware");
      os.remove_child("loader");
      os.remove_child("nvram");
    }
    // The boot order is each disk's own, see applyDisks().
    while (os.remove_child("boot")) {
    }
    const bool installing = !config.install.kernel.empty();
    for (const auto& [name, value] : {std::pair{"kernel", config.install.kernel}, {"initrd", config.install.initrd}, {"cmdline", config.install.cmdline}}) {
      if (xml_node node = present(os, name, installing && !value.empty())) {
        setText(node, value);
      }
    }
    setText(ensure(domain, "on_reboot"), installing ? "destroy" : "restart");
    xml_node features = ensure(domain, "features");
    xml_node smm = present(features, "smm", secure);
    if (smm) {
      setAttribute(smm, "state", "on");
    }
  }

  void applyWindows(xml_node domain) {
    xml_node features = ensure(domain, "features");
    if (!features.child("hyperv")) {
      xml_node hyperv = features.append_child("hyperv");
      setAttribute(hyperv, "mode", "custom");
      for (const char* name : {"relaxed", "vapic", "vpindex", "runtime", "synic", "stimer", "frequencies", "tlbflush", "ipi"}) {
        setAttribute(hyperv.append_child(name), "state", "on");
      }
      xml_node spinlocks = hyperv.append_child("spinlocks");
      setAttribute(spinlocks, "state", "on");
      setAttribute(spinlocks, "retries", "8191");
    }
    xml_node clock = ensure(domain, "clock");
    setAttribute(clock, "offset", "localtime");
    bool hypervClock = false;
    for (xml_node timer : clock.children("timer")) {
      hypervClock = hypervClock || std::string_view(timer.attribute("name").value()) == "hypervclock";
    }
    if (!hypervClock) {
      xml_node timer = clock.append_child("timer");
      setAttribute(timer, "name", "hypervclock");
      setAttribute(timer, "present", "yes");
    }
  }

  void applyDisks(xml_node devices, const coffret::VmConfig& config) {
    std::vector<xml_node> existing;
    for (xml_node node : devices.children("disk")) {
      existing.push_back(node);
    }
    std::set<std::string> usedTargets;
    std::vector<xml_node> kept(config.disks.size());
    // A disk that stays keeps its element, and with it its address in the guest.
    for (std::size_t i = 0; i < config.disks.size(); ++i) {
      const coffret::Disk& disk = config.disks[i];
      for (xml_node& node : existing) {
        // A drive stays the same drive whatever is in it, or nothing.
        const bool same = disk.cdrom ? std::string_view(node.attribute("device").value()) == "cdrom"
                                     : disk.path == node.child("source").attribute("file").value();
        if (node && same && std::string_view(busName(disk.bus)) == node.child("target").attribute("bus").value()) {
          kept[i] = node;
          usedTargets.insert(node.child("target").attribute("dev").value());
          node = xml_node();
          break;
        }
      }
    }
    for (xml_node node : existing) {
      if (node) {
        devices.remove_child(node);
      }
    }
    // New disks follow the ones that are there, in the order given.
    xml_node last;
    for (xml_node node : devices.children("disk")) {
      last = node;
    }
    for (std::size_t i = 0; i < config.disks.size(); ++i) {
      const coffret::Disk& disk = config.disks[i];
      xml_node node = kept[i];
      if (!node) {
        node = last ? devices.insert_child_after("disk", last) : devices.prepend_child("disk");
        last = node;
        setAttribute(node, "type", "file");
        xml_node target = ensure(node, "target");
        setAttribute(target, "dev", freeTarget(disk.bus, usedTargets));
        setAttribute(target, "bus", busName(disk.bus));
      }
      setAttribute(node, "device", disk.cdrom ? "cdrom" : "disk");
      // An empty drive has no source at all; libvirt refuses an empty one.
      if (xml_node source = present(node, "source", !disk.path.empty())) {
        setAttribute(source, "file", disk.path);
      }
      xml_node driver = ensure(node, "driver");
      setAttribute(driver, "name", "qemu");
      setAttribute(driver, "type", disk.cdrom ? "raw" : disk.format);
      if (!disk.cdrom && disk.format == "qcow2") {
        setAttribute(driver, "discard", "unmap");
      }
      present(node, "readonly", disk.readOnly || disk.cdrom);
      xml_node boot = present(node, "boot", disk.bootOrder > 0);
      if (boot) {
        setAttribute(boot, "order", std::to_string(disk.bootOrder));
      }
    }
  }

  void applyNics(xml_node devices, const coffret::VmConfig& config) {
    std::vector<xml_node> existing;
    for (xml_node node : devices.children("interface")) {
      existing.push_back(node);
    }
    for (std::size_t i = config.nics.size(); i < existing.size(); ++i) {
      devices.remove_child(existing[i]);
    }
    for (std::size_t i = 0; i < config.nics.size(); ++i) {
      const coffret::Nic& nic = config.nics[i];
      xml_node node = i < existing.size() ? existing[i] : devices.append_child("interface");
      const bool bridge = nic.kind == NetworkKind::Bridge;
      setAttribute(node, "type", bridge ? "bridge" : "user");
      xml_node source = present(node, "source", bridge);
      if (source) {
        setAttribute(source, "bridge", nic.bridge);
      }
      xml_node backend = present(node, "backend", !bridge);
      if (backend) {
        setAttribute(backend, "type", "passt");
      }
      setAttribute(ensure(node, "model"), "type", nic.model.empty() ? "virtio" : nic.model);
      xml_node mac = present(node, "mac", !nic.mac.empty() || node.child("mac"));
      if (mac && !nic.mac.empty()) {
        setAttribute(mac, "address", nic.mac);
      }
      while (node.remove_child("portForward")) {
      }
      if (!bridge) {
        for (const bool udp : {false, true}) {
          xml_node forward;
          for (const auto& rule : nic.forwards) {
            if (rule.udp != udp) {
              continue;
            }
            if (!forward) {
              forward = node.append_child("portForward");
              setAttribute(forward, "proto", udp ? "udp" : "tcp");
            }
            xml_node range = forward.append_child("range");
            setAttribute(range, "start", std::to_string(rule.hostPort));
            setAttribute(range, "to", std::to_string(rule.guestPort));
          }
        }
      }
    }
  }

  void applyFolders(xml_node devices, const coffret::VmConfig& config, const coffret::DomainEnvironment& environment) {
    std::vector<xml_node> existing;
    for (xml_node node : devices.children("filesystem")) {
      if (std::string_view(node.child("driver").attribute("type").value()) == "virtiofs") {
        existing.push_back(node);
      }
    }
    for (xml_node node : existing) {
      const std::string_view tag = node.child("target").attribute("dir").value();
      if (std::ranges::none_of(config.folders, [tag](const auto& folder) { return folder.tag == tag; })) {
        devices.remove_child(node);
      }
    }
    for (const auto& folder : config.folders) {
      xml_node node;
      for (xml_node candidate : devices.children("filesystem")) {
        if (folder.tag == candidate.child("target").attribute("dir").value()) {
          node = candidate;
        }
      }
      if (!node) {
        node = devices.append_child("filesystem");
        setAttribute(node, "type", "mount");
        setAttribute(node, "accessmode", "passthrough");
        setAttribute(ensure(node, "driver"), "type", "virtiofs");
        setAttribute(ensure(node, "target"), "dir", folder.tag);
      }
      setAttribute(ensure(node, "source"), "dir", folder.path);
      present(node, "readonly", folder.readOnly);
      if (!environment.virtiofsd.empty()) {
        setAttribute(ensure(node, "binary"), "path", environment.virtiofsd);
      }
      if (environment.uid >= 0 && environment.gid >= 0 && !node.child("idmap")) {
        xml_node idmap = node.append_child("idmap");
        const auto map = [&idmap](const char* name, long target) {
          xml_node entry = idmap.append_child(name);
          setAttribute(entry, "start", "0");
          setAttribute(entry, "target", std::to_string(target));
          setAttribute(entry, "count", "1");
        };
        map("uid", environment.uid);
        map("gid", environment.gid);
      }
    }
  }

  void applyUsb(xml_node devices, const coffret::VmConfig& config) {
    std::vector<xml_node> existing;
    for (xml_node node : devices.children("hostdev")) {
      if (std::string_view(node.attribute("type").value()) == "usb" && node.child("source").child("vendor")) {
        existing.push_back(node);
      }
    }
    const auto same = [](xml_node node, const coffret::UsbDevice& device) {
      return hexId(node.child("source").child("vendor").attribute("id").value()) == device.vendor
          && hexId(node.child("source").child("product").attribute("id").value()) == device.product;
    };
    for (xml_node node : existing) {
      if (std::ranges::none_of(config.usbDevices, [&](const auto& device) { return same(node, device); })) {
        devices.remove_child(node);
      }
    }
    for (const auto& device : config.usbDevices) {
      if (std::ranges::any_of(existing, [&](xml_node node) { return node.parent() && same(node, device); })) {
        continue;
      }
      xml_node node = devices.append_child("hostdev");
      setAttribute(node, "mode", "subsystem");
      setAttribute(node, "type", "usb");
      setAttribute(node, "managed", "yes");
      xml_node source = node.append_child("source");
      // The machine starts without a device that is not plugged in.
      setAttribute(source, "startupPolicy", "optional");
      setAttribute(source.append_child("vendor"), "id", std::format("{:#06x}", device.vendor));
      setAttribute(source.append_child("product"), "id", std::format("{:#06x}", device.product));
    }
  }

  void applyDisplay(xml_node domain, xml_node devices, const coffret::VmConfig& config) {
    xml_node graphics = ensure(devices, "graphics");
    setAttribute(graphics, "type", "dbus");
    setAttribute(graphics, "p2p", "yes");
    xml_node gl = present(graphics, "gl", config.accel3d);
    if (gl) {
      setAttribute(gl, "enable", "yes");
      if (!config.renderNode.empty()) {
        setAttribute(gl, "rendernode", config.renderNode);
      } else {
        gl.remove_attribute("rendernode");
      }
    }
    xml_node video = ensure(devices, "video");
    xml_node model = ensure(video, "model");
    setAttribute(model, "type", "virtio");
    xml_node acceleration = present(model, "acceleration", config.accel3d);
    if (acceleration) {
      setAttribute(acceleration, "accel3d", "yes");
    }
    // libvirt writes down which of QEMU's devices it took, and that one depends on the 3D setting.
    model.remove_attribute("device");
    // libvirt has no setting for Vulkan in the guest: it goes to QEMU's device directly.
    const bool venus = config.accel3d && config.venus;
    if (venus) {
      setAttribute(model, "blob", "on");
      setAttribute(ensure(video, "alias"), "name", kVideoAlias);
    } else {
      model.remove_attribute("blob");
    }
    xml_node override = domain.child("qemu:override");
    xml_node device;
    for (xml_node node : override.children("qemu:device")) {
      if (std::string_view(node.attribute("alias").value()) == kVideoAlias) {
        device = node;
      }
    }
    if (device) {
      override.remove_child(device);
      if (!override.first_child()) {
        domain.remove_child(override);
      }
    }
    if (venus) {
      setAttribute(domain, "xmlns:qemu", kQemuNamespace);
      override = ensure(domain, "qemu:override");
      device = override.append_child("qemu:device");
      setAttribute(device, "alias", kVideoAlias);
      xml_node frontend = device.append_child("qemu:frontend");
      const auto property = [&frontend](const char* name, const char* type, const char* value) {
        xml_node node = frontend.append_child("qemu:property");
        setAttribute(node, "name", name);
        setAttribute(node, "type", type);
        setAttribute(node, "value", value);
      };
      property("venus", "bool", "true");
      // Room on the host for the buffers guest and host share, 4 GiB of address space.
      property("hostmem", "unsigned", "4294967296");
    }
  }

  void applySound(xml_node devices, const coffret::VmConfig& config, const coffret::DomainEnvironment& environment) {
    xml_node sound = present(devices, "sound", config.sound);
    xml_node audio = present(devices, "audio", config.sound);
    if (!sound) {
      return;
    }
    // QEMU plays through the user's PipeWire by itself; sound goes on when no window is open.
    setAttribute(audio, "id", "1");
    setAttribute(audio, "type", "pipewire");
    if (!environment.runtimeDir.empty()) {
      setAttribute(audio, "runtimeDir", environment.runtimeDir);
    }
    if (!sound.attribute("model")) {
      setAttribute(sound, "model", "ich9");
    }
    setAttribute(ensure(sound, "audio"), "id", "1");
    // A sound card that only plays has no way to listen.
    while (sound.remove_child("codec")) {
    }
    setAttribute(sound.append_child("codec"), "type", config.microphone ? "duplex" : "output");
  }

  void applyChannels(xml_node devices, const coffret::VmConfig& config) {
    xml_node agent = channel(devices, kAgentChannel);
    if (config.guestAgent && !agent) {
      agent = devices.append_child("channel");
      setAttribute(agent, "type", "unix");
      xml_node target = agent.append_child("target");
      setAttribute(target, "type", "virtio");
      setAttribute(target, "name", kAgentChannel);
    } else if (!config.guestAgent && agent) {
      devices.remove_child(agent);
    }
    xml_node vdagent = channel(devices, kVdagentChannel);
    if (config.clipboard && !vdagent) {
      vdagent = devices.append_child("channel");
      setAttribute(vdagent, "type", "qemu-vdagent");
      xml_node source = vdagent.append_child("source");
      setAttribute(source.append_child("clipboard"), "copypaste", "yes");
      setAttribute(source.append_child("mouse"), "mode", "client");
      xml_node target = vdagent.append_child("target");
      setAttribute(target, "type", "virtio");
      setAttribute(target, "name", kVdagentChannel);
    } else if (!config.clipboard && vdagent) {
      devices.remove_child(vdagent);
    }
  }

  void apply(pugi::xml_document& document, const coffret::VmConfig& config, const coffret::DomainEnvironment& environment) {
    xml_node domain = document.child("domain");
    setText(ensure(domain, "name"), config.name);
    xml_node uuid = present(domain, "uuid", !config.uuid.empty() || domain.child("uuid"));
    if (uuid && !config.uuid.empty()) {
      setText(uuid, config.uuid);
    }
    xml_node description = present(domain, "description", !config.description.empty());
    if (description) {
      setText(description, config.description);
    }
    applyMetadata(domain, config);
    xml_node memory = ensure(domain, "memory");
    setAttribute(memory, "unit", "MiB");
    setText(memory, std::to_string(config.memoryMiB));
    domain.remove_child("currentMemory");
    setText(ensure(domain, "vcpu"), std::to_string(config.cpus));
    // One socket with all the cores: Windows uses no more than two sockets.
    xml_node topology = ensure(ensure(domain, "cpu"), "topology");
    setAttribute(topology, "sockets", "1");
    setAttribute(topology, "dies", "1");
    setAttribute(topology, "cores", std::to_string(config.cpus));
    setAttribute(topology, "threads", "1");
    applyFirmware(domain, config);
    if (config.kind == GuestKind::Windows) {
      applyWindows(domain);
    }
    xml_node devices = ensure(domain, "devices");
    applyDisks(devices, config);
    applyNics(devices, config);
    applyFolders(devices, config, environment);
    applyUsb(devices, config);
    applyDisplay(domain, devices, config);
    applySound(devices, config, environment);
    applyChannels(devices, config);
    xml_node tpm = present(devices, "tpm", config.tpm);
    if (tpm && !tpm.child("backend")) {
      setAttribute(tpm, "model", "tpm-crb");
      xml_node backend = tpm.append_child("backend");
      setAttribute(backend, "type", "emulator");
      setAttribute(backend, "version", "2.0");
    }
  }
} // namespace

const char* coffret::toString(GuestKind kind) {
  switch (kind) {
  case GuestKind::Linux:
    return "linux";
  case GuestKind::Windows:
    return "windows";
  case GuestKind::Yocto:
    return "yocto";
  case GuestKind::Other:
    return "other";
  }
  return "other";
}

const char* coffret::toString(DiskBus bus) { return busName(bus); }

std::string coffret::buildDomainXml(const VmConfig& config, const DomainEnvironment& environment) {
  pugi::xml_document document;
  document.load_string(kSkeleton);
  apply(document, config, environment);
  return serialize(document);
}

std::expected<std::string, std::string>
coffret::updateDomainXml(std::string_view xml, const VmConfig& config, const DomainEnvironment& environment) {
  pugi::xml_document document;
  const auto parsed = document.load_buffer(xml.data(), xml.size());
  if (!parsed || !document.child("domain")) {
    return std::unexpected(std::format("not a domain: {}", parsed ? "no <domain> element" : parsed.description()));
  }
  apply(document, config, environment);
  return serialize(document);
}

std::expected<coffret::VmConfig, std::string> coffret::readDomainXml(std::string_view xml) {
  pugi::xml_document document;
  const auto parsed = document.load_buffer(xml.data(), xml.size());
  const xml_node domain = document.child("domain");
  if (!parsed || !domain) {
    return std::unexpected(std::format("not a domain: {}", parsed ? "no <domain> element" : parsed.description()));
  }
  VmConfig config;
  config.name = domain.child_value("name");
  config.uuid = domain.child_value("uuid");
  config.description = domain.child_value("description");

  const xml_node vm = domain.child("metadata").child("coffret:vm");
  const std::string_view kind = vm.child("coffret:guest").attribute("kind").value();
  config.kind = GuestKind::Other;
  for (const auto candidate : {GuestKind::Linux, GuestKind::Windows, GuestKind::Yocto, GuestKind::Other}) {
    if (kind == toString(candidate)) {
      config.kind = candidate;
    }
  }
  config.osId = vm.child("coffret:guest").attribute("os").value();
  config.folder = vm.child_value("coffret:folder");
  config.favourite = static_cast<bool>(vm.child("coffret:favourite"));

  const xml_node memory = domain.child("memory");
  const std::string_view unit = memory.attribute("unit").as_string("KiB");
  const unsigned long long amount = memory.text().as_ullong();
  config.memoryMiB = static_cast<unsigned>(
      unit == "MiB" || unit == "M"   ? amount
      : unit == "GiB" || unit == "G" ? amount * 1024
      : unit == "b" || unit == "bytes" ? amount / (1024 * 1024)
                                       : amount / 1024
  );
  config.cpus = number(domain.child_value("vcpu"), 1);

  const xml_node os = domain.child("os");
  config.firmware = Firmware::Bios;
  const bool uefi = std::string_view(os.attribute("firmware").value()) == "efi"
                 || std::string_view(os.child("loader").attribute("type").value()) == "pflash";
  if (uefi) {
    config.firmware = Firmware::Uefi;
    for (xml_node node : os.child("firmware").children("feature")) {
      if (std::string_view(node.attribute("name").value()) == "secure-boot"
          && std::string_view(node.attribute("enabled").value()) == "yes") {
        config.firmware = Firmware::UefiSecureBoot;
      }
    }
    if (std::string_view(os.child("loader").attribute("secure").value()) == "yes") {
      config.firmware = Firmware::UefiSecureBoot;
    }
  }
  config.nvramPath = os.child_value("nvram");
  config.install = {.kernel = os.child_value("kernel"), .initrd = os.child_value("initrd"), .cmdline = os.child_value("cmdline")};
  // A firmware libvirt picked is its to pick again; one named outright stays.
  if (uefi && !os.attribute("firmware")) {
    config.firmwareCode = os.child_value("loader");
    config.nvramTemplate = os.child("nvram").attribute("template").value();
  }

  const xml_node devices = domain.child("devices");
  config.tpm = static_cast<bool>(devices.child("tpm"));
  for (xml_node node : devices.children("disk")) {
    Disk disk;
    disk.path = node.child("source").attribute("file").value();
    disk.format = node.child("driver").attribute("type").as_string("raw");
    disk.bus = busFromName(node.child("target").attribute("bus").value());
    disk.cdrom = std::string_view(node.attribute("device").value()) == "cdrom";
    disk.readOnly = static_cast<bool>(node.child("readonly"));
    disk.bootOrder = number(node.child("boot").attribute("order").value());
    if (disk.cdrom) {
      disk.format = "raw";
    }
    config.disks.push_back(std::move(disk));
  }
  for (xml_node node : devices.children("interface")) {
    Nic nic;
    nic.kind = std::string_view(node.attribute("type").value()) == "bridge" ? NetworkKind::Bridge : NetworkKind::User;
    nic.bridge = node.child("source").attribute("bridge").value();
    nic.model = node.child("model").attribute("type").as_string("virtio");
    nic.mac = node.child("mac").attribute("address").value();
    for (xml_node forward : node.children("portForward")) {
      const bool udp = std::string_view(forward.attribute("proto").value()) == "udp";
      for (xml_node range : forward.children("range")) {
        const auto host = static_cast<std::uint16_t>(number(range.attribute("start").value()));
        nic.forwards.push_back({udp, host, static_cast<std::uint16_t>(number(range.attribute("to").value(), host))});
      }
    }
    config.nics.push_back(std::move(nic));
  }
  for (xml_node node : devices.children("filesystem")) {
    if (std::string_view(node.child("driver").attribute("type").value()) == "virtiofs") {
      config.folders.push_back({node.child("source").attribute("dir").value(), node.child("target").attribute("dir").value(),
                                static_cast<bool>(node.child("readonly"))});
    }
  }
  for (xml_node node : devices.children("hostdev")) {
    const xml_node source = node.child("source");
    if (std::string_view(node.attribute("type").value()) == "usb" && source.child("vendor")) {
      config.usbDevices.push_back({hexId(source.child("vendor").attribute("id").value()),
                                   hexId(source.child("product").attribute("id").value())});
    }
  }

  const xml_node gl = devices.child("graphics").child("gl");
  const xml_node model = devices.child("video").child("model");
  config.accel3d = std::string_view(gl.attribute("enable").value()) == "yes"
                && std::string_view(model.child("acceleration").attribute("accel3d").value()) == "yes";
  config.renderNode = gl.attribute("rendernode").value();
  config.venus = false;
  for (xml_node device : domain.child("qemu:override").children("qemu:device")) {
    for (xml_node property : device.child("qemu:frontend").children("qemu:property")) {
      if (std::string_view(property.attribute("name").value()) == "venus"
          && std::string_view(property.attribute("value").value()) == "true") {
        config.venus = config.accel3d;
      }
    }
  }
  config.sound = static_cast<bool>(devices.child("sound"));
  // Without a codec named, libvirt gives the card one that plays and records.
  config.microphone = false;
  if (const xml_node sound = devices.child("sound")) {
    config.microphone = !sound.child("codec");
    for (xml_node codec : sound.children("codec")) {
      const std::string_view type = codec.attribute("type").value();
      config.microphone = config.microphone || type == "duplex" || type == "micro";
    }
  }
  config.clipboard = static_cast<bool>(channel(devices, kVdagentChannel));
  config.guestAgent = static_cast<bool>(channel(devices, kAgentChannel));
  return config;
}

std::string coffret::formatPortForwards(const std::vector<PortForward>& forwards) {
  std::string text;
  for (const auto& forward : forwards) {
    text += std::format("{}{}:{}:{}", text.empty() ? "" : ", ", forward.udp ? "udp" : "tcp", forward.hostPort, forward.guestPort);
  }
  return text;
}

std::optional<std::vector<coffret::PortForward>> coffret::parsePortForwards(std::string_view text) {
  std::vector<PortForward> forwards;
  const auto port = [](std::string_view part) -> std::optional<std::uint16_t> {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    return error == std::errc() && end == part.data() + part.size() && value >= 1 && value <= 65535
               ? std::optional(static_cast<std::uint16_t>(value))
               : std::nullopt;
  };
  while (!text.empty()) {
    const auto comma = text.find(',');
    std::string_view item = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    while (!item.empty() && item.front() == ' ') {
      item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
      item.remove_suffix(1);
    }
    if (item.empty()) {
      continue;
    }
    const auto first = item.find(':'), second = item.find(':', first == std::string_view::npos ? first : first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view protocol = item.substr(0, first);
    const auto host = port(item.substr(first + 1, second - first - 1)), guest = port(item.substr(second + 1));
    if ((protocol != "tcp" && protocol != "udp") || !host || !guest) {
      return std::nullopt;
    }
    forwards.push_back({protocol == "udp", *host, *guest});
  }
  return forwards;
}
