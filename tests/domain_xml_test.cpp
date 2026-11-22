#include "check.h"
#include "vm/domain_xml.h"

#include <string>

namespace {
  bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

  coffret::VmConfig sample() {
    coffret::VmConfig config;
    config.name = "fedora";
    config.description = "Test <machine> & more";
    config.osId = "http://fedoraproject.org/fedora/45";
    config.memoryMiB = 8192;
    config.cpus = 4;
    config.nvramPath = "/vm/fedora/vars.qcow2";
    config.renderNode = "/dev/dri/renderD129";
    config.folder = "Linux";
    config.favourite = true;
    config.disks = {
        {.path = "/vm/fedora/disk.qcow2", .bootOrder = 1},
        {.path = "/iso/fedora.iso", .format = "raw", .bus = coffret::DiskBus::Sata, .cdrom = true, .readOnly = true, .bootOrder = 2},
    };
    coffret::Nic nic;
    nic.forwards = {{false, 2222, 22}, {true, 5353, 53}};
    config.nics = {nic};
    config.folders = {{"/home/flk/share", "share", false}};
    config.usbDevices = {{0x1915, 0x1025}};
    return config;
  }
} // namespace

int main() {
  const coffret::DomainEnvironment environment{.virtiofsd = "/usr/bin/virtiofsd", .uid = 1000, .gid = 100, .runtimeDir = "/run/user/1000"};
  const auto config = sample();
  const std::string xml = coffret::buildDomainXml(config, environment);

  CHECK(has(xml, "<domain type=\"kvm\">"));
  CHECK(has(xml, "<memory unit=\"MiB\">8192</memory>"));
  CHECK(has(xml, "<graphics type=\"dbus\" p2p=\"yes\">"));
  CHECK(has(xml, "<gl enable=\"yes\" rendernode=\"/dev/dri/renderD129\""));
  CHECK(has(xml, "<acceleration accel3d=\"yes\""));
  CHECK(has(xml, "<os firmware=\"efi\">"));
  CHECK(has(xml, "<nvram format=\"qcow2\">/vm/fedora/vars.qcow2</nvram>"));
  CHECK(has(xml, "<feature name=\"secure-boot\" enabled=\"no\""));
  CHECK(has(xml, "<backend type=\"passt\""));
  CHECK(has(xml, "<range start=\"2222\" to=\"22\""));
  CHECK(has(xml, "<audio id=\"1\" type=\"pipewire\" runtimeDir=\"/run/user/1000\""));
  CHECK(has(xml, "<driver type=\"virtiofs\""));
  CHECK(has(xml, "<binary path=\"/usr/bin/virtiofsd\""));
  CHECK(has(xml, "<uid start=\"0\" target=\"1000\" count=\"1\"") && has(xml, "<gid start=\"0\" target=\"100\" count=\"1\""));
  CHECK(has(xml, "<vendor id=\"0x1915\""));
  CHECK(has(xml, "<target dev=\"vda\" bus=\"virtio\""));
  CHECK(has(xml, "<target dev=\"sda\" bus=\"sata\""));
  CHECK(has(xml, "Test &lt;machine&gt; &amp; more"));
  CHECK(!has(xml, "<tpm"));
  CHECK(has(xml, "<codec type=\"output\""));
  CHECK(!has(xml, "qemu:override"));
  CHECK(!has(xml, "<hyperv"));

  // What was written is read back as it was.
  const auto read = coffret::readDomainXml(xml);
  CHECK(read.has_value());
  CHECK(*read == config);

  // Windows 11: secure boot, a TPM and the Hyper-V helpers.
  auto windows = config;
  windows.kind = coffret::GuestKind::Windows;
  windows.firmware = coffret::Firmware::UefiSecureBoot;
  windows.tpm = true;
  const std::string windowsXml = coffret::buildDomainXml(windows, environment);
  CHECK(has(windowsXml, "<feature name=\"secure-boot\" enabled=\"yes\""));
  CHECK(has(windowsXml, "<feature name=\"enrolled-keys\" enabled=\"yes\""));
  CHECK(has(windowsXml, "<smm state=\"on\""));
  CHECK(has(windowsXml, "<tpm model=\"tpm-crb\">"));
  CHECK(has(windowsXml, "<hyperv mode=\"custom\">"));
  CHECK(has(windowsXml, "<clock offset=\"localtime\">"));
  CHECK((*coffret::readDomainXml(windowsXml) == windows));

  // Vulkan goes to QEMU's device by its alias, and away again.
  auto venus = config;
  venus.venus = true;
  const std::string venusXml = coffret::buildDomainXml(venus, environment);
  CHECK(has(venusXml, "xmlns:qemu=\"http://libvirt.org/schemas/domain/qemu/1.0\""));
  CHECK(has(venusXml, "<qemu:device alias=\"ua-coffret-video\">"));
  CHECK(has(venusXml, "<qemu:property name=\"venus\" type=\"bool\" value=\"true\""));
  CHECK(has(venusXml, "<alias name=\"ua-coffret-video\""));
  CHECK((*coffret::readDomainXml(venusXml) == venus));
  const auto noVenus = coffret::updateDomainXml(venusXml, config, environment);
  CHECK(noVenus.has_value() && !has(*noVenus, "qemu:override") && !has(*noVenus, "blob="));

  // An update keeps what libvirt added and what coffret does not manage.
  const std::string defined = R"(<domain type='kvm'>
  <name>fedora</name>
  <uuid>11111111-2222-3333-4444-555555555555</uuid>
  <memory unit='KiB'>8388608</memory>
  <currentMemory unit='KiB'>8388608</currentMemory>
  <vcpu placement='static'>4</vcpu>
  <os firmware='efi'>
    <type arch='x86_64' machine='pc-q35-11.1'>hvm</type>
    <firmware><feature enabled='no' name='secure-boot'/><feature enabled='no' name='enrolled-keys'/></firmware>
    <loader readonly='yes' type='pflash' format='qcow2'>/usr/share/qemu/edk2-x86_64-code.fd</loader>
    <nvram format='qcow2'>/vm/fedora/vars.qcow2</nvram>
  </os>
  <features><acpi/><apic/></features>
  <cpu mode='host-passthrough'/>
  <devices>
    <emulator>/usr/bin/qemu-system-x86_64</emulator>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2'/>
      <source file='/vm/fedora/disk.qcow2'/>
      <target dev='vda' bus='virtio'/>
      <boot order='1'/>
      <address type='pci' domain='0x0000' bus='0x04' slot='0x00' function='0x0'/>
    </disk>
    <interface type='user'>
      <mac address='52:54:00:12:34:56'/>
      <model type='virtio'/>
      <backend type='passt'/>
      <address type='pci' domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>
    </interface>
    <watchdog model='itco' action='reset'/>
    <graphics type='dbus' p2p='yes'/>
    <video><model type='virtio' heads='1' primary='yes'/></video>
  </devices>
</domain>)";
  auto existing = coffret::readDomainXml(defined);
  CHECK(existing.has_value());
  CHECK(existing->uuid == "11111111-2222-3333-4444-555555555555");
  CHECK(existing->memoryMiB == 8192 && existing->cpus == 4);
  CHECK(existing->firmware == coffret::Firmware::Uefi);
  CHECK(existing->kind == coffret::GuestKind::Other);
  CHECK(existing->disks.size() == 1 && existing->nics.size() == 1);
  CHECK(existing->nics[0].mac == "52:54:00:12:34:56");
  CHECK(!existing->accel3d && !existing->sound && !existing->clipboard);

  existing->memoryMiB = 4096;
  existing->disks.push_back({.path = "/vm/fedora/data.qcow2"});
  existing->disks.push_back({.path = "/iso/tools.iso", .format = "raw", .bus = coffret::DiskBus::Sata, .cdrom = true, .readOnly = true});
  existing->sound = true;
  const auto updated = coffret::updateDomainXml(defined, *existing, environment);
  CHECK(updated.has_value());
  CHECK(has(*updated, "<memory unit=\"MiB\">4096</memory>"));
  CHECK(!has(*updated, "currentMemory"));
  CHECK(has(*updated, "<watchdog model=\"itco\""));
  CHECK(has(*updated, "<emulator>/usr/bin/qemu-system-x86_64</emulator>"));
  CHECK(has(*updated, "machine=\"pc-q35-11.1\""));
  // The old disk and the network card keep their addresses and the card its MAC.
  CHECK(has(*updated, "bus=\"0x04\""));
  CHECK(has(*updated, "bus=\"0x01\""));
  CHECK(has(*updated, "52:54:00:12:34:56"));
  // The new disk gets the next free name, the loader of the old firmware setting goes.
  CHECK(has(*updated, "<target dev=\"vdb\" bus=\"virtio\""));
  CHECK(has(*updated, "<target dev=\"sda\" bus=\"sata\""));
  CHECK(!has(*updated, "<loader"));
  CHECK((*coffret::readDomainXml(*updated) == *existing));

  // Without 3D the device libvirt chose for 3D goes too, or libvirt refuses the machine.
  const std::string chosen = R"(<domain type='kvm'><name>a</name><devices>
    <graphics type='dbus' p2p='yes'><gl enable='yes' rendernode='/dev/dri/renderD129'/></graphics>
    <video><model type='virtio' heads='1' primary='yes' device='virtio-vga-gl'><acceleration accel3d='yes'/></model></video>
  </devices></domain>)";
  auto flat = *coffret::readDomainXml(chosen);
  CHECK(flat.accel3d);
  flat.accel3d = false;
  const auto without = coffret::updateDomainXml(chosen, flat, environment);
  CHECK(without.has_value() && !has(*without, "virtio-vga-gl") && !has(*without, "<gl") && !has(*without, "accel3d"));

  // Taking a disk away removes its element and nothing else.
  existing->disks.erase(existing->disks.begin() + 1);
  const auto fewer = coffret::updateDomainXml(*updated, *existing, environment);
  CHECK(fewer.has_value() && !has(*fewer, "data.qcow2") && has(*fewer, "tools.iso") && has(*fewer, "bus=\"0x04\""));

  // A firmware named outright: no picking by libvirt, and the template for new variables.
  auto named = config;
  named.firmwareCode = "/fw/code.qcow2";
  named.nvramTemplate = "/fw/vars.qcow2";
  const std::string namedXml = coffret::buildDomainXml(named, environment);
  CHECK(has(namedXml, "<loader readonly=\"yes\" secure=\"no\" type=\"pflash\" format=\"qcow2\">/fw/code.qcow2</loader>"));
  CHECK(has(namedXml, "<nvram template=\"/fw/vars.qcow2\" templateFormat=\"qcow2\" format=\"qcow2\">/vm/fedora/vars.qcow2</nvram>"));
  CHECK(!has(namedXml, "firmware="));
  CHECK((*coffret::readDomainXml(namedXml) == named));
  named.firmware = coffret::Firmware::UefiSecureBoot;
  const auto secured = coffret::updateDomainXml(namedXml, named, environment);
  CHECK(secured.has_value() && has(*secured, "secure=\"yes\"") && has(*secured, "<smm state=\"on\""));
  CHECK((*coffret::readDomainXml(*secured) == named));
  // Back to a firmware of libvirt's choice.
  const auto picked = coffret::updateDomainXml(namedXml, config, environment);
  CHECK(picked.has_value() && has(*picked, "<os firmware=\"efi\">") && !has(*picked, "<loader"));

  // The microphone is asked for; a sound card libvirt set up by itself has one.
  auto listening = config;
  listening.microphone = true;
  const std::string listeningXml = coffret::buildDomainXml(listening, environment);
  CHECK(has(listeningXml, "<codec type=\"duplex\"") && !has(listeningXml, "type=\"output\""));
  CHECK((*coffret::readDomainXml(listeningXml) == listening));
  CHECK(coffret::readDomainXml("<domain><devices><sound model='ich9'/></devices></domain>")->microphone);

  // BIOS has neither firmware features nor variables.
  auto bios = config;
  bios.firmware = coffret::Firmware::Bios;
  bios.nvramPath.clear();
  const std::string biosXml = coffret::buildDomainXml(bios, environment);
  CHECK(!has(biosXml, "firmware") && !has(biosXml, "nvram"));
  CHECK((*coffret::readDomainXml(biosXml) == bios));

  // A bridged card has a source and no passt.
  auto bridged = config;
  bridged.nics = {{.kind = coffret::NetworkKind::Bridge, .bridge = "br0", .mac = "52:54:00:aa:bb:cc", .forwards = {}}};
  const std::string bridgedXml = coffret::buildDomainXml(bridged, environment);
  CHECK(has(bridgedXml, "<interface type=\"bridge\">") && has(bridgedXml, "<source bridge=\"br0\""));
  CHECK(!has(bridgedXml, "type=\"passt\"") && !has(bridgedXml, "portForward"));
  CHECK((*coffret::readDomainXml(bridgedXml) == bridged));

  const auto forwards = coffret::parsePortForwards(" tcp:2222:22 ,udp:5353:53,");
  CHECK(forwards.has_value() && forwards->size() == 2);
  CHECK(((*forwards)[1] == coffret::PortForward{true, 5353, 53}));
  CHECK(coffret::formatPortForwards(*forwards) == "tcp:2222:22, udp:5353:53");
  CHECK(coffret::parsePortForwards("")->empty());
  CHECK(!coffret::parsePortForwards("2222:22").has_value());
  CHECK(!coffret::parsePortForwards("tcp:0:22").has_value());
  CHECK(!coffret::parsePortForwards("tcp:70000:22").has_value());
  CHECK(!coffret::parsePortForwards("icmp:1:1").has_value());

  CHECK(!coffret::readDomainXml("<network/>").has_value());
  CHECK(!coffret::readDomainXml("not xml <").has_value());
  CHECK(!coffret::updateDomainXml("<x/>", config, environment).has_value());
  return 0;
}
