#include "libvirt/hypervisor.h"

#include "app/worker.h"
#include "core/log.h"
#include "vm/domain_xml.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
  const Logger kLog("libvirt");

  std::string lastError(const char* fallback) {
    const char* message = virGetLastErrorMessage();
    return message && *message ? message : fallback;
  }

  // libvirt's events need a loop of their own; it runs for as long as the program does.
  void startEventLoop() {
    static std::once_flag once;
    std::call_once(once, [] {
      // Errors are reported where they happen, not printed.
      virSetErrorFunc(nullptr, [](void*, virErrorPtr) {});
      virEventRegisterDefaultImpl();
      // Keeps the loop turning, so new handles are picked up.
      virEventAddTimeout(1000, [](int, void*) {}, nullptr, nullptr);
      std::thread([] {
        for (;;) {
          if (virEventRunDefaultImpl() < 0) {
            usleep(100 * 1000);
          }
        }
      }).detach();
    });
  }

  coffret::MachineState stateOf(virDomainPtr domain) {
    using coffret::MachineState;
    int state = VIR_DOMAIN_NOSTATE;
    virDomainGetState(domain, &state, nullptr, 0);
    switch (state) {
    case VIR_DOMAIN_RUNNING:
    case VIR_DOMAIN_BLOCKED:
      return MachineState::Running;
    case VIR_DOMAIN_PAUSED:
      return MachineState::Paused;
    case VIR_DOMAIN_SHUTDOWN:
      return MachineState::ShuttingDown;
    case VIR_DOMAIN_CRASHED:
      return MachineState::Crashed;
    case VIR_DOMAIN_PMSUSPENDED:
      return MachineState::Suspended;
    default:
      return virDomainHasManagedSaveImage(domain, 0) == 1 ? MachineState::Saved : MachineState::Off;
    }
  }

  std::string takeString(char* text) {
    std::string result = text ? text : "";
    std::free(text);
    return result;
  }
} // namespace

struct coffret::Hypervisor::Events {
  std::weak_ptr<int> alive;
  Hypervisor* self;
};

struct coffret::Hypervisor::Core {
  virConnectPtr connection = nullptr;
  int lifecycleCallback = -1;

  ~Core() {
    if (!connection) {
      return;
    }
    if (lifecycleCallback >= 0) {
      virConnectDomainEventDeregisterAny(connection, lifecycleCallback);
    }
    virConnectUnregisterCloseCallback(connection, closed);
    virConnectClose(connection);
  }

  static void closed(virConnectPtr, int reason, void* data) {
    auto* events = static_cast<Events*>(data);
    runOnMain([alive = events->alive, self = events->self, reason] {
      if (!alive.expired()) {
        self->lost(reason == VIR_CONNECT_CLOSE_REASON_CLIENT ? "" : "The connection to libvirt was lost.");
      }
    });
  }

  static int lifecycle(virConnectPtr, virDomainPtr, int, int, void* data) {
    auto* events = static_cast<Events*>(data);
    runOnMain([alive = events->alive, self = events->self] {
      if (!alive.expired()) {
        self->refresh();
      }
    });
    return 0;
  }
};

const char* coffret::toString(MachineState state) {
  switch (state) {
  case MachineState::Off:
    return "Powered off";
  case MachineState::Saved:
    return "Suspended";
  case MachineState::Running:
    return "Running";
  case MachineState::Paused:
    return "Paused";
  case MachineState::Suspended:
    return "Sleeping";
  case MachineState::ShuttingDown:
    return "Shutting down";
  case MachineState::Crashed:
    return "Crashed";
  }
  return "";
}

coffret::Hypervisor::Hypervisor() { startEventLoop(); }

coffret::Hypervisor::~Hypervisor() {
  if (m_retryTimer != 0) {
    g_source_remove(m_retryTimer);
  }
}

void coffret::Hypervisor::changed() const {
  if (m_changed) {
    m_changed();
  }
}

