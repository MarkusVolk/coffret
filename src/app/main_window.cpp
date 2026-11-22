#include "app/main_window.h"

#include "app/console_window.h"
#include "coffret_build_info.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "display/console_node.h"
#include "input/input_grab.h"
#include "osinfo/install_media.h"
#include "platform/file_portal.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/machine_page.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "vm/domain_xml.h"
#include "vm/firmware_store.h"
#include "osinfo/windows_answers.h"
#include "vm/host_info.h"
#include "vm/host_network.h"
#include "vm/machine_factory.h"
#include "app/worker.h"
#include "storage/process.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <linux/input-event-codes.h>
#include <print>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  const Logger kLog("window");

  // What a domain is told about this session.
  coffret::DomainEnvironment environment() {
    static const coffret::DomainEnvironment found = [] {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      // coffret's own starter for virtiofsd where it is installed, the plain program otherwise.
      const char* override = std::getenv("COFFRET_VIRTIOFSD");
      std::string virtiofsd = override && *override ? override : COFFRET_INSTALL_LIBEXECDIR "/coffret-virtiofsd";
      if (::access(virtiofsd.c_str(), X_OK) != 0) {
        virtiofsd = coffret::findVirtiofsd();
      }
      return coffret::DomainEnvironment{
          .virtiofsd = std::move(virtiofsd), .uid = ::getuid(), .gid = ::getgid(), .runtimeDir = runtime ? runtime : ""
      };
    }();
    return found;
  }
  std::string usbAclHelper() {
    const char* override = std::getenv("COFFRET_USB_ACL");
    return override && *override ? override : COFFRET_INSTALL_LIBEXECDIR "/coffret-usb-acl";
  }

  constexpr float kSidebarWidth = 248.0F;
  constexpr float kTabBarHeight = 44.0F;
  constexpr float kToolbarHeight = 48.0F;
  constexpr float kStatusHeight = 26.0F;

  const char* glyphOf(coffret::MachineState state) {
    using coffret::MachineState;
    switch (state) {
    case MachineState::Running:
    case MachineState::ShuttingDown:
      return "player-play";
    case MachineState::Paused:
      return "player-pause";
    case MachineState::Saved:
    case MachineState::Suspended:
      return "zzz";
    case MachineState::Crashed:
      return "alert-triangle";
    case MachineState::Off:
      break;
    }
    return "device-desktop";
  }

  std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  std::string memoryText(unsigned mib) {
    return mib % 1024 == 0 ? std::format("{} GiB", mib / 1024) : std::format("{} MiB", mib);
  }

  void show(Node* node, bool visible) {
    node->setVisible(visible);
    node->setParticipatesInLayout(visible);
  }

  void clear(Flex* box) {
    while (!box->children().empty()) {
      box->removeChild(box->children().back().get());
    }
  }

  std::unique_ptr<Label> plainLabel(ui::LabelProps props) {
    props.configure = [](Label& label) { label.setUseMarkup(false); };
    return ui::label(std::move(props));
  }
} // namespace

coffret::MainWindow::MainWindow(Services services) : m_services(services), m_surface(services.connection) {
  m_surface.setRenderContext(&m_services.renderer);
  m_surface.setAnimationManager(&m_services.animations);
  m_surface.setPrepareFrameCallback([this](bool, bool) { prepare(); });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    m_surface.requestLayout();
    for (auto& tab : m_tabs) {
      tab->console->sizeChanged();
    }
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.connection.extension<DesktopIntegration>().setCursorShape(serial, shape);
  });
  if (!m_surface.initialize(
          nullptr, {.width = 1360U, .height = 860U, .minWidth = 720, .minHeight = 480, .title = "Coffret", .appId = "de.flk.Coffret"}
      )) {
    throw std::runtime_error("cannot create the window");
  }
  m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);
  m_services.hypervisor.setOnChanged([this] { machinesChanged(); });
  m_services.hypervisor.connect();
}

coffret::MainWindow::~MainWindow() {
  m_services.hypervisor.setOnChanged({});
  for (const auto& device : m_usbGranted) {
    (void)runCommand(usbAccessCommand(usbAclHelper(), device, false));
  }
}

void coffret::MainWindow::requestUpdate() { m_surface.requestLayout(); }

// ── Machines and tabs ────────────────────────────────────────────────────────

coffret::MainWindow::Tab* coffret::MainWindow::tabOf(const std::string& uuid) {
  const auto it = std::ranges::find_if(m_tabs, [&uuid](const auto& tab) { return tab->uuid == uuid; });
  return it == m_tabs.end() ? nullptr : it->get();
}

coffret::MainWindow::Tab* coffret::MainWindow::currentTab() { return m_current.empty() ? nullptr : tabOf(m_current); }

const coffret::Machine* coffret::MainWindow::currentMachine() const {
  return m_current.empty() ? nullptr : m_services.hypervisor.find(m_current);
}

const coffret::Machine* coffret::MainWindow::machineFor(const std::string& text) const {
  if (text.empty()) {
    return currentMachine();
  }
  for (const auto& machine : m_services.hypervisor.machines()) {
    if (machine.uuid == text || machine.name == text) {
      return &machine;
    }
  }
  return nullptr;
}

coffret::ConsoleView* coffret::MainWindow::shownConsole() {
  Tab* tab = currentTab();
  const Machine* machine = currentMachine();
  return tab && machine && isActive(machine->state) && !tab->details ? tab->console.get() : nullptr;
}

void coffret::MainWindow::machinesChanged() {
  m_listStale = true;
  m_tabsStale = true;
  for (const auto& machine : m_services.hypervisor.machines()) {
    if (machine.config.install.kernel.empty()) {
      m_installing.erase(machine.uuid);
    } else if (isActive(machine.state)) {
      m_installing.insert(machine.uuid);
    } else if (m_installing.erase(machine.uuid) != 0) {
      kLog.info("install: {} stopped", machine.name);
      finishInstall(machine.uuid);
    }
  }
  // A machine that stopped has given its devices back.
  for (auto it = m_usbConnected.begin(); it != m_usbConnected.end();) {
    const Machine* machine = m_services.hypervisor.find(it->first);
    if (machine && isActive(machine->state)) {
      ++it;
      continue;
    }
    for (const auto& device : it->second) {
      revokeUsb(device);
    }
    it = m_usbConnected.erase(it);
  }
  if (m_services.hypervisor.listed() && !m_waiting.empty()) {
    for (const auto& [name, parameter] : std::exchange(m_waiting, {})) {
      action(name, parameter);
    }
  }
  if (!m_openWhenKnown.empty() && m_services.hypervisor.find(m_openWhenKnown)) {
    openTab(std::exchange(m_openWhenKnown, {}));
    return;
  }
  // A machine that is gone takes its tab along.
  for (std::size_t index = m_tabs.size(); index-- > 0;) {
    if (m_services.hypervisor.connected() && !m_services.hypervisor.find(m_tabs[index]->uuid)) {
      closeTab(index);
    }
  }
  for (auto& tab : m_tabs) {
    const Machine* machine = m_services.hypervisor.find(tab->uuid);
    const bool active = machine && isActive(machine->state);
    if (!active) {
      tab->console->detach();
      tab->failures = 0;
      tab->displayProblem.clear();
    } else if (!tab->console->attached() && !tab->opening) {
      openDisplay(tab->uuid);
    }
  }
  requestUpdate();
}

void coffret::MainWindow::openDisplay(const std::string& uuid) {
  Tab* tab = tabOf(uuid);
  if (!tab || tab->opening) {
    return;
  }
  tab->opening = true;
  std::weak_ptr<int> alive = m_alive;
  m_services.hypervisor.openDisplay(uuid, [this, alive, uuid](std::expected<int, std::string> bus) {
    Tab* found = alive.expired() ? nullptr : tabOf(uuid);
    if (!found) {
      if (bus) {
        ::close(*bus);
      }
      return;
    }
    found->opening = false;
    if (bus) {
      found->failures = 0;
      found->displayProblem.clear();
      found->console->attach(*bus);
    } else {
      // A machine that is just starting may have no display yet.
      found->displayProblem = bus.error();
      kLog.warn("display of {}: {}", uuid, bus.error());
      if (++found->failures < 5) {
        found->retry.start(std::chrono::seconds(1), [this, uuid] {
          const Machine* machine = m_services.hypervisor.find(uuid);
          if (machine && isActive(machine->state)) {
            openDisplay(uuid);
          }
        });
      }
    }
    requestUpdate();
  });
}

