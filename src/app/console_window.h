#pragma once

#include "core/timer_manager.h"
#include "display/frame_store.h"
#include "display/qemu_display.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class AnimationManager;
class Label;
class RenderContext;
class WaylandConnection;
struct PointerEvent;

namespace coffret {
  class ConsoleNode;
  class RawKeyboard;

  // A window that is nothing but the console of one QEMU, attached over its QMP socket. The first
  // user of the display code, and the way to look at a guest that was started by hand.
  class ConsoleWindow {
  public:
    struct Services {
      WaylandConnection& connection;
      RenderContext& renderer;
      AnimationManager& animations;
      RawKeyboard& keyboard;
    };

    ConsoleWindow(Services services, std::string qmpSocket);
    ~ConsoleWindow();
    ConsoleWindow(const ConsoleWindow&) = delete;
    ConsoleWindow& operator=(const ConsoleWindow&) = delete;

    // Runs a named action ("fullscreen", "keys", "state", ...); false when there is none of that name.
    bool action(const std::string& name, const std::string& parameter);
    bool running() const { return m_surface.isRunning(); }
    void close() { m_surface.close(); }
    wl_surface* surface() const { return m_surface.wlSurface(); }

    void pointer(const PointerEvent& event);
    void key(std::uint32_t evdev, bool pressed);

  private:
    void attach();
    void prepare();
    void build();
    void requestGuestSize();
    void requestUpdate();
    std::string describe() const;

    Services m_services;
    std::string m_qmpSocket;
    WindowSurface m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Node> m_root;
    ConsoleNode* m_console = nullptr;
    Label* m_status = nullptr;

    std::shared_ptr<FrameStore> m_frames;
    std::unique_ptr<QemuDisplay> m_display;
    std::string m_problem;
    bool m_ready = false;
    // The size last asked of the guest, in guest pixels.
    std::uint32_t m_askedWidth = 0, m_askedHeight = 0;
    Timer m_resizeTimer;
    Timer m_statusTimer;
    std::uint64_t m_framesAtLastStatus = 0;
    std::chrono::steady_clock::time_point m_lastStatus{};
    double m_fps = 0.0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace coffret
