#include "ui/console_view.h"

#include "app/worker.h"
#include "core/log.h"
#include "display/console_node.h"
#include "input/input_grab.h"
#include "input/linux_to_qnum.h"
#include "platform/desktop_integration.h"
#include "wayland/wayland_connection.h"

#include <atomic>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>

namespace {
  const Logger kLog("console");

  std::uint32_t qnumOf(std::uint32_t evdev) { return evdev < coffret_linux_to_qnum_len ? coffret_linux_to_qnum[evdev] : 0; }

  // The button numbers of QEMU's display for those of Linux; nullopt for a button a guest has none of.
  std::optional<std::uint32_t> buttonOf(std::uint32_t button) {
    switch (button) {
    case BTN_LEFT:
      return 0;
    case BTN_MIDDLE:
      return 1;
    case BTN_RIGHT:
      return 2;
    case BTN_SIDE:
      return 5;
    case BTN_EXTRA:
      return 6;
    default:
      return std::nullopt;
    }
  }
} // namespace

coffret::ConsoleView::ConsoleView(Host host) : m_host(std::move(host)), m_frames(std::make_shared<FrameStore>()) {
  // A new picture arrives on the display's thread; the drawing happens here. A guest can change its
  // picture far more often than a frame is drawn: one wake-up at a time.
  std::weak_ptr<int> alive = m_alive;
  auto queued = std::make_shared<std::atomic<bool>>(false);
  m_frames->setOnChanged([this, alive, queued] {
    if (queued->exchange(true)) {
      return;
    }
    runOnMain([this, alive, queued] {
      queued->store(false);
      if (!alive.expired() && m_active) {
        m_host.surface.requestRedraw();
      }
    });
  });
  m_statusTimer.startRepeating(std::chrono::seconds(1), [this] {
    const auto now = std::chrono::steady_clock::now();
    if (m_node) {
      const auto frames = m_node->stats().frames;
      const double seconds = std::chrono::duration<double>(now - m_lastStatus).count();
      m_fps = seconds > 0 ? static_cast<double>(frames - m_framesAtLastStatus) / seconds : 0.0;
      m_framesAtLastStatus = frames;
    }
    m_lastStatus = now;
    if (m_active && m_display) {
      m_host.changed();
    }
  });
}

coffret::ConsoleView::~ConsoleView() {
  release();
  m_frames->setOnChanged({});
}

std::unique_ptr<Node> coffret::ConsoleView::build() {
  auto node = std::make_unique<ConsoleNode>(m_frames);
  m_node = node.get();
  return node;
}

void coffret::ConsoleView::attach(int busFd) {
  detach();
  m_problem.clear();
  m_display = std::make_unique<QemuDisplay>(
      busFd, m_frames,
      QemuDisplay::Callbacks{
          .ready =
              [this] {
                m_ready = true;
                m_askedWidth = m_askedHeight = 0;
                requestGuestSize();
                m_host.changed();
              },
          .closed =
              [this](const std::string& why) {
                m_ready = false;
                m_problem = why;
                release();
                m_host.changed();
              },
          .pointerMode =
              [this](bool) {
                // A guest that loads its tablet driver no longer needs the pointer locked, and the other way round.
                if (m_grabbed) {
                  release();
                  grab();
                }
                m_host.changed();
              },
      }
  );
}

void coffret::ConsoleView::detach() {
  release();
  m_keysDown.clear();
  m_buttonsDown.clear();
  m_display.reset();
  m_ready = false;
  m_frames->disable();
}

void coffret::ConsoleView::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (!active) {
    focusLost();
  } else {
    sizeChanged();
  }
}

void coffret::ConsoleView::sizeChanged() {
  m_resizeTimer.start(std::chrono::milliseconds(200), [this] { requestGuestSize(); });
}

void coffret::ConsoleView::requestSize(std::uint32_t width, std::uint32_t height) {
  if (!m_display || !m_ready) {
    return;
  }
  m_askedWidth = width;
  m_askedHeight = height;
  // 96 dpi; the guest scales its interface by its own settings.
  m_display->requestSize(
      width, height, static_cast<std::uint16_t>(width * 254 / 960), static_cast<std::uint16_t>(height * 254 / 960)
  );
}

void coffret::ConsoleView::requestGuestSize() {
  if (!m_display || !m_ready || !m_node || !m_active) {
    return;
  }
  const float scale = m_host.surface.effectiveBufferScale();
  const auto width = static_cast<std::uint32_t>(std::lround(m_node->width() * scale));
  const auto height = static_cast<std::uint32_t>(std::lround(m_node->height() * scale));
  if (width < 320 || height < 200 || (width == m_askedWidth && height == m_askedHeight)) {
    return;
  }
  requestSize(width, height);
}

std::string coffret::ConsoleView::describe() const {
  if (!m_problem.empty()) {
    return m_problem;
  }
  if (!m_display || !m_ready || !m_node) {
    return "Connecting to the display …";
  }
  const auto stats = m_node->stats();
  return std::format(
      "{}x{}   {}   {:.0f} fps   pointer: {}   {}", stats.width, stats.height, stats.path, m_fps,
      m_display->pointerAbsolute() ? "absolute" : "relative",
      m_grabbed ? "Ctrl+Alt releases keyboard and mouse" : "click into the picture to control the guest"
  );
}