void coffret::MainWindow::openTab(const std::string& uuid) {
  if (!m_services.hypervisor.find(uuid)) {
    return;
  }
  if (!tabOf(uuid)) {
    auto tab = std::make_unique<Tab>();
    tab->uuid = uuid;
    tab->console = std::make_unique<ConsoleView>(
        ConsoleView::Host{
            .surface = m_surface,
            .grab = m_services.grab,
            .changed = [this] { requestUpdate(); },
            .hostCursor =
                [this](bool hidden, std::uint32_t serial) {
                  if (hidden) {
                    if (wl_pointer* pointer = m_services.connection.pointer()) {
                      wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
                    }
                  } else {
                    // The toolkit believes its arrow is still up.
                    m_services.connection.extension<DesktopIntegration>().setCursorShape(serial, 1);
                  }
                },
            .guestCopied =
                [this](std::string text) {
                  m_hostClipboard = text;
                  if (m_services.setClipboard) {
                    m_services.setClipboard(std::move(text));
                  }
                },
        }
    );
    tab->console->hostCopied(m_hostClipboard);
    tab->console->setActive(false);
    if (const Machine* machine = m_services.hypervisor.find(uuid)) {
      tab->console->setFollowsWithoutPointer(machine->config.kind == GuestKind::Windows && machine->config.neptune);
    }
    m_tabs.push_back(std::move(tab));
  }
  if (Tab* old = currentTab()) {
    old->console->setActive(false);
  }
  m_current = uuid;
  m_newShown = false;
  m_message.clear();
  m_listStale = true;
  m_tabsStale = true;
  machinesChanged();
}

void coffret::MainWindow::closeTab(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  Tab& tab = *m_tabs[index];
  if (tab.uuid == m_current) {
    m_current.clear();
  }
  if (tab.picture && m_consoles) {
    m_consoles->removeChild(tab.picture);
  }
  m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  m_listStale = true;
  m_tabsStale = true;
  requestUpdate();
}

void coffret::MainWindow::showHome() {
  if (Tab* old = currentTab()) {
    old->console->setActive(false);
  }
  m_current.clear();
  m_newShown = false;
  m_listStale = true;
  m_tabsStale = true;
  requestUpdate();
}

void coffret::MainWindow::run(
    const std::string& what, const std::string& uuid, void (Hypervisor::*call)(const std::string&, Hypervisor::Done)
) {
  if (uuid.empty()) {
    return;
  }
  m_message.clear();
  std::weak_ptr<int> alive = m_alive;
  (m_services.hypervisor.*call)(uuid, [this, alive, what, uuid](Hypervisor::Result result) {
    if (alive.expired()) {
      return;
    }
    if (!result) {
      m_message = result.error();
      kLog.warn("{}: {}", what, result.error());
      // libvirt only says that the bridge helper failed; what is missing for it is known here.
      const Machine* machine = m_services.hypervisor.find(uuid);
      if (what == "start" && machine) {
        for (const auto& nic : machine->config.nics) {
          if (nic.kind == NetworkKind::Bridge) {
            if (std::string problem = bridgeProblem(nic.bridge); !problem.empty()) {
              m_message = std::format("{} cannot start on bridge {}: {}", machine->name, nic.bridge, problem);
            }
          }
        }
      }
    }
    requestUpdate();
  });
  requestUpdate();
}

void coffret::MainWindow::importImage(ImageImport request) {
  m_message = std::format("Setting up {} …", request.name);
  requestUpdate();
  std::weak_ptr<int> alive = m_alive;
  runInThread<std::expected<VmConfig, std::string>>(
      [request = std::move(request)] { return prepareImport(request); },
      [this, alive](std::expected<VmConfig, std::string> config) {
        if (alive.expired()) {
          return;
        }
        if (!config) {
          m_creating = false;
          m_message = config.error();
          kLog.warn("import: {}", m_message);
          requestUpdate();
          return;
        }
        define(*config);
      }
  );
}

void coffret::MainWindow::define(const VmConfig& config, std::function<void()> undo, bool install) {
  std::weak_ptr<int> alive = m_alive;
  if (!undo) {
    undo = [folder = machineFolder(config.name)] {
      std::error_code error;
      std::filesystem::remove_all(folder, error);
    };
  }
  m_services.hypervisor.define(
      buildDomainXml(config, environment()),
      [this, alive, undo = std::move(undo), install, windows = config.kind == GuestKind::Windows](std::expected<std::string, std::string> uuid) {
        if (alive.expired()) {
          return;
        }
        m_creating = false;
        if (uuid) {
          m_message.clear();
          m_newShown = false;
          if (m_newPassword) {
            m_newPassword->setValue("");
          }
          // Shown as soon as the list has it.
          m_openWhenKnown = *uuid;
          // Nobody needs to be at an unattended installation, so it begins at once.
          if (install) {
            run("start", *uuid, &Hypervisor::start);
            if (windows) {
              pressKeyAtStart(*uuid);
            }
          }
        } else {
          // What was set up for a machine that does not exist is of no use.
          undo();
          m_message = uuid.error();
          kLog.warn("define: {}", m_message);
        }
        requestUpdate();
      }
  );
}

void coffret::MainWindow::applySettings(const VmConfig& wanted) {
  const std::string uuid = m_current;
  m_message.clear();
  std::weak_ptr<int> alive = m_alive;
  // Secure boot is another build of the firmware: found, and converted the first time, off this thread.
  const Machine* machine = m_services.hypervisor.find(uuid);
  if (machine && wanted.firmware != Firmware::Bios && wanted.firmware != machine->config.firmware && !m_firmwareChosen) {
    const bool secure = wanted.firmware == Firmware::UefiSecureBoot;
    runInThread<std::expected<FirmwareFiles, std::string>>(
        [secure] { return prepareFirmware(secure); },
        [this, alive, wanted, uuid](std::expected<FirmwareFiles, std::string> firmware) {
          if (alive.expired() || uuid != m_current) {
            return;
          }
          if (!firmware) {
            m_message = firmware.error();
            requestUpdate();
            return;
          }
          VmConfig config = wanted;
          config.firmwareCode = firmware->code;
          config.nvramTemplate = firmware->variables;
          if (config.nvramPath.empty()) {
            const std::string folder = ownFolder(config);
            config.nvramPath = (std::filesystem::path(folder.empty() ? machineFolder(config.name) : folder) / "vars.qcow2").string();
          }
          m_firmwareChosen = true;
          applySettings(config);
          m_firmwareChosen = false;
        }
    );
    return;
  }
  const VmConfig config = wanted;
  // Into the domain as libvirt has it, so what coffret does not manage stays.
  m_services.hypervisor.xml(uuid, false, [this, alive, config](std::expected<std::string, std::string> xml) {
    if (alive.expired()) {
      return;
    }
    const auto updated = xml.and_then([&](const std::string& text) {
      return updateDomainXml(text, config, environment());
    });
    if (!updated) {
      m_message = updated.error();
      requestUpdate();
      return;
    }
    m_services.hypervisor.define(*updated, [this, alive](std::expected<std::string, std::string> uuid) {
      if (!alive.expired()) {
        m_message = uuid ? "" : uuid.error();
        requestUpdate();
      }
    });
  });
}

void coffret::MainWindow::finishInstall(const std::string& uuid) {
  const Machine* machine = m_services.hypervisor.find(uuid);
  if (!machine) {
    return;
  }
  // The installer stopped the machine where it would have rebooted: from now on it boots its disk.
  VmConfig config = machine->config;
  const DirectBoot used = std::exchange(config.install, {});
  for (auto& disk : config.disks) {
    if (disk.cdrom) {
      disk.path.clear();
    }
  }
  std::weak_ptr<int> alive = m_alive;
  m_services.hypervisor.xml(uuid, false, [this, alive, uuid, config, used](std::expected<std::string, std::string> xml) {
    if (alive.expired()) {
      return;
    }
    const auto updated = xml.and_then([&](const std::string& text) { return updateDomainXml(text, config, environment()); });
    if (!updated) {
      m_message = updated.error();
      kLog.warn("install: {}", m_message);
      requestUpdate();
      return;
    }
    m_services.hypervisor.define(*updated, [this, alive, uuid, used](std::expected<std::string, std::string> defined) {
      if (alive.expired()) {
        return;
      }
      if (!defined) {
        m_message = defined.error();
        kLog.warn("install: {}", m_message);
        requestUpdate();
        return;
      }
      kLog.info("install: done, the machine boots from its disk now");
      std::error_code error;
      std::filesystem::remove(used.kernel, error);
      std::filesystem::remove(used.initrd, error);
      run("start", uuid, &Hypervisor::start);
    });
  });
}

