#include "app/console_window.h"

#include "app/worker.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "qemu/qmp_attach.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  const Logger kLog("console-window");
  constexpr float kStatusHeight = 26.0F;
} // namespace

std::vector<std::uint32_t> coffret::parseKeys(const std::string& text) {
  std::vector<std::uint32_t> keys;
  for (const char* at = text.c_str(); *at;) {
    char* end = nullptr;
    const auto key = static_cast<std::uint32_t>(std::strtoul(at, &end, 0));
    if (end == at) {
      break;
    }
    keys.push_back(key);
    at = *end ? end + 1 : end;
  }
  return keys;
}

coffret::ConsoleWindow::ConsoleWindow(Services services, std::string qmpSocket)
    : m_services(services), m_qmpSocket(std::move(qmpSocket)), m_surface(services.connection),
      m_console({.surface = m_surface, .grab = services.grab, .changed = [this] { m_surface.requestLayout(); }}) {
  m_surface.setRenderContext(&m_services.renderer);
  m_surface.setAnimationManager(&m_services.animations);
  m_surface.setPrepareFrameCallback([this](bool, bool) { prepare(); });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    m_surface.requestLayout();
    m_console.sizeChanged();
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
  attach();
}

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
          m_surface.requestLayout();
          return;
        }
        m_console.attach(*bus);
      }
  );
}

bool coffret::ConsoleWindow::action(const std::string& name, const std::string& parameter) {
  if (name == "fullscreen") {
    m_surface.toggleFullscreen();
  } else if (name == "state") {
    std::println("state: {}", m_problem.empty() ? m_console.describe() : m_problem);
    std::fflush(stdout);
  } else if (name == "key") {
    m_console.typeKeys(parseKeys(parameter));
  } else if (name == "size") {
    unsigned width = 0, height = 0;
    if (std::sscanf(parameter.c_str(), "%ux%u", &width, &height) != 2) {
      return false;
    }
    m_console.requestSize(width, height);
  } else if (name == "click") {
    unsigned x = 0, y = 0;
    if (std::sscanf(parameter.c_str(), "%u,%u", &x, &y) != 2) {
      return false;
    }
    m_console.clickAt(x, y);
  } else if (name == "grab") {
    m_console.grab();
  } else if (name == "release") {
    m_console.release();
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
  auto console = m_console.build();
  console->setFlexGrow(1.0F);
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
  m_status->setText(m_problem.empty() ? m_console.describe() : m_problem);
  auto& renderer = m_surface.renderTarget().renderer();
  m_root->arrange(renderer, {0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())});
}