void coffret::ConsoleView::grab() {
  if (m_grabbed || !m_display || !m_ready || !m_active) {
    return;
  }
  m_grabbed = true;
  m_chordArmed = false;
  wl_surface* surface = m_host.surface.wlSurface();
  m_host.grab.inhibitShortcuts(surface);
  if (!m_display->pointerAbsolute()) {
    m_pendingDx = m_pendingDy = 0.0;
    m_host.grab.lockPointer(surface, [this](double dx, double dy) {
      if (!m_display) {
        return;
      }
      m_pendingDx += dx;
      m_pendingDy += dy;
      const auto stepX = static_cast<std::int32_t>(m_pendingDx), stepY = static_cast<std::int32_t>(m_pendingDy);
      if (stepX != 0 || stepY != 0) {
        m_pendingDx -= stepX;
        m_pendingDy -= stepY;
        m_display->pointerMotion(stepX, stepY);
      }
    });
  }
  m_host.changed();
}

void coffret::ConsoleView::release() {
  if (!m_grabbed) {
    return;
  }
  m_grabbed = false;
  m_chordArmed = false;
  m_host.grab.releaseShortcuts();
  m_host.grab.unlockPointer();
  m_host.changed();
}

void coffret::ConsoleView::focusLost() {
  release();
  // The guest would see them held for good.
  if (m_display && m_ready) {
    for (const auto evdev : m_keysDown) {
      if (const auto qnum = qnumOf(evdev)) {
        m_display->keyRelease(qnum);
      }
    }
  }
  m_keysDown.clear();
}

void coffret::ConsoleView::sendKey(std::uint32_t evdev, bool pressed) {
  const std::uint32_t qnum = qnumOf(evdev);
  if (qnum == 0 || !m_display || !m_ready) {
    return;
  }
  if (pressed) {
    m_keysDown.insert(evdev);
    m_display->keyPress(qnum);
  } else if (m_keysDown.erase(evdev) != 0) {
    m_display->keyRelease(qnum);
  }
}

void coffret::ConsoleView::clickAt(std::uint32_t x, std::uint32_t y, std::uint32_t button) {
  if (!m_display || !m_ready || !m_display->pointerAbsolute()) {
    return;
  }
  m_display->pointerPosition(x, y);
  m_display->pointerPress(button);
  m_display->pointerRelease(button);
}

void coffret::ConsoleView::typeKeys(const std::vector<std::uint32_t>& keys) {
  for (const auto evdev : keys) {
    sendKey(evdev, true);
  }
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    sendKey(*it, false);
  }
}

void coffret::ConsoleView::key(std::uint32_t evdev, bool pressed) {
  const bool control = evdev == KEY_LEFTCTRL || evdev == KEY_RIGHTCTRL;
  const bool alt = evdev == KEY_LEFTALT || evdev == KEY_RIGHTALT;
  if (!pressed) {
    // The guest saw the press, so it gets the release, grab or not.
    sendKey(evdev, false);
    if (m_grabbed && m_chordArmed && (control || alt)) {
      release();
    }
    m_chordArmed = false;
    return;
  }
  if (!m_grabbed) {
    return;
  }
  sendKey(evdev, true);
  const auto down = [this](std::uint32_t a, std::uint32_t b) { return m_keysDown.contains(a) || m_keysDown.contains(b); };
  m_chordArmed = (control || alt) && m_keysDown.size() == 2 && down(KEY_LEFTCTRL, KEY_RIGHTCTRL) && down(KEY_LEFTALT, KEY_RIGHTALT);
}

bool coffret::ConsoleView::pointer(const PointerEvent& event) {
  if (event.surface != m_host.surface.wlSurface() || !m_display || !m_ready || !m_node || !m_active) {
    return false;
  }
  const bool locked = m_host.grab.pointerLocked();
  const auto position = [this, &event]() -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
    float localX = 0, localY = 0;
    if (!Node::mapFromScene(m_node, static_cast<float>(event.sx), static_cast<float>(event.sy), localX, localY)) {
      return std::nullopt;
    }
    const auto stats = m_node->stats();
    return guestPoint(m_node->placement(m_host.surface.effectiveBufferScale()), localX, localY, stats.width, stats.height);
  };
  switch (event.type) {
  case PointerEvent::Type::Enter:
  case PointerEvent::Type::Motion:
    if (m_display->pointerAbsolute()) {
      if (const auto point = position()) {
        m_display->pointerPosition(point->first, point->second);
        return true;
      }
    }
    return locked;
  case PointerEvent::Type::Button:
    // A locked pointer has no position; before the lock a click must land in the picture. A release
    // follows its press wherever the pointer went.
    if (const auto button = buttonOf(event.button)) {
      if (event.pressed && (locked || position())) {
        grab();
        m_buttonsDown.insert(*button);
        m_display->pointerPress(*button);
        return true;
      }
      if (!event.pressed && m_buttonsDown.erase(*button) != 0) {
        m_display->pointerRelease(*button);
        return true;
      }
    }
    return locked;
  case PointerEvent::Type::Axis:
    // Vertical wheel steps; a guest has no finer scrolling over this interface.
    if ((locked || position()) && event.axis == 0 && event.axisValue120 != 0) {
      const std::uint32_t button = event.axisValue120 < 0 ? 3 : 4;
      for (int steps = std::max(1, std::abs(event.axisValue120) / 120); steps > 0; --steps) {
        m_display->pointerPress(button);
        m_display->pointerRelease(button);
      }
      return true;
    }
    return locked;
  case PointerEvent::Type::Leave:
    break;
  }
  return false;
}
