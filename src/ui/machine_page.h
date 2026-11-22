#pragma once

#include "libvirt/hypervisor.h"
#include "snapshot/snapshot_tree.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"

#include <functional>
#include <string>
#include <vector>

class Button;
class Input;
class Label;
class Stepper;
class Toggle;

namespace coffret {
  // What a machine is and what can be changed about it: its hardware, and its snapshots.
  class MachinePage final : public Flex {
  public:
    struct Callbacks {
      // The settings as they are to be from now on.
      std::function<void(VmConfig)> apply;
      std::function<void(std::string name)> takeSnapshot;
      std::function<void(std::string name)> revertTo;
      std::function<void(std::string name)> deleteSnapshot;
      // Something on the page changed and wants a new layout.
      std::function<void()> changed;
    };

    explicit MachinePage(Callbacks callbacks);

    // The machine to show. What the user changed and has not applied stays while it is the same machine.
    void setMachine(const Machine& machine);
    void setSnapshots(std::vector<Snapshot> snapshots);
    // An operation on the machine's snapshots is under way.
    void setBusy(bool busy);
    // Brings the controls up to date; in the layout phase, before arranging.
    void sync();

  private:
    void fill();
    void rebuildFacts();
    void rebuildSnapshots();

    Callbacks m_callbacks;
    ScrollViewState m_scroll;
    Machine m_machine;
    // The settings as the controls have them.
    VmConfig m_edit;
    std::vector<Snapshot> m_snapshots;
    // The snapshot a row asks about, and what: "revert" or "delete".
    std::string m_askName, m_askWhat;
    bool m_busy = false;
    bool m_factsStale = true;
    bool m_snapshotsStale = true;

    Label* m_name = nullptr;
    Label* m_state = nullptr;
    Stepper* m_memory = nullptr;
    Stepper* m_cpus = nullptr;
    Toggle* m_accel = nullptr;
    Toggle* m_sound = nullptr;
    Toggle* m_clipboard = nullptr;
    Input* m_cdrom = nullptr;
    Flex* m_facts = nullptr;
    Button* m_apply = nullptr;
    Label* m_applyNote = nullptr;
    Input* m_snapshotName = nullptr;
    Button* m_take = nullptr;
    Label* m_snapshotNote = nullptr;
    Flex* m_snapshotRows = nullptr;
  };
} // namespace coffret