const coffret::Machine* coffret::Hypervisor::find(const std::string& uuid) const {
  const auto it = std::ranges::find(m_machines, uuid, &Machine::uuid);
  return it == m_machines.end() ? nullptr : &*it;
}

void coffret::Hypervisor::connect() {
  if (m_core || m_connecting) {
    return;
  }
  m_connecting = true;
  std::weak_ptr<int> alive = m_alive;
  using Opened = std::expected<std::shared_ptr<Core>, std::string>;
  runInThread<Opened>(
      [this, alive]() -> Opened {
        const char* uri = std::getenv("COFFRET_LIBVIRT_URI");
        auto core = std::make_shared<Core>();
        core->connection = virConnectOpen(uri && *uri ? uri : "qemu:///session");
        if (!core->connection) {
          return std::unexpected(lastError("libvirt cannot be reached."));
        }
        virConnectSetKeepAlive(core->connection, 5, 3);
        const auto free = [](void* data) { delete static_cast<Events*>(data); };
        virConnectRegisterCloseCallback(core->connection, Core::closed, new Events{alive, this}, free);
        core->lifecycleCallback = virConnectDomainEventRegisterAny(
            core->connection, nullptr, VIR_DOMAIN_EVENT_ID_LIFECYCLE, VIR_DOMAIN_EVENT_CALLBACK(Core::lifecycle),
            new Events{alive, this}, free
        );
        return core;
      },
      [this, alive](Opened opened) {
        if (alive.expired()) {
          return;
        }
        m_connecting = false;
        if (!opened) {
          lost(opened.error());
          return;
        }
        m_core = std::move(*opened);
        m_problem.clear();
        kLog.info("connected");
        refresh();
        changed();
      }
  );
}

void coffret::Hypervisor::lost(const std::string& why) {
  m_core.reset();
  m_machines.clear();
  if (!why.empty()) {
    m_problem = why;
    kLog.warn("{}", why);
  }
  if (m_retryTimer == 0) {
    m_retryTimer = g_timeout_add_seconds(
        3,
        [](gpointer data) -> gboolean {
          auto* self = static_cast<Hypervisor*>(data);
          self->m_retryTimer = 0;
          self->connect();
          return G_SOURCE_REMOVE;
        },
        this
    );
  }
  changed();
}

void coffret::Hypervisor::refresh() {
  if (!m_core) {
    return;
  }
  if (m_refreshing) {
    m_refreshAgain = true;
    return;
  }
  m_refreshing = true;
  std::weak_ptr<int> alive = m_alive;
  runInThread<std::vector<Machine>>(
      [core = m_core] {
        std::vector<Machine> machines;
        virDomainPtr* domains = nullptr;
        const int count = virConnectListAllDomains(core->connection, &domains, 0);
        for (int index = 0; index < count; ++index) {
          Machine machine;
          char uuid[VIR_UUID_STRING_BUFLEN] = {};
          virDomainGetUUIDString(domains[index], uuid);
          machine.uuid = uuid;
          machine.name = virDomainGetName(domains[index]);
          machine.state = stateOf(domains[index]);
          const std::string xml = takeString(virDomainGetXMLDesc(domains[index], VIR_DOMAIN_XML_INACTIVE));
          if (auto config = readDomainXml(xml)) {
            machine.config = std::move(*config);
          }
          machines.push_back(std::move(machine));
          virDomainFree(domains[index]);
        }
        std::free(domains);
        std::ranges::sort(machines, {}, &Machine::name);
        return machines;
      },
      [this, alive](std::vector<Machine> machines) {
        if (alive.expired()) {
          return;
        }
        m_refreshing = false;
        if (m_core) {
          m_machines = std::move(machines);
          changed();
        }
        if (m_refreshAgain) {
          m_refreshAgain = false;
          refresh();
        }
      }
  );
}

