#include "input/input_grab.h"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <wayland-client.h>

namespace {
  void relativeMotion(
      void* data, zwp_relative_pointer_v1*, std::uint32_t, std::uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t dxUnaccelerated,
      wl_fixed_t dyUnaccelerated
  ) {
    static_cast<coffret::InputGrab*>(data)->relativeMotion(wl_fixed_to_double(dxUnaccelerated), wl_fixed_to_double(dyUnaccelerated));
  }
  const zwp_relative_pointer_v1_listener kRelativeListener = {.relative_motion = relativeMotion};
} // namespace

coffret::InputGrab::InputGrab(WaylandConnection& connection) : m_connection(connection) {}

coffret::InputGrab::~InputGrab() = default;

void coffret::InputGrab::onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t) {
  if (interface == zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name && !m_inhibitManager) {
    m_inhibitManager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1)
    );
    m_inhibitGlobal = name;
  } else if (interface == zwp_pointer_constraints_v1_interface.name && !m_constraints) {
    m_constraints =
        static_cast<zwp_pointer_constraints_v1*>(wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    m_constraintsGlobal = name;
  } else if (interface == zwp_relative_pointer_manager_v1_interface.name && !m_relativeManager) {
    m_relativeManager = static_cast<zwp_relative_pointer_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1)
    );
    m_relativeGlobal = name;
  }
}

void coffret::InputGrab::onGlobalRemove(std::uint32_t name) {
  if (name == m_inhibitGlobal && m_inhibitManager) {
    releaseShortcuts();
    zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(m_inhibitManager);
    m_inhibitManager = nullptr;
  } else if (name == m_constraintsGlobal && m_constraints) {
    unlockPointer();
    zwp_pointer_constraints_v1_destroy(m_constraints);
    m_constraints = nullptr;
  } else if (name == m_relativeGlobal && m_relativeManager) {
    unlockPointer();
    zwp_relative_pointer_manager_v1_destroy(m_relativeManager);
    m_relativeManager = nullptr;
  }
}

void coffret::InputGrab::releaseGlobals() {
  releaseShortcuts();
  unlockPointer();
  if (m_inhibitManager) {
    zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(m_inhibitManager);
    m_inhibitManager = nullptr;
  }
  if (m_constraints) {
    zwp_pointer_constraints_v1_destroy(m_constraints);
    m_constraints = nullptr;
  }
  if (m_relativeManager) {
    zwp_relative_pointer_manager_v1_destroy(m_relativeManager);
    m_relativeManager = nullptr;
  }
}

void coffret::InputGrab::inhibitShortcuts(wl_surface* surface) {
  if (m_inhibitor || !m_inhibitManager || !surface || !m_connection.seat()) {
    return;
  }
  m_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m_inhibitManager, surface, m_connection.seat());
}

void coffret::InputGrab::releaseShortcuts() {
  if (m_inhibitor) {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
  }
}

void coffret::InputGrab::lockPointer(wl_surface* surface, Motion motion) {
  wl_pointer* pointer = m_connection.pointer();
  if (m_lock || !canLockPointer() || !surface || !pointer) {
    return;
  }
  m_motion = std::move(motion);
  m_lock = zwp_pointer_constraints_v1_lock_pointer(
      m_constraints, surface, pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT
  );
  m_relative = zwp_relative_pointer_manager_v1_get_relative_pointer(m_relativeManager, pointer);
  zwp_relative_pointer_v1_add_listener(m_relative, &kRelativeListener, this);
}

void coffret::InputGrab::unlockPointer() {
  if (m_relative) {
    zwp_relative_pointer_v1_destroy(m_relative);
    m_relative = nullptr;
  }
  if (m_lock) {
    zwp_locked_pointer_v1_destroy(m_lock);
    m_lock = nullptr;
  }
  m_motion = {};
}

void coffret::InputGrab::relativeMotion(double dx, double dy) {
  if (m_lock && m_motion) {
    m_motion(dx, dy);
  }
}
