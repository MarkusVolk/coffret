#include "app/console_window.h"

#include "app/worker.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "display/console_node.h"
#include "input/linux_to_qnum.h"
#include "input/raw_keyboard.h"
#include "qemu/qmp_attach.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <atomic>
#include <cmath>
#include <format>
#include <print>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <stdexcept>

namespace {
  const Logger kLog("console-window");
  constexpr float kStatusHeight = 26.0F;

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

coffret::ConsoleWindow::ConsoleWindow(Services services, std::string qmpSocket)
    : m_services(services), m_qmpSocket(std::move(qmpSocket)), m_surface(services.connection),
      m_frames(std::make_shared<FrameStore>()) {
  m_surface.setRenderContext(&m_services.renderer);
  m_surface.setAnimationManager(&m_services.animations);
  m_surface.setPrepareFrameCallback([this](bool, bool) { prepare(); });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    m_surface.requestLayout();
    // The guest follows the window, once the size has settled.
    m_resizeTimer.start(std::chrono::milliseconds(200), [this] { requestGuestSize(); });
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.connection.extension<DesktopIntegration>().setCursorShape(serial, shape);
  });
  if (!m_surface.initialize(
          nullptr, {.width = 1280U, .height = 800U + static_cast<std::uint32_t>(kStatusHeight), .minWidth = 320, .minHeight = 240,
                    .title = "Coffret", .appId = "de.flk.Coffret"}
      )) {
    throw std::runtime_error("cannot create the window");
  }
  // A new picture arrives on the display's thread; the drawing happens here.
  // A guest can change its picture far more often than a frame is drawn: one wake-up at a time.
  std::weak_ptr<int> alive = m_alive;
  auto queued = std::make_shared<std::atomic<bool>>(false);
  m_frames->setOnChanged([this, alive, queued] {
    if (queued->exchange(true)) {
      return;
    }
    runOnMain([this, alive, queued] {
      queued->store(false);
      if (!alive.expired()) {
        m_surface.requestRedraw();
      }
    });
  });
  m_statusTimer.startRepeating(std::chrono::seconds(1), [this] {
    const auto now = std::chrono::steady_clock::now();
    if (m_console) {
      const auto frames = m_console->stats().frames;
      const double seconds = std::chrono::duration<double>(now - m_lastStatus).count();
      m_fps = seconds > 0 ? static_cast<double>(frames - m_framesAtLastStatus) / seconds : 0.0;
      m_framesAtLastStatus = frames;
    }
    m_lastStatus = now;
    requestUpdate();
  });
  attach();
}

coffret::ConsoleWindow::~ConsoleWindow() { m_frames->setOnChanged({}); }

void coffret::ConsoleWindow::requestUpdate() { m_surface.requestLayout(); }

void coffret::ConsoleWindow::attach() {
  std::weak_ptr<int> alive = m_alive;
  runInThread<std::expected<int, std::string>>(
      [socket = m_qmpSocket] { return openDisplayBus(socket); },
      [this, alive](std::expected<int, std::string> bus) {
        if (alive.expired()) {
          if (bus) {
            ::close(*bus);
          }
          return;
        }
        if (!bus) {
          m_problem = bus.error();
          kLog.warn("{}", m_problem);
          requestUpdate();
          return;
        }
        m_display = std::make_unique<QemuDisplay>(
            *bus, m_frames,
            QemuDisplay::Callbacks{
                .ready =
                    [this] {
                      m_ready = true;
                      requestGuestSize();
                      requestUpdate();
                    },
                .closed =
                    [this](const std::string& why) {
                      m_ready = false;
                      m_problem = why;
                      requestUpdate();
                    },
                .pointerMode = [this](bool) { requestUpdate(); },
            }
        );
      }
  );
}

void coffret::ConsoleWindow::requestGuestSize() {
  if (!m_display || !m_ready || !m_console) {
    return;
  }
  const float scale = m_surface.effectiveBufferScale();
  const auto width = static_cast<std::uint32_t>(std::lround(m_console->width() * scale));
  const auto height = static_cast<std::uint32_t>(std::lround(m_console->height() * scale));
  if (width < 320 || height < 200 || (width == m_askedWidth && height == m_askedHeight)) {
    return;
  }
  m_askedWidth = width;
  m_askedHeight = height;
  // 96 dpi; the guest scales its interface by its own settings.
  m_display->requestSize(
      width, height, static_cast<std::uint16_t>(width * 254 / 960), static_cast<std::uint16_t>(height * 254 / 960)
  );
}

