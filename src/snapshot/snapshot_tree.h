#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace coffret {
  // A state of a machine that can be gone back to.
  struct Snapshot {
    std::string name;
    // The snapshot this one was taken after; empty for a first one.
    std::string parent;
    std::string description;
    // Seconds since the epoch.
    std::int64_t created = 0;
    // Taken while the machine ran: going back to it goes back into the running machine.
    bool running = false;
    // The one the machine's present state comes from.
    bool current = false;
    bool operator==(const Snapshot&) const = default;
  };

  // A snapshot as libvirt describes it (virDomainSnapshotGetXMLDesc).
  std::expected<Snapshot, std::string> parseSnapshotXml(std::string_view xml);
  // What libvirt needs to take one.
  std::string snapshotXml(const std::string& name, const std::string& description);

  struct SnapshotRow {
    Snapshot snapshot;
    // How far from a first snapshot.
    unsigned depth = 0;
    // "You are here" follows this row: the machine's present state comes from this snapshot.
    bool hereAfter = false;
    // Where "You are here" goes: in line after the end of a straight line, else one step in.
    unsigned hereDepth = 0;
  };

  // The tree as rows from top to bottom: every snapshot before those taken after it, the older
  // branch first. Snapshots whose parent is missing count as first ones.
  std::vector<SnapshotRow> snapshotRows(std::vector<Snapshot> snapshots);

  // "Snapshot 3" when "Snapshot 1" and "Snapshot 2" are taken.
  std::string nextSnapshotName(const std::vector<Snapshot>& snapshots);
} // namespace coffret
