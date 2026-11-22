#pragma once

#include "wayland/wayland_extension.h"

#include <cstdint>
#include <functional>

struct wl_surface;
struct zwp_confined_pointer_v1;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;
struct zwp_locked_pointer_v1;
struct zwp_pointer_constraints_v1;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;
class WaylandConnection;

namespace coffret {
  // What it takes to hand the keyboard and the mouse to a guest: the compositor's own shortcuts
  // are switched off for the surface, so Super and Alt+Tab reach the guest, and the pointer can be
  // locked in place while it reports how far the mouse moved, for a guest that has a plain mouse
  // and no tablet.
  class InputGrab final : public WaylandExtension {
  public:
    using Motion = std::function<void(double dx, double dy)>;

    explicit InputGrab(WaylandConnection& connection);
    ~InputGrab() override;

    bool canInhibitShortcuts() const noexcept { return m_inhibitManager != nullptr; }
    bool canLockPointer() const noexcept { return m_constraints != nullptr && m_relativeManager != nullptr; }

    void inhibitShortcuts(wl_surface* surface);
    void releaseShortcuts();
    bool shortcutsInhibited() const noexcept { return m_inhibitor != nullptr; }

    // Locks the pointer where it is; `motion` gets the unaccelerated movement of the mouse.
    void lockPointer(wl_surface* surface, Motion motion);
    void unlockPointer();
    bool pointerLocked() const noexcept { return m_lock != nullptr; }

    void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
    void onGlobalRemove(std::uint32_t name) override;
    void releaseGlobals() override;

    // zwp_relative_pointer_v1 event.
    void relativeMotion(double dx, double dy);

  private:
    WaylandConnection& m_connection;
    zwp_keyboard_shortcuts_inhibit_manager_v1* m_inhibitManager = nullptr;
    zwp_pointer_constraints_v1* m_constraints = nullptr;
    zwp_relative_pointer_manager_v1* m_relativeManager = nullptr;
    std::uint32_t m_inhibitGlobal = 0, m_constraintsGlobal = 0, m_relativeGlobal = 0;
    zwp_keyboard_shortcuts_inhibitor_v1* m_inhibitor = nullptr;
    zwp_locked_pointer_v1* m_lock = nullptr;
    zwp_relative_pointer_v1* m_relative = nullptr;
    Motion m_motion;
  };
} // namespace coffret
