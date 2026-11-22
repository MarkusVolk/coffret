#pragma once

#include "vm/vm_config.h"

#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace coffret {
  // What it takes to make a second machine out of one that is off.
  struct ClonePlan {
    // The new machine; libvirt gives it a uuid and its network cards new addresses.
    VmConfig clone;
    // Files that change their name first: a linked clone turns each of the source's disks into a
    // base that neither machine writes to any more.
    std::vector<std::pair<std::string, std::string>> renames;
    // qemu-img command lines, in this order, after the renames.
    std::vector<std::vector<std::string>> commands;
    // Images copied whole where they exist, without the snapshots in them: the firmware's
    // variables, which a machine that never ran does not have yet.
    std::vector<std::pair<std::string, std::string>> copies;
    // Files that must not be written to from then on.
    std::vector<std::string> readOnly;
  };

  // A full clone copies every disk and stands alone. A linked clone is made at once and takes only
  // the room of what differs, but needs the source's folder for as long as it lives; the source
  // must then have no snapshots, as they would stay behind in the base. `folder` is the clone's own.
  std::expected<ClonePlan, std::string>
  planClone(const VmConfig& source, const std::string& name, const std::string& folder, bool linked, bool sourceHasSnapshots);
} // namespace coffret
