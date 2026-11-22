#include "check.h"
#include "vm/clone_plan.h"

namespace {
  coffret::VmConfig source() {
    coffret::VmConfig config;
    config.name = "fedora";
    config.uuid = "11111111-2222-3333-4444-555555555555";
    config.nvramPath = "/vm/fedora/vars.qcow2";
    config.disks = {
        {.path = "/vm/fedora/disk.qcow2", .bootOrder = 1},
        {.path = "/data/disk.qcow2"},
        {.path = "/iso/fedora.iso", .format = "raw", .bus = coffret::DiskBus::Sata, .cdrom = true, .readOnly = true},
    };
    coffret::Nic nic;
    nic.mac = "52:54:00:12:34:56";
    config.nics = {nic};
    config.usbDevices = {{0x1915, 0x1025}};
    return config;
  }
} // namespace

int main() {
  const auto full = coffret::planClone(source(), "fedora 2", "/vm/fedora-2", false, true);
  CHECK(full.has_value());
  CHECK(full->clone.name == "fedora 2" && full->clone.uuid.empty());
  CHECK(full->clone.nics[0].mac.empty() && full->clone.usbDevices.empty());
  CHECK(full->clone.disks[0].path == "/vm/fedora-2/disk.qcow2" && full->clone.disks[0].bootOrder == 1);
  CHECK(full->clone.disks[1].path == "/vm/fedora-2/disk-2.qcow2");
  // The CD stays the one it was.
  CHECK(full->clone.disks[2].path == "/iso/fedora.iso");
  CHECK(full->clone.nvramPath == "/vm/fedora-2/vars.qcow2");
  CHECK(full->renames.empty() && full->readOnly.empty());
  CHECK(full->commands.size() == 2);
  CHECK((full->commands[0] == std::vector<std::string>{"qemu-img", "convert", "-O", "qcow2", "/vm/fedora/disk.qcow2", "/vm/fedora-2/disk.qcow2"}));
  CHECK((full->copies == std::vector<std::pair<std::string, std::string>>{{"/vm/fedora/vars.qcow2", "/vm/fedora-2/vars.qcow2"}}));

  const auto linked = coffret::planClone(source(), "fedora 2", "/vm/fedora-2", true, false);
  CHECK(linked.has_value());
  CHECK(linked->renames.size() == 2);
  CHECK((linked->renames[0] == std::pair<std::string, std::string>{"/vm/fedora/disk.qcow2", "/vm/fedora/disk.base.qcow2"}));
  CHECK(linked->readOnly.size() == 2 && linked->readOnly[1] == "/data/disk.base.qcow2");
  // The source goes on at its old path, as changes to the base; so does the clone at its own.
  CHECK(linked->commands.size() == 4);
  CHECK((linked->commands[0] == std::vector<std::string>{"qemu-img", "create", "-q", "-f", "qcow2", "-b", "/vm/fedora/disk.base.qcow2", "-F", "qcow2", "/vm/fedora/disk.qcow2"}));
  CHECK(linked->commands[1].back() == "/vm/fedora-2/disk.qcow2");
  CHECK(linked->clone.disks[0].path == "/vm/fedora-2/disk.qcow2");

  CHECK(!coffret::planClone(source(), "fedora 2", "/vm/fedora-2", true, true).has_value());
  CHECK(!coffret::planClone(source(), "fedora", "/vm/x", false, false).has_value());
  CHECK(!coffret::planClone(source(), "", "/vm/x", false, false).has_value());
  auto raw = source();
  raw.disks[0].format = "raw";
  CHECK(!coffret::planClone(raw, "fedora 2", "/vm/fedora-2", true, false).has_value());
  const auto rawFull = coffret::planClone(raw, "fedora 2", "/vm/fedora-2", false, false);
  CHECK(rawFull.has_value() && rawFull->clone.disks[0].format == "qcow2");
  return 0;
}