void coffret::MainWindow::loadSnapshots() {
  const std::string uuid = m_current;
  m_snapshotsOf = uuid;
  std::weak_ptr<int> alive = m_alive;
  m_services.hypervisor.snapshots(uuid, [this, alive, uuid](std::expected<std::vector<Snapshot>, std::string> snapshots) {
    if (alive.expired() || uuid != m_current) {
      return;
    }
    m_machinePage->setBusy(false);
    m_machinePage->setSnapshots(snapshots ? std::move(*snapshots) : std::vector<Snapshot>());
    if (!snapshots) {
      m_message = snapshots.error();
    }
    requestUpdate();
  });
}

void coffret::MainWindow::snapshotDone(Hypervisor::Result result) {
  m_message = result ? "" : result.error();
  if (!result) {
    kLog.warn("snapshot: {}", result.error());
  }
  loadSnapshots();
  requestUpdate();
}

void coffret::MainWindow::removeMachine(const std::string& uuid, bool deleteFiles) {
  const Machine* machine = m_services.hypervisor.find(uuid);
  if (!machine) {
    return;
  }
  if (isActive(machine->state)) {
    m_message = "A machine can be removed while it is off.";
    requestUpdate();
    return;
  }
  const std::string folder = ownFolder(machine->config);
  std::vector<VmConfig> others;
  for (const auto& other : m_services.hypervisor.machines()) {
    if (other.uuid != uuid) {
      others.push_back(other.config);
    }
  }
  m_message.clear();
  std::weak_ptr<int> alive = m_alive;
  // Linked clones would lose the disks they build on.
  runInThread<std::vector<std::string>>(
      [folder, others = std::move(others), deleteFiles] {
        return deleteFiles && !folder.empty() ? machinesBuiltOn(folder, others) : std::vector<std::string>();
      },
      [this, alive, uuid, folder, deleteFiles](std::vector<std::string> clones) {
        if (alive.expired()) {
          return;
        }
        if (!clones.empty()) {
          std::string names;
          for (const auto& name : clones) {
            names += (names.empty() ? "" : ", ") + name;
          }
          m_message = std::format("Its disks cannot be deleted: {} is linked to them.", names);
          requestUpdate();
          return;
        }
        m_services.hypervisor.remove(uuid, [this, alive, folder, deleteFiles](Hypervisor::Result result) {
          if (alive.expired()) {
            return;
          }
          if (!result) {
            m_message = result.error();
          } else if (deleteFiles && !folder.empty()) {
            std::error_code error;
            // The base of linked clones is kept read-only.
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
              std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error);
            }
            std::filesystem::remove_all(folder, error);
            if (error) {
              m_message = std::format("{} could not be deleted: {}", folder, error.message());
            }
          }
          requestUpdate();
        });
      }
  );
}

void coffret::MainWindow::connectUsb(const HostUsbDevice& device, bool connect) {
  const std::string uuid = m_current;
  m_message.clear();
  std::weak_ptr<int> alive = m_alive;
  const auto done = [this, alive, uuid, device, connect](Hypervisor::Result result) {
    if (alive.expired()) {
      return;
    }
    auto& connected = m_usbConnected[uuid];
    if (!result) {
      m_message = result.error().find("ermission") != std::string::npos
                      ? std::format("{} could not be opened: this user has no access to USB devices.", device.name)
                      : result.error();
      kLog.warn("usb: {}", result.error());
      if (connect) {
        revokeUsb(device);
      }
    } else if (connect) {
      connected.push_back(device);
    } else {
      std::erase(connected, device);
      revokeUsb(device);
    }
    if (uuid == m_current) {
      m_machinePage->setConnectedUsb(connected);
    }
    requestUpdate();
  };
  if (!connect) {
    m_services.hypervisor.detachDevice(uuid, usbHostdevXml(device), done);
    return;
  }
  // QEMU opens the device as the user, who by default may not: coffret-usb-acl lets them, for this
  // device alone and until it is taken back.
  if (::access(usbNode(device).c_str(), R_OK | W_OK) == 0) {
    m_services.hypervisor.attachDevice(uuid, usbHostdevXml(device), done);
    return;
  }
  runInThread<std::expected<std::string, std::string>>(
      [command = usbAccessCommand(usbAclHelper(), device, true)] { return runCommand(command); },
      [this, alive, uuid, device, done](std::expected<std::string, std::string> granted) {
        if (alive.expired()) {
          return;
        }
        if (!granted) {
          m_message = std::format("{} could not be opened: {}", device.name, granted.error());
          kLog.warn("usb: {}", granted.error());
          requestUpdate();
          return;
        }
        m_usbGranted.push_back(device);
        m_services.hypervisor.attachDevice(uuid, usbHostdevXml(device), done);
      }
  );
}

void coffret::MainWindow::revokeUsb(const HostUsbDevice& device) {
  if (std::erase(m_usbGranted, device) == 0) {
    return;
  }
  runInThread<bool>(
      [command = usbAccessCommand(usbAclHelper(), device, false)] {
        if (const auto done = runCommand(command); !done) {
          kLog.warn("usb: {}", done.error());
        }
        return true;
      },
      [](bool) {}
  );
}

void coffret::MainWindow::cloneMachine(const std::string& name, bool linked) {
  const Machine* machine = currentMachine();
  if (!machine || m_creating) {
    return;
  }
  if (isActive(machine->state)) {
    m_message = "A machine can be cloned while it is off.";
    requestUpdate();
    return;
  }
  if (std::ranges::any_of(m_services.hypervisor.machines(), [&name](const Machine& other) { return other.name == name; })) {
    m_message = std::format("There is a machine named {} already.", name);
    requestUpdate();
    return;
  }
  m_creating = true;
  m_machinePage->setBusy(true);
  m_message = std::format("Cloning {} …", machine->name);
  requestUpdate();
  std::weak_ptr<int> alive = m_alive;
  const VmConfig source = machine->config;
  // Whether there are snapshots decides what kind of clone is possible; libvirt knows.
  m_services.hypervisor.snapshots(machine->uuid, [this, alive, source, name, linked](std::expected<std::vector<Snapshot>, std::string> snapshots) {
    if (alive.expired()) {
      return;
    }
    const auto failed = [this](const std::string& why) {
      m_creating = false;
      m_machinePage->setBusy(false);
      m_message = why;
      kLog.warn("clone: {}", why);
      requestUpdate();
    };
    if (!snapshots) {
      failed(snapshots.error());
      return;
    }
    const std::string folder = machineFolder(name);
    const auto plan = planClone(source, name, folder, linked, !snapshots->empty());
    if (!plan) {
      failed(plan.error());
      return;
    }
    runInThread<std::expected<VmConfig, std::string>>(
        [plan = *plan, folder] { return runClone(plan, folder); },
        [this, alive, failed, plan = *plan, folder](std::expected<VmConfig, std::string> config) {
          if (alive.expired()) {
            return;
          }
          m_machinePage->setBusy(false);
          if (!config) {
            failed(config.error());
            return;
          }
          define(*config, [plan, folder] { undoClone(plan, folder); });
        }
    );
  });
}

void coffret::MainWindow::showNew(const std::string& iso) {
  if (Tab* old = currentTab()) {
    old->console->setActive(false);
  }
  m_newShown = true;
  m_message.clear();
  m_tabsStale = true;
  if (!iso.empty()) {
    if (m_newIso) {
      m_newIso->setValue(iso);
    }
    setInstallMedia(iso);
  }
  requestUpdate();
}

