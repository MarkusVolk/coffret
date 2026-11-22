#include "ui/machine_page.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "vm/host_info.h"

#include <algorithm>
#include <ctime>
#include <format>

namespace {
  constexpr float kNameWidth = 170.0F;

  std::unique_ptr<Label> text(std::string value, ColorRole role, float size = Style::fontSizeBody, Label** out = nullptr) {
    return ui::label({
        .out = out,
        .text = std::move(value),
        .fontSize = size,
        .color = colorSpecFromRole(role),
        .configure = [](Label& label) { label.setUseMarkup(false); },
    });
  }

  std::unique_ptr<Label> heading(std::string value) {
    return ui::label({
        .text = std::move(value),
        .fontSize = Style::fontSizeTitle,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .configure = [](Label& label) { label.setUseMarkup(false); },
    });
  }

  std::unique_ptr<Flex> field(const std::string& name, std::unique_ptr<Node> control) {
    auto label = text(name, ColorRole::OnSurfaceVariant);
    label->setMinWidth(kNameWidth);
    return ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd}, std::move(label), std::move(control));
  }

  void clear(Flex* box) {
    while (!box->children().empty()) {
      box->removeChild(box->children().back().get());
    }
  }

  std::string when(std::int64_t seconds) {
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_r(&time, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
  }

  coffret::Disk* cdromOf(coffret::VmConfig& config) {
    const auto it = std::ranges::find_if(config.disks, [](const coffret::Disk& disk) { return disk.cdrom; });
    return it == config.disks.end() ? nullptr : &*it;
  }
} // namespace

