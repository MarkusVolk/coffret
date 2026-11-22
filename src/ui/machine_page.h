#pragma once

#include "libvirt/hypervisor.h"
#include "snapshot/snapshot_tree.h"
#include "usb/usb_devices.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Button;
class Input;
class Select;
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
      // A second machine made from this one; linked: sharing what does not differ.
      std::function<void(std::string name, bool linked)> clone;
      // Gives a device of the host to the running machine, or takes it back.
      std::function<void(HostUsbDevice device, bool connect)> usb;
      // Out of the library; deleteFiles: with the machine's own folder and the disks in it.
      std::function<void(bool deleteFiles)> remove;
      // Puts the viogpu3d driver into the running Windows guest.
      std::function<void()> installGraphicsDriver;
      // Something on the page changed and wants a new layout.
      std::function<void()> changed;
    };

    explicit MachinePage(Callbacks callbacks);

    // The machine to show. What the user changed and has not applied stays while it is the same machine.
    void setMachine(const Machine& machine);
    void setSnapshots(std::vector<Snapshot> snapshots);
    // The devices of the host the machine has right now.
    void setConnectedUsb(std::vector<HostUsbDevice> devices);
    // An operation on the machine's snapshots is under way.
    void setBusy(bool busy);
    // Whether the running Windows machine draws with viogpu3d already.
    void setGraphicsDriverLoaded(bool loaded);
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
    bool m_graphicsDriverLoaded = false;
    bool m_factsStale = true;
    bool m_snapshotsStale = true;

    Label* m_name = nullptr;
    Label* m_state = nullptr;
    Stepper* m_memory = nullptr;
    Stepper* m_cpus = nullptr;
    Toggle* m_accel = nullptr;
    Label* m_accelNote = nullptr;
    Button* m_installDriver = nullptr;
    Toggle* m_venus = nullptr;
    Toggle* m_sound = nullptr;
    Toggle* m_microphone = nullptr;
    Toggle* m_tpm = nullptr;
    Toggle* m_secureBoot = nullptr;
    Toggle* m_clipboard = nullptr;
    Input* m_cdrom = nullptr;
    Input* m_forwards = nullptr;
    Label* m_forwardsNote = nullptr;
    Flex* m_forwardsRow = nullptr;
    Select* m_network = nullptr;
    Label* m_networkNote = nullptr;
    // The bridges the network choice lists after NAT, in its order.
    std::vector<std::string> m_bridges;
    // What keeps the machine off its bridge, looked up again every few seconds.
    std::string m_bridgeChecked, m_bridgeProblem;
    std::chrono::steady_clock::time_point m_bridgeCheckedAt{};
    void fillNetwork();
    std::vector<std::string> bridgeChoices() const;
    Flex* m_facts = nullptr;
    Button* m_apply = nullptr;
    Label* m_applyNote = nullptr;
    Input* m_snapshotName = nullptr;
    Button* m_take = nullptr;
    Label* m_snapshotNote = nullptr;
    Flex* m_snapshotRows = nullptr;
    Input* m_cloneName = nullptr;
    Toggle* m_cloneLinked = nullptr;
    Button* m_clone = nullptr;
    Label* m_cloneNote = nullptr;
    bool m_linked = false;
    Flex* m_removeRow = nullptr;
    bool m_removeAsked = false;
    bool m_removeStale = true;
    void rebuildRemove();
    void rebuildFolders();
    void rebuildUsb();
    Flex* m_usbRows = nullptr;
    Label* m_usbNote = nullptr;
    std::vector<HostUsbDevice> m_usbDevices;
    std::vector<HostUsbDevice> m_usbConnected;
    // A keyboard or mouse whose Connect waits for a second word.
    std::optional<HostUsbDevice> m_usbAsk;
    std::chrono::steady_clock::time_point m_usbScanned{};
    bool m_usbStale = true;
    Flex* m_folderRows = nullptr;
    Input* m_folderPath = nullptr;
    bool m_foldersStale = true;
  };
} // namespace coffret