std::string coffret::ConsoleWindow::describe() const {
  if (!m_problem.empty()) {
    return m_problem;
  }
  if (!m_ready || !m_console) {
    return std::format("Connecting to {} …", m_qmpSocket);
  }
  const auto stats = m_console->stats();
  return std::format(
      "{}x{}   {}   {:.0f} fps   pointer: {}", stats.width, stats.height, stats.path, m_fps,
      m_display->pointerAbsolute() ? "absolute" : "relative"
  );
}

bool coffret::ConsoleWindow::action(const std::string& name, const std::string& parameter) {
  if (name == "fullscreen") {
    m_surface.toggleFullscreen();
  } else if (name == "state") {
    kLog.info("state: {}", describe());
    std::println("{}", describe());
  } else if (name == "key" && m_display) {
    // A Linux key code, pressed and released: for tests without a keyboard.
    const auto evdev = static_cast<std::uint32_t>(std::strtoul(parameter.c_str(), nullptr, 0));
    key(evdev, true);
    key(evdev, false);
  } else if (name == "size" && m_display) {
    unsigned width = 0, height = 0;
    if (std::sscanf(parameter.c_str(), "%ux%u", &width, &height) != 2) {
      return false;
    }
    m_display->requestSize(width, height, static_cast<std::uint16_t>(width * 254 / 960), static_cast<std::uint16_t>(height * 254 / 960));
  } else {
    return false;
  }
  return true;
}

void coffret::ConsoleWindow::build() {
  m_root = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Surface)});
  m_root->setAnimationManager(&m_services.animations);
  m_surface.setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());
  auto console = std::make_unique<ConsoleNode>(m_frames);
  console->setFlexGrow(1.0F);
  m_console = console.get();
  m_root->addChild(std::move(console));
  m_root->addChild(ui::row(
      {.align = FlexAlign::Center,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kStatusHeight},
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
          .configure = [](Label& label) { label.setUseMarkup(false); },
      })
  ));
}

void coffret::ConsoleWindow::prepare() {
  if (!m_surface.isRunning() || !m_services.renderer.makeCurrent(m_surface.renderTarget())) {
    return;
  }
  UiPhaseScope phase(UiPhase::Layout);
  if (!m_root) {
    build();
  }
  m_status->setText(describe());
  auto& renderer = m_surface.renderTarget().renderer();
  m_root->arrange(renderer, {0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())});
}

void coffret::ConsoleWindow::key(std::uint32_t evdev, bool pressed) {
  if (!m_display || !m_ready) {
    return;
  }
  const std::uint32_t qnum = qnumOf(evdev);
  if (qnum == 0) {
    return;
  }
  if (pressed) {
    m_display->keyPress(qnum);
  } else {
    m_display->keyRelease(qnum);
  }
}

void coffret::ConsoleWindow::pointer(const PointerEvent& event) {
  if (event.surface != m_surface.wlSurface() || !m_display || !m_ready || !m_console) {
    return;
  }
  const auto position = [this, &event]() -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
    float localX = 0, localY = 0;
    if (!Node::mapFromScene(m_console, static_cast<float>(event.sx), static_cast<float>(event.sy), localX, localY)) {
      return std::nullopt;
    }
    const auto stats = m_console->stats();
    return guestPoint(m_console->placement(m_surface.effectiveBufferScale()), localX, localY, stats.width, stats.height);
  };
  switch (event.type) {
  case PointerEvent::Type::Enter:
  case PointerEvent::Type::Motion:
    if (m_display->pointerAbsolute()) {
      if (const auto point = position()) {
        m_display->pointerPosition(point->first, point->second);
      }
    }
    break;
  case PointerEvent::Type::Button:
    if (const auto button = buttonOf(event.button); button && position()) {
      if (event.pressed) {
        m_display->pointerPress(*button);
      } else {
        m_display->pointerRelease(*button);
      }
    }
    break;
  case PointerEvent::Type::Axis:
    // Vertical wheel steps; a guest has no finer scrolling over this interface.
    if (event.axis == 0 && event.axisValue120 != 0) {
      const std::uint32_t button = event.axisValue120 < 0 ? 3 : 4;
      for (int steps = std::max(1, std::abs(event.axisValue120) / 120); steps > 0; --steps) {
        m_display->pointerPress(button);
        m_display->pointerRelease(button);
      }
    }
    break;
  case PointerEvent::Type::Leave:
    break;
  }
}