coffret::MachinePage::MachinePage(Callbacks callbacks) : m_callbacks(std::move(callbacks)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFlexGrow(1.0F);
  ScrollView* scroll = nullptr;
  addChild(ui::scrollView({
      .out = &scroll,
      .state = &m_scroll,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  }));
  Flex* page = scroll->content();
  page->setDirection(FlexDirection::Vertical);
  page->setAlign(FlexAlign::Stretch);
  page->setGap(Style::spaceSm);
  page->setPadding(Style::spaceLg);

  auto name = text("", ColorRole::OnSurface, Style::fontSizeHeader, &m_name);
  name->setFontWeight(FontWeight::Bold);
  page->addChild(std::move(name));
  page->addChild(text("", ColorRole::OnSurfaceVariant, Style::fontSizeBody, &m_state));
  page->addChild(ui::separator({.spacing = Style::spaceSm}));

  const auto edited = [this] { m_callbacks.changed(); };
  page->addChild(heading("Hardware"));
  page->addChild(field(
      "Memory",
      ui::stepper({
          .out = &m_memory,
          .minValue = 512,
          .maxValue = static_cast<int>(std::max(1024U, hostMemoryMiB())),
          .step = 1024,
          .value = 4096,
          .valueSuffix = " MiB",
          .onValueChanged =
              [this, edited](int value) {
                m_edit.memoryMiB = static_cast<unsigned>(value);
                edited();
              },
      })
  ));
  page->addChild(field(
      "Processors",
      ui::stepper({
          .out = &m_cpus,
          .minValue = 1,
          .maxValue = static_cast<int>(hostProcessors()),
          .value = 2,
          .onValueChanged =
              [this, edited](int value) {
                m_edit.cpus = static_cast<unsigned>(value);
                edited();
              },
      })
  ));
  page->addChild(field(
      "3D acceleration",
      ui::toggle({
          .out = &m_accel,
          .onChange =
              [this, edited](bool on) {
                m_edit.accel3d = on;
                if (on && m_edit.renderNode.empty()) {
                  m_edit.renderNode = defaultRenderNode();
                }
                m_edit.venus = m_edit.venus && on;
                edited();
              },
      })
  ));
  page->addChild(field(
      "Sound",
      ui::toggle({
          .out = &m_sound,
          .onChange =
              [this, edited](bool on) {
                m_edit.sound = on;
                edited();
              },
      })
  ));
  page->addChild(field(
      "Microphone",
      ui::toggle({
          .out = &m_microphone,
          .onChange =
              [this, edited](bool on) {
                m_edit.microphone = on;
                edited();
              },
      })
  ));
  page->addChild(field(
      "Shared clipboard",
      ui::toggle({
          .out = &m_clipboard,
          .onChange =
              [this, edited](bool on) {
                m_edit.clipboard = on;
                edited();
              },
      })
  ));
  page->addChild(field(
      "TPM 2.0",
      ui::toggle({
          .out = &m_tpm,
          .onChange =
              [this, edited](bool on) {
                m_edit.tpm = on;
                edited();
              },
      })
  ));
  page->addChild(field(
      "Secure boot firmware",
      ui::toggle({
          .out = &m_secureBoot,
          .onChange =
              [this, edited](bool on) {
                if (m_edit.firmware != Firmware::Bios) {
                  m_edit.firmware = on ? Firmware::UefiSecureBoot : Firmware::Uefi;
                }
                edited();
              },
      })
  ));
  page->addChild(field(
      "CD/DVD image",
      ui::input({
          .out = &m_cdrom,
          .placeholder = "No disc in the drive",
          .clearButtonEnabled = true,
          .flexGrow = 1.0F,
          .onChange =
              [this, edited](const std::string& path) {
                Disk* drive = cdromOf(m_edit);
                if (drive) {
                  drive->path = path;
                } else if (!path.empty()) {
                  m_edit.disks.push_back({.path = path, .format = "raw", .bus = DiskBus::Sata, .cdrom = true, .readOnly = true});
                }
                edited();
              },
      })
  ));
  page->addChild(field(
      "Port forwards",
      ui::input({
          .out = &m_forwards,
          .placeholder = "tcp:2222:22, udp:5353:53  (protocol : port here : port in the guest)",
          .flexGrow = 1.0F,
          .onChange =
              [this, edited](const std::string& value) {
                // What cannot be read yet leaves the settings as they are.
                if (auto forwards = parsePortForwards(value); forwards && !m_edit.nics.empty()) {
                  m_edit.nics.front().forwards = std::move(*forwards);
                }
                edited();
              },
      })
  ));
  page->addChild(field("", text("", ColorRole::OnSurfaceVariant, Style::fontSizeCaption, &m_forwardsNote)));
  page->addChild(ui::column({.out = &m_folderRows, .align = FlexAlign::Stretch, .gap = Style::spaceXs}));
  page->addChild(field(
      "Shared folder",
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .flexGrow = 1.0F},
          ui::input({.out = &m_folderPath, .placeholder = "A folder of this computer for the guest", .flexGrow = 1.0F}),
          ui::button({
              .text = "Add",
              .glyph = "plus",
              .variant = ButtonVariant::Outline,
              .onClick =
                  [this] {
                    std::string path = m_folderPath->value();
                    while (path.size() > 1 && path.back() == '/') {
                      path.pop_back();
                    }
                    if (path.empty() || std::ranges::any_of(m_edit.folders, [&](const SharedFolder& folder) { return folder.path == path; })) {
                      return;
                    }
                    // The guest mounts it by this name: the folder's own, made unique.
                    std::string base = path.substr(path.rfind('/') + 1);
                    if (base.empty()) {
                      base = "share";
                    }
                    std::string tag = base;
                    for (unsigned number = 2; std::ranges::any_of(m_edit.folders, [&](const SharedFolder& folder) { return folder.tag == tag; }); ++number) {
                      tag = std::format("{}-{}", base, number);
                    }
                    m_edit.folders.push_back({path, tag, false});
                    m_folderPath->setValue("");
                    m_foldersStale = true;
                    m_callbacks.changed();
                  },
          })
      )
  ));
  page->addChild(ui::column({.out = &m_facts, .align = FlexAlign::Stretch, .gap = Style::spaceSm}));
  page->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd},
      ui::button({
          .out = &m_apply,
          .text = "Apply",
          .variant = ButtonVariant::Primary,
          .onClick = [this] { m_callbacks.apply(m_edit); },
      }),
      text("", ColorRole::OnSurfaceVariant, Style::fontSizeCaption, &m_applyNote)
  ));

  page->addChild(ui::separator({.spacing = Style::spaceSm}));
  page->addChild(heading("USB devices"));
  page->addChild(text("", ColorRole::OnSurfaceVariant, Style::fontSizeCaption, &m_usbNote));
  page->addChild(ui::column({.out = &m_usbRows, .align = FlexAlign::Stretch, .gap = 2.0F}));

  page->addChild(ui::separator({.spacing = Style::spaceSm}));
  page->addChild(heading("Snapshots"));
  page->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      ui::input({.out = &m_snapshotName, .placeholder = "Name of the snapshot", .width = 320.0F}),
      ui::button({
          .out = &m_take,
          .text = "Take snapshot",
          .glyph = "camera",
          .variant = ButtonVariant::Outline,
          .onClick =
              [this] {
                const std::string name = m_snapshotName->value().empty() ? nextSnapshotName(m_snapshots) : m_snapshotName->value();
                m_snapshotName->setValue("");
                m_callbacks.takeSnapshot(name);
              },
      })
  ));
  page->addChild(text("", ColorRole::OnSurfaceVariant, Style::fontSizeCaption, &m_snapshotNote));
  page->addChild(ui::column({.out = &m_snapshotRows, .align = FlexAlign::Stretch, .gap = 2.0F}));

  page->addChild(ui::separator({.spacing = Style::spaceSm}));
  page->addChild(heading("Clone"));
  page->addChild(field("Name", ui::input({.out = &m_cloneName, .placeholder = "Name of the new machine", .width = 320.0F})));
  page->addChild(field(
      "Linked clone",
      ui::toggle({
          .out = &m_cloneLinked,
          .onChange =
              [this](bool on) {
                m_linked = on;
                m_callbacks.changed();
              },
      })
  ));
  page->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd},
      ui::button({
          .out = &m_clone,
          .text = "Clone",
          .glyph = "copy",
          .variant = ButtonVariant::Outline,
          .onClick =
              [this] {
                const std::string name = m_cloneName->value().empty() ? m_machine.name + " clone" : m_cloneName->value();
                m_callbacks.clone(name, m_linked);
              },
      }),
      text("", ColorRole::OnSurfaceVariant, Style::fontSizeCaption, &m_cloneNote)
  ));

  page->addChild(ui::separator({.spacing = Style::spaceSm}));
  page->addChild(heading("Remove"));
  page->addChild(ui::row({.out = &m_removeRow, .align = FlexAlign::Center, .gap = Style::spaceSm}));
}