template <typename Value, typename Call>
void coffret::Hypervisor::run(
    const std::string& uuid, Call call, std::function<void(std::expected<Value, std::string>)> done
) {
  using Outcome = std::expected<Value, std::string>;
  if (!m_core) {
    done(std::unexpected(std::string("There is no connection to libvirt.")));
    return;
  }
  std::weak_ptr<int> alive = m_alive;
  runInThread<Outcome>(
      [core = m_core, uuid, call = std::move(call)]() -> Outcome {
        virDomainPtr domain = virDomainLookupByUUIDString(core->connection, uuid.c_str());
        if (!domain) {
          return std::unexpected(lastError("The machine is gone."));
        }
        Outcome outcome = call(domain);
        virDomainFree(domain);
        return outcome;
      },
      [this, alive, done = std::move(done)](Outcome outcome) {
        if (alive.expired()) {
          return;
        }
        refresh();
        done(std::move(outcome));
      }
  );
}

namespace {
  // A libvirt call that answers 0 or -1.
  template <typename Call> auto plain(Call call, const char* fallback) {
    return [call, fallback](virDomainPtr domain) -> coffret::Hypervisor::Result {
      if (call(domain) < 0) {
        return std::unexpected(lastError(fallback));
      }
      return {};
    };
  }
} // namespace

void coffret::Hypervisor::start(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainCreateWithFlags(domain, 0); }, "The machine did not start."), std::move(done));
}

void coffret::Hypervisor::shutDown(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainShutdown(domain); }, "The guest could not be asked to shut down."), std::move(done));
}

void coffret::Hypervisor::forceOff(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainDestroyFlags(domain, 0); }, "The machine could not be switched off."), std::move(done));
}

void coffret::Hypervisor::reset(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainReset(domain, 0); }, "The machine could not be reset."), std::move(done));
}

void coffret::Hypervisor::pause(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainSuspend(domain); }, "The machine could not be paused."), std::move(done));
}

void coffret::Hypervisor::resume(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainResume(domain); }, "The machine could not be resumed."), std::move(done));
}

void coffret::Hypervisor::saveState(const std::string& uuid, Done done) {
  run<void>(uuid, plain([](virDomainPtr domain) { return virDomainManagedSave(domain, 0); }, "The machine's state could not be saved."), std::move(done));
}

void coffret::Hypervisor::remove(const std::string& uuid, Done done) {
  run<void>(
      uuid,
      plain(
          [](virDomainPtr domain) {
            return virDomainUndefineFlags(
                domain, VIR_DOMAIN_UNDEFINE_MANAGED_SAVE | VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA
                            | VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA | VIR_DOMAIN_UNDEFINE_NVRAM
            );
          },
          "The machine could not be removed."
      ),
      std::move(done)
  );
}

void coffret::Hypervisor::xml(
    const std::string& uuid, bool running, std::function<void(std::expected<std::string, std::string>)> done
) {
  run<std::string>(
      uuid,
      [running](virDomainPtr domain) -> std::expected<std::string, std::string> {
        char* text = virDomainGetXMLDesc(domain, running ? 0 : VIR_DOMAIN_XML_INACTIVE);
        if (!text) {
          return std::unexpected(lastError("The machine's settings could not be read."));
        }
        return takeString(text);
      },
      std::move(done)
  );
}

void coffret::Hypervisor::snapshots(
    const std::string& uuid, std::function<void(std::expected<std::vector<Snapshot>, std::string>)> done
) {
  run<std::vector<Snapshot>>(
      uuid,
      [](virDomainPtr domain) -> std::expected<std::vector<Snapshot>, std::string> {
        virDomainSnapshotPtr* list = nullptr;
        const int count = virDomainListAllSnapshots(domain, &list, 0);
        if (count < 0) {
          return std::unexpected(lastError("The snapshots could not be read."));
        }
        std::vector<Snapshot> snapshots;
        for (int index = 0; index < count; ++index) {
          if (auto snapshot = parseSnapshotXml(takeString(virDomainSnapshotGetXMLDesc(list[index], 0)))) {
            snapshot->current = virDomainSnapshotIsCurrent(list[index], 0) == 1;
            snapshots.push_back(std::move(*snapshot));
          }
          virDomainSnapshotFree(list[index]);
        }
        std::free(list);
        return snapshots;
      },
      std::move(done)
  );
}

