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
}

void coffret::MachinePage::setMachine(const Machine& machine) {
  const bool other = machine.uuid != m_machine.uuid;
  // Settings that changed underneath replace what is shown, unless the user is in the middle of changing them.
  const bool refill = other || (machine.config != m_machine.config && m_edit == m_machine.config);
  if (other) {
    m_askName.clear();
    m_snapshots.clear();
    m_snapshotsStale = true;
  }
  m_factsStale = m_factsStale || refill || machine.state != m_machine.state;
  m_machine = machine;
  if (refill) {
    m_edit = machine.config;
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
  m_clipboard->setCheckedImmediate(m_edit.clipboard);
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
  for (const auto& folder : config.folders) {
    fact("Shared folder", std::format("{} as {}", folder.path, folder.tag));
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
  const bool active = isActive(m_machine.state);
  const bool dirty = m_edit != m_machine.config;
  m_apply->setEnabled(dirty);
  m_applyNote->setText(dirty && active ? "Takes effect when the machine is started the next time." : "");
  // QEMU cannot save a running machine whose picture is drawn by the host's GPU.
  const bool blocked = active && m_machine.config.accel3d;
  m_take->setEnabled(!m_busy && !blocked);
  m_take->setText(m_busy ? "Working …" : "Take snapshot");
  m_snapshotNote->setText(
      blocked            ? "With 3D acceleration a snapshot can only be taken while the machine is off."
      : active           ? "A snapshot of the running machine keeps its memory too and takes a moment."
      : m_snapshots.empty() ? "A snapshot keeps the machine's disks as they are now, to go back to later."
                            : ""
  );
}