void coffret::MachinePage::rebuildFolders() {
  clear(m_folderRows);
  for (const auto& folder : m_edit.folders) {
    m_folderRows->addChild(field(
        "Shared folder",
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .flexGrow = 1.0F},
            text(folder.path, ColorRole::OnSurface),
            text(std::format("in the guest: mount -t virtiofs {} /mnt", folder.tag), ColorRole::OnSurfaceVariant, Style::fontSizeCaption),
            ui::button({
                .glyph = "x",
                .variant = ButtonVariant::Ghost,
                .tooltip = "Stop sharing the folder",
                .onClick =
                    [this, tag = folder.tag] {
                      std::erase_if(m_edit.folders, [&tag](const SharedFolder& other) { return other.tag == tag; });
                      m_foldersStale = true;
                      m_callbacks.changed();
                    },
            })
        )
    ));
  }
  m_foldersStale = false;
}

void coffret::MachinePage::setConnectedUsb(std::vector<HostUsbDevice> devices) {
  m_usbConnected = std::move(devices);
  m_usbStale = true;
}

void coffret::MachinePage::rebuildUsb() {
  clear(m_usbRows);
  const bool active = isActive(m_machine.state);
  for (const auto& device : m_usbDevices) {
    const bool connected = std::ranges::find(m_usbConnected, device) != m_usbConnected.end();
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    row->addChild(ui::glyph({.glyph = "usb", .glyphSize = 16.0F, .color = colorSpecFromRole(connected ? ColorRole::Primary : ColorRole::OnSurfaceVariant)}));
    auto name = text(device.name, ColorRole::OnSurface);
    name->setMinWidth(320.0F);
    row->addChild(std::move(name));
    auto ids = text(std::format("{:04x}:{:04x}", device.vendor, device.product), ColorRole::OnSurfaceVariant, Style::fontSizeCaption);
    ids->setFlexGrow(1.0F);
    row->addChild(std::move(ids));
    row->addChild(ui::button({
        .text = connected ? "Disconnect" : "Connect",
        .enabled = active,
        .variant = connected ? ButtonVariant::Secondary : ButtonVariant::Outline,
        .onClick = [this, device, connected] { m_callbacks.usb(device, !connected); },
    }));
    m_usbRows->addChild(std::move(row));
  }
  m_usbStale = false;
}