void coffret::MainWindow::setInstallMedia(const std::string& iso) {
  std::error_code error;
  if (iso == m_newMediaPath || !std::filesystem::is_regular_file(iso, error)) {
    return;
  }
  m_newMediaPath = iso;
  m_newOsId.clear();
  m_newUnattendedPossible = false;
  m_newSource = SourceKind::Unknown;
  m_newKind = GuestKind::Linux;
  if (m_newDetected) {
    m_newDetected->setText("Looking at the medium …");
  }
  std::weak_ptr<int> alive = m_alive;
  struct Looked {
    SourceKind kind = SourceKind::Unknown;
    std::optional<InstallMedia> media;
  };
  runInThread<Looked>(
      [iso] {
        Looked looked;
        looked.kind = classifySource(iso);
        if (looked.kind == SourceKind::InstallMedium) {
          looked.media = detectInstallMedia(iso);
        }
        return looked;
      },
      [this, alive, iso](Looked looked) {
        if (alive.expired() || iso != m_newMediaPath || !m_newDetected) {
          return;
        }
        m_newSource = looked.kind;
        std::string name = std::filesystem::path(iso).stem().string();
        if (const auto dot = name.find('.'); dot != std::string::npos && dot > 0) {
          // "gnome-image-intel-corei7-64.rootfs" is no better a name with its ending.
          name.erase(dot);
        }
        if (looked.media) {
          const InstallMedia& media = *looked.media;
          m_newOsId = media.id;
          m_newKind = media.kind;
          m_newUnattendedPossible = media.unattended;
          name = media.name;
          m_newDetected->setText(std::format("This is {}, to install from.", media.name));
          // At least what the system's makers recommend, and not less than a desktop is comfortable with.
          m_newMemory->setValue(static_cast<int>(std::clamp(std::max(media.memoryMiB, 4096U) / 1024, 1U, std::max(1U, hostMemoryMiB() / 1024))));
          m_newCpus->setValue(static_cast<int>(std::clamp(std::max(media.cpus, 2U), 1U, hostProcessors())));
          m_newDisk->setValue(static_cast<int>(std::max<std::uint64_t>(media.diskBytes >> 30, 40)));
        } else if (looked.kind == SourceKind::InstallMedium) {
          m_newDetected->setText("An installation medium whose system is not known; the settings are general ones.");
        } else if (looked.kind == SourceKind::DiskImage) {
          m_newDetected->setText("A disk with a system on it: the machine starts from it.");
        } else {
          m_newDetected->setText("Neither an installation medium nor a disk image.");
        }
        if (!m_newNameTyped) {
          m_newName->setValue(name);
          m_newNameTyped = false;
        }
        requestUpdate();
      }
  );
}

void coffret::MainWindow::createNew() {
  if (m_creating || !m_newName) {
    return;
  }
  if (m_newSource == SourceKind::DiskImage) {
    ImageImport request;
    request.name = m_newName->value();
    request.image = m_newIso->value();
    request.copy = !m_newKeepInPlace;
    request.memoryMiB = static_cast<unsigned>(m_newMemory->value()) * 1024;
    request.cpus = static_cast<unsigned>(m_newCpus->value());
    if (std::ranges::any_of(m_services.hypervisor.machines(), [&](const Machine& machine) { return machine.name == request.name; })) {
      m_message = std::format("There is a machine named {} already.", request.name);
      requestUpdate();
      return;
    }
    m_creating = true;
    importImage(std::move(request));
    return;
  }
  NewMachine request;
  request.name = m_newName->value();
  request.iso = m_newIso->value();
  request.kind = m_newKind;
  request.osId = m_newOsId;
  request.memoryMiB = static_cast<unsigned>(m_newMemory->value()) * 1024;
  request.cpus = static_cast<unsigned>(m_newCpus->value());
  request.diskBytes = static_cast<std::uint64_t>(m_newDisk->value()) << 30;
  if (m_newUnattendedPossible && m_newUnattended) {
    GuestSetup setup = m_hostSetup;
    setup.user = m_newUser->value();
    setup.password = m_newPassword->value();
    if (setup.user.empty() || setup.password.empty()) {
      m_message = "An installation without anybody at it needs a user name and a password.";
      requestUpdate();
      return;
    }
    if (setup.user != m_hostSetup.user) {
      setup.realName.clear();
    }
    request.unattended = std::move(setup);
  }
  if (m_services.hypervisor.find(request.name) || std::ranges::any_of(m_services.hypervisor.machines(), [&](const Machine& machine) {
        return machine.name == request.name;
      })) {
    m_message = std::format("There is a machine named {} already.", request.name);
    requestUpdate();
    return;
  }
  m_creating = true;
  m_message.clear();
  requestUpdate();
  std::weak_ptr<int> alive = m_alive;
  const bool unattended = request.unattended.has_value();
  runInThread<std::expected<VmConfig, std::string>>(
      [request = std::move(request)] { return prepareNew(request); },
      [this, alive, unattended](std::expected<VmConfig, std::string> config) {
        if (alive.expired()) {
          return;
        }
        if (!config) {
          m_creating = false;
          m_message = config.error();
          requestUpdate();
          return;
        }
        define(*config, {}, unattended);
      }
  );
}

void coffret::MainWindow::pressKeyAtStart(const std::string& uuid) {
  // The medium asks for a key for five seconds once the firmware is through, which takes a few.
  m_pressKeyTicks = 30;
  m_pressKey.startRepeating(std::chrono::milliseconds(700), [this, uuid] {
    if (Tab* tab = tabOf(uuid); tab && tab->console->ready()) {
      tab->console->typeKeys({KEY_ENTER});
    }
    if (--m_pressKeyTicks == 0) {
      m_pressKey.stop();
    }
  });
}

void coffret::MainWindow::toggleFullscreen() {
  m_surface.toggleFullscreen();
  m_barRevealed = false;
  requestUpdate();
}

void coffret::MainWindow::toggleBare() {
  // Full screen shows the picture alone already.
  if (m_surface.fullscreen()) {
    return;
  }
  m_bare = !m_bare;
  m_barRevealed = false;
  requestUpdate();
}

// ── Actions ──────────────────────────────────────────────────────────────────

std::string coffret::MainWindow::describe() const {
  std::string text;
  const Hypervisor& hypervisor = m_services.hypervisor;
  text += hypervisor.connected() ? "libvirt: connected\n" : std::format("libvirt: {}\n", hypervisor.problem().empty() ? "connecting" : hypervisor.problem());
  for (const auto& machine : hypervisor.machines()) {
    text += std::format("machine: {} [{}] {}\n", machine.name, toString(machine.state), machine.uuid);
  }
  for (const auto& tab : m_tabs) {
    const Machine* machine = hypervisor.find(tab->uuid);
    text += std::format(
        "tab: {}{} console: {}{}\n", machine ? machine->name : tab->uuid, tab->uuid == m_current ? " (shown)" : "",
        tab->console->attached() ? tab->console->describe() : "not attached",
        tab->displayProblem.empty() ? "" : " problem: " + tab->displayProblem
    );
    if (const ConsoleNode* node = tab->console->node()) {
      const GuestCursor cursor = node->guestCursor();
      text += std::format("  guest cursor: {}x{} at {},{} {}\n", cursor.width, cursor.height, cursor.x, cursor.y, cursor.visible ? "shown" : "hidden");
    }
  }
  if (!m_message.empty()) {
    text += std::format("message: {}\n", m_message);
  }
  if (!m_hostClipboard.empty()) {
    std::string start = m_hostClipboard.substr(0, 60);
    std::ranges::replace(start, '\n', ' ');
    text += std::format("clipboard: {} bytes: {}\n", m_hostClipboard.size(), start);
  }
  text += std::format("fullscreen: {}\n", m_surface.fullscreen());
  return text;
}

