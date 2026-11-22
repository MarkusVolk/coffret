#pragma once

#include "vm/vm_config.h"

#include <expected>
#include <string>
#include <string_view>

namespace coffret {
  // What a domain needs to know about the session it runs in.
  struct DomainEnvironment {
    // The program that serves shared folders, for a libvirt that is not told about it by a
    // description under /usr/share/qemu/vhost-user; empty leaves the choice to libvirt.
    std::string virtiofsd;
    // The user the machines run as: a shared folder's files belong to them, whoever is root in
    // the guest. -1 leaves the mapping to libvirt, which then needs /etc/subuid and newuidmap.
    long uid = -1;
    long gid = -1;
    // $XDG_RUNTIME_DIR, where QEMU finds the user's PipeWire.
    std::string runtimeDir;
  };

  // A new libvirt domain for the machine: q35 with KVM, virtio devices, a D-Bus display that is
  // reached peer to peer, PipeWire sound and shared memory, so folders can be shared later.
  std::string buildDomainXml(const VmConfig& config, const DomainEnvironment& environment);

  // The settings of `config` written into an existing domain; whatever coffret does not manage,
  // down to device addresses, stays as it is.
  std::expected<std::string, std::string>
  updateDomainXml(std::string_view xml, const VmConfig& config, const DomainEnvironment& environment);

  // What coffret manages of a domain, read back. Any libvirt domain can be read; what it has
  // beyond that is ignored.
  std::expected<VmConfig, std::string> readDomainXml(std::string_view xml);
} // namespace coffret