void coffret::MachinePage::rebuildRemove() {
  clear(m_removeRow);
  const bool active = isActive(m_machine.state);
  if (!m_removeAsked) {
    m_removeRow->addChild(ui::button({
        .text = "Remove this machine …",
        .glyph = "trash",
        .enabled = !active,
        .variant = ButtonVariant::Outline,
        .onClick =
            [this] {
              m_removeAsked = true;
              m_removeStale = true;
              m_callbacks.changed();
            },
    }));
    if (active) {
      m_removeRow->addChild(text("A machine can be removed while it is off.", ColorRole::OnSurfaceVariant, Style::fontSizeCaption));
    }
  } else {
    const auto answer = [this](bool remove, bool deleteFiles) {
      return [this, remove, deleteFiles] {
        m_removeAsked = false;
        m_removeStale = true;
        if (remove) {
          m_callbacks.remove(deleteFiles);
        }
        m_callbacks.changed();
      };
    };
    m_removeRow->addChild(ui::button({.text = "Delete with its disks", .glyph = "trash", .variant = ButtonVariant::Primary, .onClick = answer(true, true)}));
    m_removeRow->addChild(ui::button({.text = "Only out of the library", .variant = ButtonVariant::Outline, .onClick = answer(true, false)}));
    m_removeRow->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = answer(false, false)}));
    m_removeRow->addChild(text("Deleting cannot be undone.", ColorRole::OnSurfaceVariant, Style::fontSizeCaption));
  }
  m_removeStale = false;
}

void coffret::MachinePage::setMachine(const Machine& machine) {
  const bool other = machine.uuid != m_machine.uuid;
  // Settings that changed underneath replace what is shown, unless the user is in the middle of changing them.
  const bool refill = other || (machine.config != m_machine.config && m_edit == m_machine.config);
  m_removeStale = m_removeStale || other || isActive(machine.state) != isActive(m_machine.state);
  m_usbStale = m_usbStale || other || isActive(machine.state) != isActive(m_machine.state);
  if (other || !isActive(machine.state)) {
    m_usbConnected.clear();
  }
  if (other) {
    m_removeAsked = false;
    m_askName.clear();
    m_snapshots.clear();
    m_snapshotsStale = true;
  }
  m_factsStale = m_factsStale || refill || machine.state != m_machine.state;
  m_machine = machine;
  if (refill) {
    m_edit = machine.config;
    m_foldersStale = true;
    fill();
  }
}

void coffret::MachinePage::setSnapshots(std::vector<Snapshot> snapshots) {
  m_snapshots = std::move(snapshots);
  m_askName.clear();
  m_snapshotsStale = true;
}

void coffret::MachinePage::setBusy(bool busy) { m_busy = busy; }