bool coffret::MainWindow::action(const std::string& name, const std::string& parameter) {
  // What is about machines waits for the list of them.
  if (!m_services.hypervisor.listed() && name != "fullscreen" && name != "state" && name != "search") {
    m_waiting.emplace_back(name, parameter);
    return true;
  }
  using Call = void (Hypervisor::*)(const std::string&, Hypervisor::Done);
  static const std::pair<const char*, Call> kPower[] = {
      {"start", &Hypervisor::start},        {"shutdown", &Hypervisor::shutDown}, {"force-off", &Hypervisor::forceOff},
      {"reset", &Hypervisor::reset},        {"pause", &Hypervisor::pause},       {"resume", &Hypervisor::resume},
      {"suspend", &Hypervisor::saveState},
  };
  for (const auto& [action, call] : kPower) {
    if (name == action) {
      const Machine* machine = machineFor(parameter);
      if (!machine) {
        m_message = std::format("No machine named {}.", parameter);
        return true;
      }
      run(name, machine->uuid, call);
      return true;
    }
  }
  if (name == "open") {
    if (const Machine* machine = machineFor(parameter)) {
      openTab(machine->uuid);
    }
  } else if (name == "home") {
    showHome();
  } else if (name == "close-tab") {
    const auto it = std::ranges::find_if(m_tabs, [this](const auto& tab) { return tab->uuid == m_current; });
    if (it != m_tabs.end()) {
      closeTab(static_cast<std::size_t>(it - m_tabs.begin()));
    }
  } else if (name == "fullscreen") {
    toggleFullscreen();
  } else if (name == "bare") {
    toggleBare();
  } else if (name == "state") {
    std::print("{}", describe());
    std::fflush(stdout);
  } else if (name == "search") {
    m_query = lower(parameter);
    m_listStale = true;
    requestUpdate();
  } else if (name == "clipboard") {
    // As if the host had copied this text: for tests without the keyboard.
    hostCopied(parameter);
  } else if (name == "details") {
    if (Tab* tab = currentTab()) {
      tab->details = !tab->details;
      requestUpdate();
    }
  } else if (name == "snapshot" || name == "revert" || name == "delete-snapshot") {
    if (!currentMachine()) {
      return false;
    }
    const auto done = [this, alive = std::weak_ptr<int>(m_alive)](Hypervisor::Result result) {
      if (!alive.expired()) {
        snapshotDone(std::move(result));
      }
    };
    if (name == "snapshot") {
      m_services.hypervisor.takeSnapshot(m_current, parameter, "", done);
    } else if (name == "revert") {
      m_services.hypervisor.revertToSnapshot(m_current, parameter, done);
    } else {
      m_services.hypervisor.deleteSnapshot(m_current, parameter, done);
    }
  } else if (name == "delete" || name == "forget") {
    // delete: with the machine's own folder; forget: only out of the library.
    const Machine* machine = machineFor(parameter);
    if (!machine) {
      return false;
    }
    removeMachine(machine->uuid, name == "delete");
  } else if (name == "usb" || name == "usb-off") {
    // "046d:c548": the first device of these ids that is plugged in.
    unsigned vendor = 0, product = 0;
    if (!currentMachine() || std::sscanf(parameter.c_str(), "%x:%x", &vendor, &product) != 2) {
      return false;
    }
    for (const auto& device : listUsbDevices()) {
      if (device.vendor == vendor && device.product == product) {
        connectUsb(device, name == "usb");
        return true;
      }
    }
    m_message = std::format("No USB device {} is plugged in.", parameter);
  } else if (name == "install-3d-driver") {
    installGraphicsDriver();
  } else if (name == "clone" || name == "clone-linked") {
    cloneMachine(parameter, name == "clone-linked");
  } else if (name == "set") {
    // "memory=2048", "cpus=4", "3d=off", "sound=off": one setting of the shown machine.
    const Machine* machine = currentMachine();
    const auto split = parameter.find('=');
    if (!machine || split == std::string::npos) {
      return false;
    }
    VmConfig config = machine->config;
    const std::string key = parameter.substr(0, split), value = parameter.substr(split + 1);
    if (key == "memory") {
      config.memoryMiB = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "cpus") {
      config.cpus = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "3d") {
      config.accel3d = value == "on";
      if (config.kind == GuestKind::Windows) {
        config.venus = config.neptune = config.accel3d;
      }
    } else if (key == "venus") {
      config.venus = value == "on";
    } else if (key == "sound") {
      config.sound = value == "on";
    } else if (key == "tpm") {
      config.tpm = value == "on";
    } else if (key == "secureboot" && config.firmware != Firmware::Bios) {
      config.firmware = value == "on" ? Firmware::UefiSecureBoot : Firmware::Uefi;
    } else if (key == "microphone") {
      config.microphone = value == "on";
    } else if (key == "forward") {
      // "forward=tcp:2222:22,udp:5353:53" for the first network card; "forward=" forwards nothing.
      if (config.nics.empty()) {
        return false;
      }
      config.nics[0].forwards = parsePortForwards(value).value_or(std::vector<PortForward>());
    } else if (key == "network") {
      // "network=nat", or "network=br0" for a bridge, for the first network card.
      if (config.nics.empty()) {
        config.nics.emplace_back();
      }
      config.nics[0].kind = value == "nat" ? NetworkKind::User : NetworkKind::Bridge;
      config.nics[0].bridge = value == "nat" ? "" : value;
    } else if (key == "share") {
      // "share=/path:tag"; "share=" shares nothing.
      config.folders.clear();
      if (const auto colon = value.rfind(':'); colon != std::string::npos) {
        config.folders.push_back({value.substr(0, colon), value.substr(colon + 1), false});
      }
    } else {
      return false;
    }
    applySettings(config);
  } else if (name == "new") {
    showNew(parameter);
  } else if (name == "finish-install") {
    if (const Machine* machine = machineFor(parameter)) {
      finishInstall(machine->uuid);
    }
  } else if (name == "unattended") {
    m_newUnattended = parameter != "off";
  } else if (name == "name" && m_newName) {
    m_newName->setValue(parameter);
    m_newNameTyped = true;
  } else if (name == "user" && m_newUser) {
    m_newUser->setValue(parameter);
  } else if (name == "password" && m_newPassword) {
    m_newPassword->setValue(parameter);
  } else if (name == "create" || name == "create-in-place") {
    m_newKeepInPlace = name == "create-in-place";
    createNew();
  } else if (name == "import") {
    // "<image>" or "<image>|<name>|<cdrom>": a machine from a disk image, as a copy.
    std::vector<std::string> parts;
    std::stringstream stream(parameter);
    for (std::string part; std::getline(stream, part, '|');) {
      parts.push_back(part);
    }
    if (parts.empty()) {
      return false;
    }
    ImageImport request;
    request.image = parts[0];
    request.name = parts.size() > 1 && !parts[1].empty() ? parts[1] : std::filesystem::path(parts[0]).stem().string();
    request.cdrom = parts.size() > 2 ? parts[2] : "";
    importImage(std::move(request));
  } else if (name == "define") {
    // A domain from a file, as it is: for tests and for machines made elsewhere.
    std::ifstream file(parameter);
    std::stringstream xml;
    xml << file.rdbuf();
    std::weak_ptr<int> alive = m_alive;
    m_services.hypervisor.define(xml.str(), [this, alive](std::expected<std::string, std::string> uuid) {
      if (alive.expired()) {
        return;
      }
      m_message = uuid ? "" : uuid.error();
      if (!uuid) {
        kLog.warn("define: {}", uuid.error());
      }
      requestUpdate();
    });
  } else if (ConsoleView* console = shownConsole(); console && name == "key") {
    console->typeKeys(parseKeys(parameter));
  } else if (console && name == "click") {
    unsigned x = 0, y = 0;
    if (std::sscanf(parameter.c_str(), "%u,%u", &x, &y) != 2) {
      return false;
    }
    console->clickAt(x, y);
  } else if (console && name == "shot") {
    console->dumpTo(parameter);
  } else if (console && name == "grab") {
    console->grab();
  } else if (console && name == "release") {
    console->release();
  } else if (console && name == "size") {
    unsigned width = 0, height = 0;
    if (std::sscanf(parameter.c_str(), "%ux%u", &width, &height) != 2) {
      return false;
    }
    console->requestSize(width, height);
  } else {
    return false;
  }
  return true;
}

// ── Building ─────────────────────────────────────────────────────────────────

