#pragma once

#include "libvirt/hypervisor.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/console_view.h"
#include "ui/controls/scroll_view.h"
#include "vm/machine_factory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AnimationManager;
class Button;
class Flex;
class Input;
class Label;
class RenderContext;
class TextInputService;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace coffret {
  class InputGrab;

  // The window: the library of machines on the left, a tab per opened machine, and for each its
  // summary while it is off and its console while it runs.
  class MainWindow {
  public:
    struct Services {
      WaylandConnection& connection;
      RenderContext& renderer;
      TextInputService& textInput;
      AnimationManager& animations;
      Hypervisor& hypervisor;
      InputGrab& grab;
    };

    explicit MainWindow(Services services);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Runs a named action ("open", "start", "state", ...); false when there is none of that name.
    bool action(const std::string& name, const std::string& parameter);
    bool running() const { return m_surface.isRunning(); }
    void close() { m_surface.close(); }
    wl_surface* surface() const { return m_surface.wlSurface(); }

    void keyboard(const KeyboardEvent& event);
    void pointer(const PointerEvent& event);
    void focus(wl_surface* surface, bool entered);
    // Keys as the keyboard sends them, for the guest.
    void rawKey(std::uint32_t evdev, bool pressed);

  private:
    struct Tab {
      std::string uuid;
      std::unique_ptr<ConsoleView> console;
      Node* picture = nullptr;
      // A connection to the display is being opened.
      bool opening = false;
      unsigned failures = 0;
      Timer retry;
      std::string displayProblem;
    };

    void prepare();
    void build();
    void requestUpdate();
    void machinesChanged();
    void syncList();
    void syncTabs();
    void syncPage();
    void openTab(const std::string& uuid);
    void closeTab(std::size_t index);
    void showHome();
    void openDisplay(const std::string& uuid);
    Tab* tabOf(const std::string& uuid);
    Tab* currentTab();
    const Machine* currentMachine() const;
    // The machine of that name or uuid; the current tab's for an empty one.
    const Machine* machineFor(const std::string& text) const;
    ConsoleView* shownConsole();
    void run(const std::string& what, const std::string& uuid, void (Hypervisor::*call)(const std::string&, Hypervisor::Done));
    void toggleFullscreen();
    void importImage(ImageImport request);
    std::string describe() const;

    Services m_services;
    WindowSurface m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Node> m_root;
    ScrollViewState m_listScroll;
    ScrollViewState m_homeScroll;

    Flex* m_sidebar = nullptr;
    Input* m_search = nullptr;
    Flex* m_list = nullptr;
    Label* m_connection = nullptr;
    Flex* m_tabBar = nullptr;
    Flex* m_toolbar = nullptr;
    Button* m_startButton = nullptr;
    Button* m_pauseButton = nullptr;
    Button* m_suspendButton = nullptr;
    Button* m_shutDownButton = nullptr;
    Button* m_forceOffButton = nullptr;
    Button* m_resetButton = nullptr;
    Button* m_keysButton = nullptr;
    Button* m_fullscreenButton = nullptr;
    Flex* m_home = nullptr;
    Flex* m_homeList = nullptr;
    Label* m_homeEmpty = nullptr;
    Flex* m_summary = nullptr;
    Label* m_summaryName = nullptr;
    Label* m_summaryState = nullptr;
    Flex* m_summaryRows = nullptr;
    Flex* m_consoles = nullptr;
    Flex* m_statusBar = nullptr;
    Label* m_status = nullptr;

    std::vector<std::unique_ptr<Tab>> m_tabs;
    // The tab shown; none for the home page.
    std::string m_current;
    std::string m_query;
    std::string m_message;
    bool m_listStale = true;
    bool m_tabsStale = true;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace coffret