void coffret::MachinePage::fill() {
  // A machine with less than the stepper's smallest step keeps what it has until it is changed.
  m_memory->setValue(static_cast<int>(m_edit.memoryMiB));
  m_cpus->setValue(static_cast<int>(m_edit.cpus));
  m_accel->setCheckedImmediate(m_edit.accel3d);
  m_sound->setCheckedImmediate(m_edit.sound);
  m_microphone->setCheckedImmediate(m_edit.microphone);
  m_tpm->setCheckedImmediate(m_edit.tpm);
  m_secureBoot->setCheckedImmediate(m_edit.firmware == Firmware::UefiSecureBoot);
  m_clipboard->setCheckedImmediate(m_edit.clipboard);
  m_forwards->setValue(m_edit.nics.empty() ? "" : formatPortForwards(m_edit.nics.front().forwards));
  const Disk* drive = cdromOf(m_edit);
  m_cdrom->setValue(drive ? drive->path : "");
}

void coffret::MachinePage::rebuildFacts() {
  clear(m_facts);
  const VmConfig& config = m_machine.config;
  const auto fact = [this](const std::string& name, std::string value) {
    m_facts->addChild(field(name, text(std::move(value), ColorRole::OnSurface)));
  };
  for (const auto& disk : config.disks) {
    if (!disk.cdrom) {
      fact("Hard disk", std::format("{} ({})", disk.path, toString(disk.bus)));
    }
  }
  for (const auto& nic : config.nics) {
    fact("Network", nic.kind == NetworkKind::Bridge ? std::format("Bridged ({})", nic.bridge) : "NAT");
  }
  fact(
      "Firmware", config.firmware == Firmware::Bios             ? "BIOS"
                  : config.firmware == Firmware::UefiSecureBoot ? "UEFI with secure boot"
                                                                : "UEFI"
  );
  if (config.tpm) {
    fact("TPM", "2.0");
  }
  m_factsStale = false;
}