void coffret::MainWindow::build() {
  m_root = ui::row({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Surface)});
  m_root->setAnimationManager(&m_services.animations);
  m_surface.setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());

  auto sidebar = ui::column({
      .out = &m_sidebar,
      .align = FlexAlign::Stretch,
      .gap = 2.0F,
      .paddingV = Style::spaceMd,
      .paddingH = Style::spaceSm,
      .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
      .width = kSidebarWidth,
  });
  sidebar->addChild(ui::input({
      .out = &m_search,
      .placeholder = "Search machines",
      .clearButtonEnabled = true,
      .onChange =
          [this](const std::string& text) {
            m_query = lower(text);
            m_listStale = true;
            requestUpdate();
          },
  }));
  sidebar->addChild(ui::separator({.spacing = Style::spaceSm}));
  ScrollView* listScroll = nullptr;
  sidebar->addChild(ui::scrollView({
      .out = &listScroll,
      .state = &m_listScroll,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  }));
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(2.0F);
  sidebar->addChild(ui::button({
      .text = "New machine",
      .glyph = "plus",
      .contentAlign = ButtonContentAlign::Start,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { showNew({}); },
  }));
  sidebar->addChild(plainLabel({
      .out = &m_connection,
      .fontSize = Style::fontSizeCaption,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 3,
  }));
  m_root->addChild(std::move(sidebar));

  auto content = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  content->addChild(ui::row(
      {.out = &m_tabBar, .align = FlexAlign::Center, .gap = 2.0F, .paddingH = Style::spaceSm, .height = kTabBarHeight}
  ));

  const auto tool = [this](Button** out, const char* glyph, const char* tooltip, std::function<void()> click) {
    return ui::button({
        .out = out,
        .glyph = std::string(glyph),
        .variant = ButtonVariant::Ghost,
        .tooltip = std::string(tooltip),
        .onClick = std::move(click),
    });
  };
  const auto power = [this](void (Hypervisor::*call)(const std::string&, Hypervisor::Done), const char* what) {
    return [this, call, what] { run(what, m_current, call); };
  };
  content->addChild(ui::row(
      {.out = &m_toolbar,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kToolbarHeight},
      tool(&m_startButton, "player-play", "Power on", power(&Hypervisor::start, "start")),
      tool(&m_pauseButton, "player-pause", "Pause", [this] {
        const Machine* machine = currentMachine();
        run("pause", m_current, machine && machine->state == MachineState::Paused ? &Hypervisor::resume : &Hypervisor::pause);
      }),
      tool(&m_suspendButton, "zzz", "Suspend: stop with the state kept", power(&Hypervisor::saveState, "suspend")),
      tool(&m_shutDownButton, "power", "Shut down the guest", power(&Hypervisor::shutDown, "shutdown")),
      tool(&m_forceOffButton, "plug-x", "Power off at once", power(&Hypervisor::forceOff, "force-off")),
      tool(&m_resetButton, "refresh", "Reset", power(&Hypervisor::reset, "reset")),
      ui::separator({.spacing = Style::spaceSm}),
      tool(&m_keysButton, "keyboard", "Send Ctrl+Alt+Del", [this] {
        if (ConsoleView* console = shownConsole()) {
          console->typeKeys({KEY_LEFTCTRL, KEY_LEFTALT, KEY_DELETE});
        }
      }),
      tool(&m_bareButton, "arrows-maximize", "Only the guest's picture (Shift+F11)", [this] { toggleBare(); }),
      tool(&m_fullscreenButton, "maximize", "Full screen (F11)", [this] { toggleFullscreen(); }),
      ui::spacer(),
      tool(&m_detailsButton, "settings", "Settings and snapshots", [this] {
        if (Tab* tab = currentTab()) {
          tab->details = !tab->details;
          requestUpdate();
        }
      })
  ));

  // Home: every machine with what it is doing.
  auto home = ui::column({.out = &m_home, .align = FlexAlign::Stretch, .gap = Style::spaceMd, .padding = Style::spaceLg, .flexGrow = 1.0F});
  home->addChild(plainLabel({
      .text = "Virtual machines",
      .fontSize = Style::fontSizeHeader,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  home->addChild(plainLabel({
      .out = &m_homeEmpty,
      .fontSize = Style::fontSizeBody,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  ScrollView* homeScroll = nullptr;
  home->addChild(ui::scrollView({
      .out = &homeScroll,
      .state = &m_homeScroll,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  }));
  m_homeList = homeScroll->content();
  m_homeList->setDirection(FlexDirection::Vertical);
  m_homeList->setAlign(FlexAlign::Stretch);
  m_homeList->setGap(Style::spaceXs);
  content->addChild(std::move(home));

  // The machine's page: what it is made of and its snapshots.
  auto page = std::make_unique<MachinePage>(MachinePage::Callbacks{
      .apply = [this](VmConfig config) { applySettings(config); },
      .takeSnapshot =
          [this](std::string name) {
            m_machinePage->setBusy(true);
            m_services.hypervisor.takeSnapshot(m_current, name, "", [this, alive = std::weak_ptr<int>(m_alive)](Hypervisor::Result result) {
              if (!alive.expired()) {
                snapshotDone(std::move(result));
              }
            });
            requestUpdate();
          },
      .revertTo =
          [this](std::string name) {
            m_machinePage->setBusy(true);
            m_services.hypervisor.revertToSnapshot(m_current, name, [this, alive = std::weak_ptr<int>(m_alive)](Hypervisor::Result result) {
              if (!alive.expired()) {
                snapshotDone(std::move(result));
              }
            });
            requestUpdate();
          },
      .deleteSnapshot =
          [this](std::string name) {
            m_machinePage->setBusy(true);
            m_services.hypervisor.deleteSnapshot(m_current, name, [this, alive = std::weak_ptr<int>(m_alive)](Hypervisor::Result result) {
              if (!alive.expired()) {
                snapshotDone(std::move(result));
              }
            });
            requestUpdate();
          },
      .clone = [this](std::string name, bool linked) { cloneMachine(name, linked); },
      .usb = [this](HostUsbDevice device, bool connect) { connectUsb(device, connect); },
      .remove = [this](bool deleteFiles) { removeMachine(m_current, deleteFiles); },
      .installGraphicsDriver = [this] { installGraphicsDriver(); },
      .changed = [this] { requestUpdate(); },
  });
  m_machinePage = page.get();
  content->addChild(std::move(page));

  // A new machine from an installation medium.
  const auto field = [](const char* name, std::unique_ptr<Node> control) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceMd},
        plainLabel({.text = std::string(name), .fontSize = Style::fontSizeBody, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .minWidth = 150.0F}),
        std::move(control)
    );
  };
  auto fresh = ui::column({.out = &m_newPage, .align = FlexAlign::Stretch, .gap = Style::spaceMd, .padding = Style::spaceLg, .flexGrow = 1.0F});
  fresh->addChild(plainLabel({
      .text = "New virtual machine",
      .fontSize = Style::fontSizeHeader,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  fresh->addChild(field(
      "Medium or disk image",
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .flexGrow = 1.0F},
          ui::input({
              .out = &m_newIso,
              .placeholder = "An ISO image to install from, or a disk image to start from",
              .flexGrow = 1.0F,
              .onChange = [this](const std::string& text) { setInstallMedia(text); },
          }),
          ui::button({
              .text = "Browse …",
              .variant = ButtonVariant::Outline,
              .onClick =
                  [this] {
                    if (!m_services.portal.available()) {
                      m_message = "No file dialog is available on this desktop.";
                      requestUpdate();
                      return;
                    }
                    std::weak_ptr<int> alive = m_alive;
                    m_services.portal.openFiles(
                        m_surface.portalHandle(), std::filesystem::path(g_get_home_dir()),
                        [this, alive](std::vector<std::filesystem::path> files) {
                          if (!alive.expired() && !files.empty()) {
                            m_newIso->setValue(files.front().string());
                            setInstallMedia(files.front().string());
                            requestUpdate();
                          }
                        }
                    );
                  },
          })
      )
  ));
  fresh->addChild(field("", plainLabel({.out = &m_newDetected, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .flexGrow = 1.0F})));
  fresh->addChild(field(
      "Name",
      ui::input({
          .out = &m_newName,
          .placeholder = "What the machine is called",
          .flexGrow = 1.0F,
          .onChange = [this](const std::string& text) { m_newNameTyped = !text.empty(); },
      })
  ));
  fresh->addChild(field("Memory", ui::stepper({.out = &m_newMemory, .minValue = 1, .maxValue = static_cast<int>(std::max(1U, hostMemoryMiB() / 1024)), .value = 4, .valueSuffix = " GiB"})));
  fresh->addChild(field("Processors", ui::stepper({.out = &m_newCpus, .minValue = 1, .maxValue = static_cast<int>(hostProcessors()), .value = 2})));
  {
    auto row = field("New hard disk", ui::stepper({.out = &m_newDisk, .minValue = 8, .maxValue = 2048, .step = 8, .value = 40, .valueSuffix = " GiB"}));
    m_newDiskRow = row.get();
    fresh->addChild(std::move(row));
  }
  {
    auto row = field(
        "Keep the image in place",
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceMd, .flexGrow = 1.0F},
            ui::toggle({.out = &m_newInPlace, .onChange = [this](bool on) {
                          m_newKeepInPlace = on;
                          requestUpdate();
                        }}),
            plainLabel({
                .text = "Made at once: the machine keeps only its changes, and the image must stay where it is, unchanged. Otherwise the image is copied.",
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            })
        )
    );
    m_newInPlaceRow = row.get();
    fresh->addChild(std::move(row));
  }
  m_hostSetup = hostGuestSetup();
  {
    auto row = field(
        "Install automatically",
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceMd, .flexGrow = 1.0F},
            ui::toggle({.out = &m_newUnattendedToggle, .checked = true, .onChange = [this](bool on) {
                          m_newUnattended = on;
                          requestUpdate();
                        }}),
            plainLabel({
                .text = std::format(
                    "Nobody needs to be at it. The guest takes this computer's keyboard layout, language and time zone, here {}, {} and {}.",
                    m_hostSetup.keyboard, m_hostSetup.language, m_hostSetup.timezone
                ),
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            })
        )
    );
    m_newUnattendedRow = row.get();
    fresh->addChild(std::move(row));
  }
  {
    auto row = field("User name", ui::input({.out = &m_newUser, .value = m_hostSetup.user, .flexGrow = 1.0F}));
    m_newUserRow = row.get();
    fresh->addChild(std::move(row));
  }
  {
    auto row = field(
        "Password",
        ui::input({.out = &m_newPassword, .placeholder = "Also the guest's root password", .flexGrow = 1.0F, .configure = [](Input& input) {
                     input.setPasswordMode(true);
                   }})
    );
    m_newPasswordRow = row.get();
    fresh->addChild(std::move(row));
  }
  fresh->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      ui::button({.out = &m_newCreate, .text = "Create", .glyph = "plus", .variant = ButtonVariant::Primary, .onClick = [this] { createNew(); }}),
      ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = [this] {
                    m_newShown = false;
                    m_tabsStale = true;
                    requestUpdate();
                  }})
  ));
  content->addChild(std::move(fresh));

  content->addChild(ui::column({.out = &m_consoles, .align = FlexAlign::Stretch, .flexGrow = 1.0F}));

  content->addChild(ui::row(
      {.out = &m_statusBar,
       .align = FlexAlign::Center,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kStatusHeight},
      plainLabel({
          .out = &m_status,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  ));
  m_root->addChild(std::move(content));
}

void coffret::MainWindow::syncList() {
  clear(m_list);
  clear(m_homeList);
  const auto& machines = m_services.hypervisor.machines();
  for (const auto& machine : machines) {
    if (!m_query.empty() && lower(machine.name).find(m_query) == std::string::npos) {
      continue;
    }
    const bool selected = machine.uuid == m_current;
    m_list->addChild(ui::button({
        .text = machine.name,
        .glyph = std::string(glyphOf(machine.state)),
        .selected = selected,
        .contentAlign = ButtonContentAlign::Start,
        .variant = selected ? ButtonVariant::Secondary : ButtonVariant::Ghost,
        .tooltip = std::string(toString(machine.state)),
        .onClick = [this, uuid = machine.uuid] { openTab(uuid); },
    }));
    m_homeList->addChild(ui::button({
        .text = std::format(
            "{}   ·   {}   ·   {}, {} processors", machine.name, toString(machine.state), memoryText(machine.config.memoryMiB),
            machine.config.cpus
        ),
        .glyph = std::string(glyphOf(machine.state)),
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Outline,
        .onClick = [this, uuid = machine.uuid] { openTab(uuid); },
    }));
  }
  const Hypervisor& hypervisor = m_services.hypervisor;
  m_connection->setText(hypervisor.connected() ? "" : hypervisor.problem().empty() ? "Connecting to libvirt …" : hypervisor.problem());
  m_homeEmpty->setText(
      !hypervisor.connected() ? "Waiting for libvirt." : machines.empty() ? "There are no virtual machines yet." : ""
  );
  show(m_homeEmpty, !hypervisor.connected() || machines.empty());
  m_listStale = false;
}

void coffret::MainWindow::syncTabs() {
  clear(m_tabBar);
  m_tabBar->addChild(ui::button({
      .text = "Home",
      .glyph = "home",
      .selected = m_current.empty(),
      .variant = m_current.empty() ? ButtonVariant::Secondary : ButtonVariant::Ghost,
      .onClick = [this] { showHome(); },
  }));
  for (const auto& tab : m_tabs) {
    const Machine* machine = m_services.hypervisor.find(tab->uuid);
    const bool selected = tab->uuid == m_current;
    m_tabBar->addChild(ui::button({
        .text = machine ? machine->name : std::string("…"),
        .glyph = std::string(machine ? glyphOf(machine->state) : "device-desktop"),
        .selected = selected,
        .variant = selected ? ButtonVariant::Secondary : ButtonVariant::Ghost,
        .onClick = [this, uuid = tab->uuid] { openTab(uuid); },
    }));
    m_tabBar->addChild(ui::button({
        .glyph = "x",
        .variant = ButtonVariant::Ghost,
        .tooltip = "Close the tab; the machine goes on running",
        .onClick =
            [this, uuid = tab->uuid] {
              const auto it = std::ranges::find_if(m_tabs, [&uuid](const auto& other) { return other->uuid == uuid; });
              if (it != m_tabs.end()) {
                closeTab(static_cast<std::size_t>(it - m_tabs.begin()));
              }
            },
    }));
  }
  m_tabsStale = false;
}

void coffret::MainWindow::syncPage() {
  // Pictures of tabs opened since the last frame.
  for (auto& tab : m_tabs) {
    if (!tab->picture) {
      auto picture = tab->console->build();
      picture->setFlexGrow(1.0F);
      tab->picture = picture.get();
      m_consoles->addChild(std::move(picture));
    }
  }
  const Machine* machine = m_newShown ? nullptr : currentMachine();
  Tab* tab = m_newShown ? nullptr : currentTab();
  const bool console = tab && machine && isActive(machine->state) && !tab->details;
  const bool fullscreen = m_surface.fullscreen() && console;
  // In full screen and in the bare window the picture is all there is.
  const bool bare = (fullscreen || m_bare) && console;
  for (auto& other : m_tabs) {
    const bool shown = console && other.get() == tab;
    show(other->picture, shown);
    other->console->setActive(shown);
  }
  show(m_home, !machine && !m_newShown);
  show(m_newPage, m_newShown);
  m_newCreate->setEnabled(!m_creating && !m_newName->value().empty() && !m_newMediaPath.empty() && m_newSource != SourceKind::Unknown);
  show(m_newDiskRow, m_newSource != SourceKind::DiskImage);
  show(m_newInPlaceRow, m_newSource == SourceKind::DiskImage);
  const bool unattended = m_newSource == SourceKind::InstallMedium && m_newUnattendedPossible;
  show(m_newUnattendedRow, unattended);
  show(m_newUserRow, unattended && m_newUnattended);
  show(m_newPasswordRow, unattended && m_newUnattended);
  m_newUnattendedToggle->setCheckedImmediate(m_newUnattended);
  m_newCreate->setText(m_creating ? "Creating …" : "Create");
  show(m_machinePage, machine && !console);
  if (machine && !console) {
    m_machinePage->setMachine(*machine);
    if (m_usbShownFor != machine->uuid || !isActive(machine->state)) {
      m_usbShownFor = machine->uuid;
      m_machinePage->setConnectedUsb(m_usbConnected[machine->uuid]);
    }
    if (m_snapshotsOf != machine->uuid) {
      loadSnapshots();
    }
    m_machinePage->sync();
  }
  show(m_consoles, console);
  show(m_sidebar, !bare);
  show(m_tabBar, !bare);
  if (!bare) {
    m_barRevealed = false;
  }
  show(m_toolbar, (!bare || m_barRevealed) && machine);
  m_toolbar->setParticipatesInLayout(!bare);
  m_toolbar->setZIndex(bare ? 10 : 0);
  show(m_statusBar, !bare);

  if (machine) {
    const MachineState state = machine->state;
    const bool active = isActive(state);
    m_startButton->setEnabled(!active);
    m_pauseButton->setEnabled(state == MachineState::Running || state == MachineState::Paused);
    m_pauseButton->setGlyph(state == MachineState::Paused ? "player-play" : "player-pause");
    m_pauseButton->setTooltip(state == MachineState::Paused ? "Resume" : "Pause");
    m_suspendButton->setEnabled(state == MachineState::Running || state == MachineState::Paused);
    m_shutDownButton->setEnabled(state == MachineState::Running);
    m_forceOffButton->setEnabled(active);
    m_resetButton->setEnabled(active);
    m_keysButton->setEnabled(console);
    m_fullscreenButton->setEnabled(console);
    m_bareButton->setEnabled(console);
    show(m_bareButton, !fullscreen);
    m_bareButton->setGlyph(m_bare ? "arrows-minimize" : "arrows-maximize");
    m_bareButton->setTooltip(m_bare ? "Show the window's controls (Shift+F11)" : "Only the guest's picture (Shift+F11)");
    m_fullscreenButton->setGlyph(fullscreen ? "minimize" : "maximize");
    m_fullscreenButton->setTooltip(fullscreen ? "Leave full screen (F11)" : "Full screen (F11)");
    m_detailsButton->setEnabled(isActive(state));
    m_detailsButton->setSelected(tab && tab->details && isActive(state));
  }
  std::string status = m_message;
  if (status.empty() && console) {
    status = !tab->displayProblem.empty() && !tab->console->attached() ? tab->displayProblem : tab->console->describe();
  } else if (status.empty() && machine) {
    status = toString(machine->state);
  }
  m_status->setText(status);
}

void coffret::MainWindow::prepare() {
  if (!m_surface.isRunning() || !m_services.renderer.makeCurrent(m_surface.renderTarget())) {
    return;
  }
  UiPhaseScope phase(UiPhase::Layout);
  if (!m_root) {
    build();
  }
  if (m_listStale) {
    syncList();
  }
  if (m_tabsStale) {
    syncTabs();
  }
  syncPage();
  auto& renderer = m_surface.renderTarget().renderer();
  m_root->arrange(renderer, {0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())});
  if (!m_toolbar->participatesInLayout() && m_toolbar->visible()) {
    m_toolbar->arrange(renderer, {0, 0, static_cast<float>(m_surface.width()), kToolbarHeight});
  }
}

