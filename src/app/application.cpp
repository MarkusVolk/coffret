#include "app/application.h"

#include "app/console_window.h"
#include "app/instance_service.h"
#include "app/main_loop.h"
#include "app/palette_watcher.h"
#include "coffret_build_info.h"
#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_strings.h"
#include "event_loop/deferred_call_poll_source.h"
#include "event_loop/glib_poll_source.h"
#include "event_loop/timer_poll_source.h"
#include "input/input_grab.h"
#include "input/raw_keyboard.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "render/codecs/codecs.h"
#include "render/core/async_texture_cache.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "ui/controls/input.h"
#include "ui/palette_file.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <print>

namespace {
  volatile std::sig_atomic_t g_stopRequested = 0;
  void stopRequested(int) { g_stopRequested = 1; }

  class KeyRepeatPollSource final : public PollSource {
  public:
    explicit KeyRepeatPollSource(WaylandConnection& seat) : m_seat(seat) {}
    int pollTimeoutMs() const override { return m_seat.repeatPollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>&, std::size_t) override { m_seat.repeatTick(); }

  private:
    void doAddPollFds(std::vector<pollfd>&) override {}
    WaylandConnection& m_seat;
  };

  // The chords the toolkit's own controls (inputs, menus, lists) ask for by action.
  void initializeKeys() {
    const std::array<std::pair<KeybindAction, std::uint32_t>, 7> keys{{
        {KeybindAction::Cancel, XKB_KEY_Escape},
        {KeybindAction::Left, XKB_KEY_Left},
        {KeybindAction::Right, XKB_KEY_Right},
        {KeybindAction::Up, XKB_KEY_Up},
        {KeybindAction::Down, XKB_KEY_Down},
        {KeybindAction::TabNext, XKB_KEY_Tab},
        {KeybindAction::Delete, XKB_KEY_Delete},
    }};
    for (const auto& [action, sym] : keys) {
      KeybindMatcher::setMatcher(action, [sym](std::uint32_t key, std::uint32_t modifiers) {
        return key == sym && modifiers == 0U;
      });
    }
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t sym, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
    });
    KeybindMatcher::setMatcher(KeybindAction::Copy, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_c && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::TabPrevious, [](std::uint32_t sym, std::uint32_t modifiers) {
      return KeySymbol::isTab(sym) && modifiers == KeyMod::Shift;
    });
  }

  void initializeLibrary() {
    setApplicationIdentity({.name = "coffret", .envPrefix = "COFFRET"});
    const char* overridePath = std::getenv("COFFRET_ASSETS_DIR");
    const std::filesystem::path assets = overridePath ? overridePath : COFFRET_INSTALL_ASSETSDIR;
    paths::setAssetRoots({.application = assets, .library = assets});
    setUiStrings({.keyName = {}, .selectPlaceholder = [] { return std::string("Select…"); }});
    setPalette(coffret::defaultPalette());
    codecs::installImageDecoder();
  }
} // namespace

int coffret::runApplication(const Launch& launch) {
  initLogLevelFromEnvironment();
  InstanceService instance;
  if (instance.role() == InstanceService::Role::Secondary) {
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    if (instance.forward({}, token ? token : "")) {
      if (!launch.action.empty()) {
        InstanceService::sendAction(launch.action, launch.parameter);
      }
      return EXIT_SUCCESS;
    }
  }
  if (launch.qmpSocket.empty()) {
    std::println(stderr, "coffret: the library window is not built yet; use --qmp SOCKET for a console");
    return EXIT_FAILURE;
  }
  initializeKeys();
  Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
    return modifiers == 0 && KeySymbol::isEnter(sym);
  });
  g_stopRequested = 0;
  const auto previousTerm = std::signal(SIGTERM, stopRequested);
  const auto previousInt = std::signal(SIGINT, stopRequested);
  int result = EXIT_SUCCESS;
  try {
    initializeLibrary();
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    if (!connection.connect()) {
      throw std::runtime_error("could not connect to Wayland: " + connection.describeDisplayError());
    }
    connection.setTextInputService(&textInput);
    auto& desktop = connection.extension<DesktopIntegration>();
    auto& keyboard = connection.extension<RawKeyboard>();
    auto& grab = connection.extension<InputGrab>();
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    AsyncTextureCache textures;
    textures.initialize(&graphics);
    textures.setMakeCurrentCallback([&graphics] { graphics.makeCurrentSurfaceless(); });
    renderer.setAsyncTextureCache(&textures);
    PaletteWatcher palette;
    AnimationManager animations;

    {
      ConsoleWindow window(
          {.connection = connection, .renderer = renderer, .animations = animations, .keyboard = keyboard, .grab = grab},
          launch.qmpSocket
      );
      instance.setHandler([&window, &desktop](const InstanceRequest& request) {
        if (!request.action.empty() && !window.action(request.action, request.parameter)) {
          std::println(stderr, "coffret: no action named {}", request.action);
        }
        if (!request.activationToken.empty()) {
          desktop.activate(request.activationToken, window.surface());
        }
      });
      connection.setPointerEventCallback([&window, &desktop](const PointerEvent& event) {
        desktop.pointer(event);
        window.pointer(event);
      });
      // The guest gets the keys as the keyboard sends them; the toolkit's own key events are not used here.
      keyboard.setHandlers(
          [&window](wl_surface* surface, std::uint32_t key, bool pressed) {
            if (surface == window.surface()) {
              window.key(key, pressed);
            }
          },
          [&window](wl_surface* surface, bool entered) {
            if (surface == window.surface() && !entered) {
              window.focusLost();
            }
          }
      );

      DeferredCallPollSource deferred;
      TimerPollSource timers;
      KeyRepeatPollSource repeat(connection);
      MainLoop loop(connection, {&deferred, &timers, &repeat, &glib, &textures}, [&window] {
        if (g_stopRequested && window.running()) {
          window.close();
        }
        return window.running();
      });
      loop.run();
      instance.release();
      keyboard.setHandlers({}, {});
      connection.setPointerEventCallback({});
    }
    renderer.setAsyncTextureCache(nullptr);
    textInput.cleanup();
    connection.setTextInputService(nullptr);
  } catch (const std::exception& error) {
    std::println(stderr, "coffret: {}", error.what());
    result = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return result;
}