void coffret::MachinePage::rebuildSnapshots() {
  clear(m_snapshotRows);
  const auto rows = snapshotRows(m_snapshots);
  const auto here = [this](unsigned depth) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    row->setPadding(2.0F, 0.0F, 2.0F, static_cast<float>(depth) * 24.0F);
    row->addChild(ui::glyph({.glyph = "map-pin", .glyphSize = 16.0F, .color = colorSpecFromRole(ColorRole::Primary)}));
    row->addChild(text("You are here", ColorRole::Primary));
    m_snapshotRows->addChild(std::move(row));
  };
  for (const auto& entry : rows) {
    const Snapshot& snapshot = entry.snapshot;
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    row->setPadding(2.0F, 0.0F, 2.0F, static_cast<float>(entry.depth) * 24.0F);
    row->addChild(ui::glyph({.glyph = "camera", .glyphSize = 16.0F, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    auto name = text(snapshot.name, ColorRole::OnSurface);
    name->setMinWidth(220.0F);
    row->addChild(std::move(name));
    auto detail = text(
        std::format("{}{}", when(snapshot.created), snapshot.running ? "   ·   running" : ""), ColorRole::OnSurfaceVariant,
        Style::fontSizeCaption
    );
    detail->setFlexGrow(1.0F);
    row->addChild(std::move(detail));
    if (m_askName == snapshot.name) {
      const bool revert = m_askWhat == "revert";
      row->addChild(text(revert ? "What happened since is lost." : "The snapshot is gone for good.", ColorRole::OnSurfaceVariant, Style::fontSizeCaption));
      row->addChild(ui::button({
          .text = revert ? "Go back" : "Delete",
          .variant = ButtonVariant::Primary,
          .onClick =
              [this, revert, name = snapshot.name] {
                m_askName.clear();
                m_snapshotsStale = true;
                (revert ? m_callbacks.revertTo : m_callbacks.deleteSnapshot)(name);
              },
      }));
      row->addChild(ui::button({
          .text = "Cancel",
          .variant = ButtonVariant::Ghost,
          .onClick =
              [this] {
                m_askName.clear();
                m_snapshotsStale = true;
                m_callbacks.changed();
              },
      }));
    } else {
      const auto ask = [this, name = snapshot.name](const char* what) {
        return [this, name, what] {
          m_askName = name;
          m_askWhat = what;
          m_snapshotsStale = true;
          m_callbacks.changed();
        };
      };
      row->addChild(ui::button({.text = "Go back", .glyph = "arrow-back-up", .enabled = !m_busy, .variant = ButtonVariant::Ghost, .onClick = ask("revert")}));
      row->addChild(ui::button({.glyph = "trash", .enabled = !m_busy, .variant = ButtonVariant::Ghost, .tooltip = "Delete the snapshot", .onClick = ask("delete")}));
    }
    m_snapshotRows->addChild(std::move(row));
    if (entry.hereAfter) {
      here(entry.depth + 1);
    }
  }
  m_snapshotsStale = false;
}

void coffret::MachinePage::sync() {
  m_name->setText(m_machine.name);
  m_state->setText(toString(m_machine.state));
  if (m_factsStale) {
    rebuildFacts();
  }
  if (m_snapshotsStale) {
    rebuildSnapshots();
  }
  if (m_removeStale) {
    rebuildRemove();
  }
  if (m_foldersStale) {
    rebuildFolders();
  }
  // Devices come and go; looking every two seconds while the page is up is cheap.
  if (const auto now = std::chrono::steady_clock::now(); now - m_usbScanned > std::chrono::seconds(2)) {
    m_usbScanned = now;
    if (auto devices = listUsbDevices(); devices != m_usbDevices) {
      m_usbDevices = std::move(devices);
      m_usbStale = true;
    }
  }
  if (m_usbStale) {
    rebuildUsb();
  }
  m_usbNote->setText(
      !isActive(m_machine.state) ? "A device of this computer can be given to the machine while it runs; this computer then no longer has it."
      : m_usbDevices.empty()     ? "No devices are plugged in."
                                 : "A connected device belongs to the machine until it is disconnected or the machine stops."
  );
  const bool active = isActive(m_machine.state);
  m_microphone->setEnabled(m_edit.sound);
  m_secureBoot->setEnabled(m_edit.firmware != Firmware::Bios);
  const bool nat = !m_edit.nics.empty() && m_edit.nics.front().kind == NetworkKind::User;
  m_forwardsNote->setText(
      !nat                                         ? "Ports are forwarded for a machine behind NAT."
      : !parsePortForwards(m_forwards->value())    ? "Not readable yet: protocol, the port on this computer, the port in the guest."
                                                   : "Connections to these ports of this computer reach the guest."
  );
  const bool dirty = m_edit != m_machine.config;
  m_apply->setEnabled(dirty);
  m_applyNote->setText(dirty && active ? "Takes effect when the machine is started the next time." : "");
  // QEMU cannot save a running machine whose picture is drawn by the host's GPU.
  const bool blocked = active && m_machine.config.accel3d;
  // A linked clone turns the disks into a base both machines build on; snapshots would stay behind in it.
  const bool canLink = m_snapshots.empty();
  if (!canLink && m_linked) {
    m_linked = false;
    m_cloneLinked->setCheckedImmediate(false);
  }
  m_cloneLinked->setEnabled(canLink);
  m_clone->setEnabled(!m_busy && !active);
  m_clone->setText(m_busy ? "Working …" : "Clone");
  m_cloneNote->setText(
      active      ? "A machine can be cloned while it is off."
      : !canLink  ? "A full copy that stands alone. A linked clone is possible for a machine without snapshots."
      : m_linked  ? "Made at once and small, but it needs this machine's folder for as long as it lives."
                  : "A full copy that stands alone."
  );
  m_take->setEnabled(!m_busy && !blocked);
  m_take->setText(m_busy ? "Working …" : "Take snapshot");
  m_snapshotNote->setText(
      blocked            ? "With 3D acceleration a snapshot can only be taken while the machine is off."
      : active           ? "A snapshot of the running machine keeps its memory too and takes a moment."
      : m_snapshots.empty() ? "A snapshot keeps the machine's disks as they are now, to go back to later."
                            : ""
  );
}
