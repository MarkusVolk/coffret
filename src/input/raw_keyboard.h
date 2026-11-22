#pragma once

#include "wayland/wayland_extension.h"

#include <cstdint>
#include <functional>
#include <unordered_set>

struct wl_keyboard;
struct wl_surface;
class WaylandConnection;

namespace coffret {
  // Keys as the keyboard sends them: the Linux key code of every press and release, with no
  // layout, no compose sequences and no repeats in between. A guest wants exactly that; the
  // toolkit's own key events have been through all three. Bound as a second wl_keyboard next to
  // the toolkit's.
  class RawKeyboard final : public WaylandExtension {
  public:
    // pressed keys are told again as released when the surface loses the keyboard.
    using KeyHandler = std::function<void(wl_surface* surface, std::uint32_t key, bool pressed)>;
    using FocusHandler = std::function<void(wl_surface* surface, bool entered)>;

    explicit RawKeyboard(WaylandConnection& connection);
    ~RawKeyboard() override;

    void setHandlers(KeyHandler key, FocusHandler focus);
    wl_surface* focus() const noexcept { return m_focus; }
    // The serial of the last key event, for requests that need one.
    std::uint32_t serial() const noexcept { return m_serial; }

    void onGlobal(wl_registry*, std::uint32_t, std::string_view, std::uint32_t) override {}
    void releaseGlobals() override;

    // wl_keyboard events.
    void enter(std::uint32_t serial, wl_surface* surface);
    void leave(std::uint32_t serial);
    void key(std::uint32_t serial, std::uint32_t key, bool pressed);

  private:
    wl_keyboard* m_keyboard = nullptr;
    wl_surface* m_focus = nullptr;
    std::uint32_t m_serial = 0;
    std::unordered_set<std::uint32_t> m_pressed;
    KeyHandler m_onKey;
    FocusHandler m_onFocus;
  };
} // namespace coffret
