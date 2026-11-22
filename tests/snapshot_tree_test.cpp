#include "check.h"
#include "snapshot/snapshot_tree.h"

namespace {
  coffret::Snapshot snap(std::string name, std::string parent = "", std::int64_t created = 0, bool current = false) {
    coffret::Snapshot snapshot;
    snapshot.name = std::move(name);
    snapshot.parent = std::move(parent);
    snapshot.created = created;
    snapshot.current = current;
    return snapshot;
  }
} // namespace

int main() {
  const auto parsed = coffret::parseSnapshotXml(R"(<domainsnapshot>
  <name>Before update</name>
  <description>Clean &amp; tidy</description>
  <state>running</state>
  <parent><name>Fresh install</name></parent>
  <creationTime>1795000100</creationTime>
  <memory snapshot='internal'/>
  <domain type='kvm'><name>fedora</name></domain>
</domainsnapshot>)");
  CHECK(parsed.has_value());
  CHECK(parsed->name == "Before update" && parsed->parent == "Fresh install" && parsed->description == "Clean & tidy");
  CHECK(parsed->created == 1795000100 && parsed->running && !parsed->current);
  const auto off = coffret::parseSnapshotXml("<domainsnapshot><name>a</name><state>shutoff</state></domainsnapshot>");
  CHECK(off.has_value() && !off->running && off->parent.empty());
  CHECK(!coffret::parseSnapshotXml("<domain/>").has_value());

  const std::string xml = coffret::snapshotXml("Before <update>", "A & B");
  const auto back = coffret::parseSnapshotXml(xml);
  CHECK(back.has_value() && back->name == "Before <update>" && back->description == "A & B");
  CHECK(coffret::snapshotXml("a", "").find("description") == std::string::npos);

  //  first ─ second ─ fourth
  //        └ third (current)
  //  lost (its parent is gone)
  const std::vector<coffret::Snapshot> snapshots = {
      snap("third", "first", 30, true), snap("fourth", "second", 40), snap("first", "", 10),
      snap("lost", "deleted", 5),       snap("second", "first", 20),
  };
  const auto rows = coffret::snapshotRows(snapshots);
  CHECK(rows.size() == 5);
  CHECK(rows[0].snapshot.name == "lost" && rows[0].depth == 0);
  CHECK(rows[1].snapshot.name == "first" && rows[1].depth == 0);
  CHECK(rows[2].snapshot.name == "second" && rows[2].depth == 1);
  CHECK(rows[3].snapshot.name == "fourth" && rows[3].depth == 2);
  CHECK(rows[4].snapshot.name == "third" && rows[4].depth == 1 && rows[4].hereAfter);
  CHECK(!rows[1].hereAfter);
  CHECK(rows[4].hereDepth == 1);

  // A straight line stays in one column, also for "You are here" at its end.
  const auto line = coffret::snapshotRows({snap("a", "", 1), snap("b", "a", 2), snap("c", "b", 3, true)});
  CHECK(line.size() == 3 && line[0].depth == 0 && line[1].depth == 0 && line[2].depth == 0);
  CHECK(line[2].hereAfter && line[2].hereDepth == 0);
  // Gone back in the middle of it: the present branches off there.
  const auto middle = coffret::snapshotRows({snap("a", "", 1), snap("b", "a", 2, true), snap("c", "b", 3)});
  CHECK(middle[1].depth == 0 && middle[1].hereDepth == 1 && middle[2].depth == 1);

  // Parents that point at each other still give every snapshot one row.
  const auto loop = coffret::snapshotRows({snap("a", "b"), snap("b", "a")});
  CHECK(loop.size() == 2);
  CHECK(coffret::snapshotRows({}).empty());

  CHECK(coffret::nextSnapshotName({}) == "Snapshot 1");
  CHECK(coffret::nextSnapshotName({snap("Snapshot 1"), snap("Snapshot 2"), snap("mine")}) == "Snapshot 3");
  return 0;
}
