#include "snapshot/snapshot_tree.h"

#include <algorithm>
#include <format>
#include <functional>
#include <pugixml.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

std::expected<coffret::Snapshot, std::string> coffret::parseSnapshotXml(std::string_view xml) {
  pugi::xml_document document;
  const auto parsed = document.load_buffer(xml.data(), xml.size());
  const pugi::xml_node node = document.child("domainsnapshot");
  if (!parsed || !node) {
    return std::unexpected(std::string("not a snapshot"));
  }
  Snapshot snapshot;
  snapshot.name = node.child_value("name");
  snapshot.description = node.child_value("description");
  snapshot.parent = node.child("parent").child_value("name");
  snapshot.created = node.child("creationTime").text().as_llong();
  const std::string_view state = node.child_value("state");
  snapshot.running = state == "running" || state == "paused";
  return snapshot;
}

std::string coffret::snapshotXml(const std::string& name, const std::string& description) {
  pugi::xml_document document;
  pugi::xml_node node = document.append_child("domainsnapshot");
  node.append_child("name").text().set(name.c_str());
  if (!description.empty()) {
    node.append_child("description").text().set(description.c_str());
  }
  std::ostringstream out;
  document.save(out, "  ", pugi::format_indent | pugi::format_no_declaration);
  return out.str();
}

std::vector<coffret::SnapshotRow> coffret::snapshotRows(std::vector<Snapshot> snapshots) {
  std::ranges::sort(snapshots, [](const Snapshot& a, const Snapshot& b) {
    return a.created != b.created ? a.created < b.created : a.name < b.name;
  });
  std::unordered_set<std::string> names;
  for (const auto& snapshot : snapshots) {
    names.insert(snapshot.name);
  }
  std::unordered_map<std::string, unsigned> children;
  for (const auto& snapshot : snapshots) {
    if (names.contains(snapshot.parent)) {
      ++children[snapshot.parent];
    }
  }
  std::vector<SnapshotRow> rows;
  std::unordered_set<std::string> placed;
  // A straight line of snapshots stays in one column; only a branch moves in. That is safe for the last
  // branch of a level alone: after it nothing follows that could be taken for its sibling.
  const std::function<void(const Snapshot&, unsigned, bool)> place = [&](const Snapshot& snapshot, unsigned depth, bool last) {
    // A loop in what libvirt reports must not hang the window.
    if (!placed.insert(snapshot.name).second) {
      return;
    }
    const unsigned count = children[snapshot.name];
    const bool straight = count == 1 && last && !snapshot.current;
    rows.push_back({snapshot, depth, snapshot.current, count == 0 && last ? depth : depth + 1});
    unsigned seen = 0;
    for (const auto& child : snapshots) {
      if (child.parent == snapshot.name) {
        ++seen;
        place(child, straight ? depth : depth + 1, straight || seen == count);
      }
    }
  };
  for (const auto& snapshot : snapshots) {
    if (snapshot.parent.empty() || !names.contains(snapshot.parent)) {
      place(snapshot, 0, true);
    }
  }
  for (const auto& snapshot : snapshots) {
    place(snapshot, 0, true);
  }
  return rows;
}

std::string coffret::nextSnapshotName(const std::vector<Snapshot>& snapshots) {
  for (unsigned number = 1;; ++number) {
    std::string name = std::format("Snapshot {}", number);
    if (std::ranges::none_of(snapshots, [&name](const Snapshot& snapshot) { return snapshot.name == name; })) {
      return name;
    }
  }
}