// ── Input ────────────────────────────────────────────────────────────────────

void coffret::MainWindow::hostCopied(const std::string& text) {
  m_hostClipboard = text;
  // Every open machine may paste it, as every window of the desktop may.
  for (auto& tab : m_tabs) {
    tab->console->hostCopied(text);
  }
}

bool coffret::MainWindow::keysToGuest() {
  ConsoleView* console = shownConsole();
  if (!console) {
    return false;
  }
  const InputArea* focused = m_dispatcher.focusedArea();
  return console->grabbed() || !(focused && focused->textInputClient());
}

void coffret::MainWindow::rawKey(std::uint32_t evdev, bool pressed) {
  if (ConsoleView* console = shownConsole()) {
    const bool toGuest = keysToGuest();
    // F11 stays ours while the guest has the keyboard, else full screen has no way out; with
    // Shift it hides the window's controls.
    if (evdev == KEY_F11 && toGuest) {
      if (pressed && (console->held(KEY_LEFTSHIFT) || console->held(KEY_RIGHTSHIFT))) {
        toggleBare();
      } else if (pressed) {
        toggleFullscreen();
      }
      return;
    }
    console->key(evdev, pressed, toGuest);
  }
}

void coffret::MainWindow::keyboard(const KeyboardEvent& event) {
  // The guest has the keyboard: nothing here is ours.
  if (keysToGuest()) {
    return;
  }
  if (event.pressed && !event.preedit) {
    const std::uint32_t sym = event.sym;
    if (event.modifiers == 0 && sym == XKB_KEY_F11) {
      toggleFullscreen();
      return;
    }
    if (event.modifiers == KeyMod::Shift && sym == XKB_KEY_F11) {
      toggleBare();
      return;
    }
    if (event.modifiers == KeyMod::Ctrl && (sym == XKB_KEY_f || sym == XKB_KEY_F)) {
      m_dispatcher.setFocus(m_search->inputArea());
      requestUpdate();
      return;
    }
    if (event.modifiers == KeyMod::Ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W) && !m_current.empty()) {
      action("close-tab", "");
      return;
    }
    if (event.modifiers == KeyMod::Ctrl && (sym == XKB_KEY_q || sym == XKB_KEY_Q)) {
      close();
      return;
    }
  }
  if (InputArea* focused = m_dispatcher.focusedArea(); focused && focused->capturesAllKeys()) {
    focused->dispatchKey(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  } else {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }
  requestUpdate();
}

