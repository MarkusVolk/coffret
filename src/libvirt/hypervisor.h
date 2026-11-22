#pragma once

#include "snapshot/snapshot_tree.h"
#include "vm/vm_config.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace coffret {
  enum class MachineState : std::uint8_t {
    Off,
    // Off, with its memory kept on disk: the next start goes on where it stopped.
    Saved,
    Running,
    Paused,
    // The guest put itself to sleep.
    Suspended,
    ShuttingDown,
    Crashed,
  };

  struct Machine {
    std::string uuid;
    std::string name;
    MachineState state = MachineState::Off;
    // The settings the next start uses.
    VmConfig config;
  };

  const char* toString(MachineState state);
  inline bool isActive(MachineState state) { return state != MachineState::Off && state != MachineState::Saved; }

  // The user's own libvirt (qemu:///session) and the machines it knows. Everything that talks to
  // libvirt runs on other threads; the list and the callbacks belong to the main thread.
  class Hypervisor {
  public:
    using Result = std::expected<void, std::string>;
    using Done = std::function<void(Result)>;

    Hypervisor();
    ~Hypervisor();
    Hypervisor(const Hypervisor&) = delete;
    Hypervisor& operator=(const Hypervisor&) = delete;

    // Called when the connection, the list or a machine's state changed.
    void setOnChanged(std::function<void()> changed) { m_changed = std::move(changed); }

    // Connects, and again by itself whenever the connection is lost.
    void connect();
    bool connected() const noexcept { return m_core != nullptr; }
    // Connected, and the machines have been read at least once.
    bool listed() const noexcept { return m_core != nullptr && m_listed; }
    // Why there is no connection; empty while connecting for the first time.
    const std::string& problem() const noexcept { return m_problem; }

    const std::vector<Machine>& machines() const noexcept { return m_machines; }
    const Machine* find(const std::string& uuid) const;

    void start(const std::string& uuid, Done done);
    // Asks the guest to shut down, as its power button would.
    void shutDown(const std::string& uuid, Done done);
    void forceOff(const std::string& uuid, Done done);
    void reset(const std::string& uuid, Done done);
    void pause(const std::string& uuid, Done done);
    void resume(const std::string& uuid, Done done);
    // Stops the machine with its memory kept on disk.
    void saveState(const std::string& uuid, Done done);

    // Defines a machine, or redefines the one of that uuid; hands back its uuid.
    void define(std::string xml, std::function<void(std::expected<std::string, std::string>)> done);
    // Forgets the machine with its snapshots and firmware variables. Its disks stay.
    void remove(const std::string& uuid, Done done);
    // The domain as libvirt has it: the settings of the next start, or the running machine.
    void xml(const std::string& uuid, bool running, std::function<void(std::expected<std::string, std::string>)> done);
    // The machine's snapshots, in no particular order.
    void snapshots(const std::string& uuid, std::function<void(std::expected<std::vector<Snapshot>, std::string>)> done);
    // Keeps the machine's present state, disks and, when it runs, memory.
    void takeSnapshot(const std::string& uuid, const std::string& name, const std::string& description, Done done);
    // Goes back to a snapshot; what happened since is lost unless it is in a snapshot of its own.
    void revertToSnapshot(const std::string& uuid, const std::string& name, Done done);
    // Forgets a snapshot; those taken after it stay.
    void deleteSnapshot(const std::string& uuid, const std::string& name, Done done);

    // A connection to the running machine's display, for QemuDisplay.
    void openDisplay(const std::string& uuid, std::function<void(std::expected<int, std::string>)> done);

  private:
    struct Core;
    struct Events;
    void refresh();
    void lost(const std::string& why);
    void changed() const;
    template <typename Value, typename Call>
    void run(const std::string& uuid, Call call, std::function<void(std::expected<Value, std::string>)> done);

    std::shared_ptr<Core> m_core;
    std::vector<Machine> m_machines;
    std::string m_problem;
    std::function<void()> m_changed;
    bool m_connecting = false;
    bool m_listed = false;
    bool m_refreshing = false;
    bool m_refreshAgain = false;
    unsigned m_retryTimer = 0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace coffret
