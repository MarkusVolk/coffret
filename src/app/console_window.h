#pragma once

#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/console_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AnimationManager;
class Label;
class RenderContext;
class WaylandConnection;
struct PointerEvent;

namespace coffret {
  class InputGrab;

  // A window that is nothing but the console of one QEMU, attached over its QMP socket: the way to
  // look at a guest that was started by hand.
  class ConsoleWindow {
  public:
    struct Services {
      WaylandConnection& connection;
      RenderContext& renderer;
      AnimationManager& animations;
      InputGrab& grab;
    };

    ConsoleWindow(Services services, std::string qmpSocket);
    ConsoleWindow(const ConsoleWindow&) = delete;
    ConsoleWindow& operator=(const ConsoleWindow&) = delete;

    // Runs a named action ("fullscreen", "key", "state", ...); false when there is none of that name.
    bool action(const std::string& name, const std::string& parameter);
    bool running() const { return m_surface.isRunning(); }
    void close() { m_surface.close(); }
    wl_surface* surface() const { return m_surface.wlSurface(); }

    void pointer(const PointerEvent& event) { m_console.pointer(event); }
    void key(std::uint32_t evdev, bool pressed) { m_console.key(evdev, pressed); }
    void focusLost() { m_console.focusLost(); }

  private:
    void attach();
    void prepare();
    void build();

    Services m_services;
    std::string m_qmpSocket;
    WindowSurface m_surface;
    InputDispatcher m_dispatcher;
    ConsoleView m_console;
    std::unique_ptr<Node> m_root;
    Label* m_status = nullptr;
    std::string m_problem;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };

  // Linux key codes from "29+56+111" or "29,56,111".
  std::vector<std::uint32_t> parseKeys(const std::string& text);
} // namespace coffret