void coffret::Hypervisor::takeSnapshot(
    const std::string& uuid, const std::string& name, const std::string& description, Done done
) {
  run<void>(
      uuid,
      [xml = snapshotXml(name, description)](virDomainPtr domain) -> Result {
        virDomainSnapshotPtr snapshot = virDomainSnapshotCreateXML(domain, xml.c_str(), 0);
        if (!snapshot) {
          return std::unexpected(lastError("The snapshot could not be taken."));
        }
        virDomainSnapshotFree(snapshot);
        return {};
      },
      std::move(done)
  );
}

namespace {
  // A call on one snapshot of a domain that answers 0 or -1.
  template <typename Call> auto onSnapshot(std::string name, Call call, const char* fallback) {
    return [name = std::move(name), call, fallback](virDomainPtr domain) -> coffret::Hypervisor::Result {
      virDomainSnapshotPtr snapshot = virDomainSnapshotLookupByName(domain, name.c_str(), 0);
      if (!snapshot) {
        return std::unexpected(lastError("The snapshot is gone."));
      }
      const int result = call(snapshot);
      std::string error = result < 0 ? lastError(fallback) : std::string();
      virDomainSnapshotFree(snapshot);
      if (result < 0) {
        return std::unexpected(std::move(error));
      }
      return {};
    };
  }
} // namespace

void coffret::Hypervisor::revertToSnapshot(const std::string& uuid, const std::string& name, Done done) {
  run<void>(
      uuid,
      onSnapshot(name, [](virDomainSnapshotPtr snapshot) { return virDomainRevertToSnapshot(snapshot, 0); }, "The machine could not go back to the snapshot."),
      std::move(done)
  );
}

void coffret::Hypervisor::deleteSnapshot(const std::string& uuid, const std::string& name, Done done) {
  run<void>(
      uuid,
      onSnapshot(name, [](virDomainSnapshotPtr snapshot) { return virDomainSnapshotDelete(snapshot, 0); }, "The snapshot could not be deleted."),
      std::move(done)
  );
}

void coffret::Hypervisor::openDisplay(const std::string& uuid, std::function<void(std::expected<int, std::string>)> done) {
  run<int>(
      uuid,
      [](virDomainPtr domain) -> std::expected<int, std::string> {
        const int fd = virDomainOpenGraphicsFD(domain, 0, VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH);
        if (fd < 0) {
          return std::unexpected(lastError("The machine's display could not be opened."));
        }
        return fd;
      },
      std::move(done)
  );
}

void coffret::Hypervisor::define(std::string xml, std::function<void(std::expected<std::string, std::string>)> done) {
  using Outcome = std::expected<std::string, std::string>;
  if (!m_core) {
    done(std::unexpected(std::string("There is no connection to libvirt.")));
    return;
  }
  std::weak_ptr<int> alive = m_alive;
  runInThread<Outcome>(
      [core = m_core, xml = std::move(xml)]() -> Outcome {
        virDomainPtr domain = virDomainDefineXMLFlags(core->connection, xml.c_str(), VIR_DOMAIN_DEFINE_VALIDATE);
        if (!domain) {
          return std::unexpected(lastError("The machine could not be set up."));
        }
        char uuid[VIR_UUID_STRING_BUFLEN] = {};
        virDomainGetUUIDString(domain, uuid);
        virDomainFree(domain);
        return std::string(uuid);
      },
      [this, alive, done = std::move(done)](Outcome outcome) {
        if (alive.expired()) {
          return;
        }
        refresh();
        done(std::move(outcome));
      }
  );
}