void coffret::MainWindow::pointer(const PointerEvent& event) {
  if (event.surface != m_surface.wlSurface()) {
    return;
  }
  ConsoleView* console = shownConsole();
  // A locked pointer is the guest's alone.
  if (console && m_services.grab.pointerLocked()) {
    console->pointer(event);
    return;
  }
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  // In full screen and in the bare window the toolbar comes down while the pointer touches the top edge.
  if (console && (m_surface.fullscreen() || m_bare) && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
    const bool reveal = m_barRevealed ? y < kToolbarHeight + 8.0F : y < 2.0F;
    if (reveal != m_barRevealed) {
      m_barRevealed = reveal;
      requestUpdate();
    }
  }
  const bool overBar = m_barRevealed && y < kToolbarHeight && event.type != PointerEvent::Type::Leave;
  if (console && overBar && !m_pointerOverBar) {
    // The guest's pointer gives way to the host's over the bar.
    PointerEvent leave = event;
    leave.type = PointerEvent::Type::Leave;
    console->pointer(leave);
  }
  m_pointerOverBar = overBar;
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button: {
    const bool taken = m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    if (taken) {
      // A click on the window's own controls takes the keyboard back.
      if (console && event.pressed) {
        console->release();
      }
      requestUpdate();
      return;
    }
    if (!console && event.pressed && event.button == BTN_LEFT && !m_surface.serverDecorated() && y < kTabBarHeight) {
      m_surface.beginMove(event.serial);
    }
    break;
  }
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
  if (console && !overBar) {
    console->pointer(event);
    // A click into the picture takes the keyboard from a text field of the window too.
    if (console->grabbed() && m_dispatcher.focusedArea()) {
      m_dispatcher.setFocus(nullptr);
    }
  }
  requestUpdate();
}

void coffret::MainWindow::focus(wl_surface* surface, bool entered) {
  if (surface != m_surface.wlSurface()) {
    return;
  }
  if (entered) {
    m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);
  } else if (ConsoleView* console = shownConsole()) {
    console->focusLost();
  }
  requestUpdate();
}

void coffret::MainWindow::installGraphicsDriver() {
  const Machine* machine = currentMachine();
  const std::string folder = direct3dDriverFolder();
  if (!machine || folder.empty() || machine->state != MachineState::Running) {
    m_message = "The 3D driver goes into a running Windows machine, on a computer that has it.";
    requestUpdate();
    return;
  }
  m_machinePage->setBusy(true);
  m_message = std::format("Installing the 3D driver in {} …", machine->name);
  requestUpdate();
  m_services.hypervisor.runInWindows(
      machine->uuid, folder, windowsGraphicsDriverScript("(Join-Path $folder 'viogpu3d.inf')"),
      [this, alive = std::weak_ptr<int>(m_alive), name = machine->name](Hypervisor::Result result) {
        if (alive.expired()) {
          return;
        }
        m_machinePage->setBusy(false);
        m_message = result ? std::format("{} draws with the 3D driver now.", name) : result.error();
        requestUpdate();
      }
  );
}
