#include "input/raw_keyboard.h"

#include "wayland/wayland_connection.h"

#include <unistd.h>
#include <wayland-client.h>

namespace {
  void keymap(void*, wl_keyboard*, std::uint32_t, std::int32_t fd, std::uint32_t) {
    // The layout is the guest's business.
    ::close(fd);
  }
  void enter(void* data, wl_keyboard*, std::uint32_t serial, wl_surface* surface, wl_array*) {
    // The keys held at this moment were pressed for somebody else: the guest never sees them.
    static_cast<coffret::RawKeyboard*>(data)->enter(serial, surface);
  }
  void leave(void* data, wl_keyboard*, std::uint32_t serial, wl_surface*) {
    static_cast<coffret::RawKeyboard*>(data)->leave(serial);
  }
  void key(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
    static_cast<coffret::RawKeyboard*>(data)->key(serial, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
  }
  void modifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
  void repeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t) {}

  const wl_keyboard_listener kListener = {
      .keymap = keymap, .enter = enter, .leave = leave, .key = key, .modifiers = modifiers, .repeat_info = repeatInfo
  };
} // namespace

coffret::RawKeyboard::RawKeyboard(WaylandConnection& connection) {
  if (wl_seat* seat = connection.seat()) {
    m_keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(m_keyboard, &kListener, this);
  }
}

coffret::RawKeyboard::~RawKeyboard() = default;

void coffret::RawKeyboard::releaseGlobals() {
  if (m_keyboard) {
    wl_keyboard_destroy(m_keyboard);
    m_keyboard = nullptr;
  }
}

void coffret::RawKeyboard::setHandlers(KeyHandler key, FocusHandler focus) {
  m_onKey = std::move(key);
  m_onFocus = std::move(focus);
}

void coffret::RawKeyboard::enter(std::uint32_t serial, wl_surface* surface) {
  m_serial = serial;
  m_focus = surface;
  m_pressed.clear();
  if (m_onFocus) {
    m_onFocus(surface, true);
  }
}

void coffret::RawKeyboard::leave(std::uint32_t serial) {
  m_serial = serial;
  wl_surface* surface = m_focus;
  // No release comes for a key that is still down when the window loses the keyboard; without
  // one the guest would keep it pressed for ever.
  const auto held = std::exchange(m_pressed, {});
  if (m_onKey) {
    for (const auto key : held) {
      m_onKey(surface, key, false);
    }
  }
  m_focus = nullptr;
  if (m_onFocus) {
    m_onFocus(surface, false);
  }
}

void coffret::RawKeyboard::key(std::uint32_t serial, std::uint32_t key, bool pressed) {
  m_serial = serial;
  if (pressed) {
    m_pressed.insert(key);
  } else if (m_pressed.erase(key) == 0) {
    // Released without a press seen here: it went down before the window had the keyboard.
    return;
  }
  if (m_onKey) {
    m_onKey(m_focus, key, pressed);
  }
}
