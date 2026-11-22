#pragma once

#include "core/timer_manager.h"
#include "display/frame_store.h"
#include "display/qemu_display.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class Node;

struct PointerEvent;

namespace coffret {
  class ConsoleNode;
  class InputGrab;
  class WindowSurface;

  // One guest's screen with its keyboard and mouse: the picture as a node for any window, and what
  // goes back to the guest. Keys go to the guest as the window passes them; a click into the
  // picture grabs keyboard and mouse, with the desktop's shortcuts, and Ctrl+Alt takes them back.
  class ConsoleView {
  public:
    struct Host {
      WindowSurface& surface;
      InputGrab& grab;
      // Something the window shows about the console changed.
      std::function<void()> changed;
      // The host's own pointer is to be hidden over the picture, where the guest draws one, or
      // shown again; `serial` is the pointer event's.
      std::function<void(bool hidden, std::uint32_t serial)> hostCursor = {};
      // The guest copied a text: it becomes the host's clipboard.
      std::function<void(std::string)> guestCopied = {};
    };

    explicit ConsoleView(Host host);
    ~ConsoleView();
    ConsoleView(const ConsoleView&) = delete;
    ConsoleView& operator=(const ConsoleView&) = delete;

    // The picture, once; it belongs to the window's scene from then on.
    std::unique_ptr<Node> build();
    ConsoleNode* node() const noexcept { return m_node; }

    // Takes over a connection to QEMU's display (see QemuDisplay).
    void attach(int busFd);
    void detach();
    bool attached() const noexcept { return m_display != nullptr; }
    bool ready() const noexcept { return m_ready; }
    // Why the display went away, if it did.
    const std::string& problem() const noexcept { return m_problem; }

    // Whether the window shows the console right now; a hidden one takes no input and asks for no frames.
    void setActive(bool active);
    // The guest follows the window without having defined a pointer: Windows with viogpu3d draws
    // its pointer itself and is never a text console that would turn its picture off.
    void setFollowsWithoutPointer(bool follows) { m_followsWithoutPointer = follows; }
    // The node's size changed: the guest follows, once it has settled.
    void sizeChanged();
    void requestSize(std::uint32_t width, std::uint32_t height);

    // A key as the keyboard sent it (Linux key code); a press goes to the guest when `toGuest`.
    void key(std::uint32_t evdev, bool pressed, bool toGuest);
    // True when the event was the guest's.
    bool pointer(const PointerEvent& event);
    // The window lost the keyboard: the guest lets go of it too.
    void focusLost();
    void grab();
    void release();
    bool grabbed() const noexcept { return m_grabbed; }
    // The guest got the press of this key and not yet its release.
    bool held(std::uint32_t evdev) const { return m_keysDown.contains(evdev); }
    // The guest's picture into a PPM file, at the next frame.
    void dumpTo(const std::string& path);
    // What the host has in its clipboard, for the guest to paste.
    void hostCopied(std::string text);
    // A click at a place of the guest's screen, in its pixels: for tests without a mouse.
    void clickAt(std::uint32_t x, std::uint32_t y, std::uint32_t button = 0);
    // Keys pressed in this order and released in reverse, e.g. Ctrl+Alt+Del.
    void typeKeys(const std::vector<std::uint32_t>& keys);

    // Size, display path, frame rate and pointer mode in one line.
    std::string describe() const;

  private:
    void requestGuestSize();
    void guestShows(std::uint32_t width, std::uint32_t height);
    void sendKey(std::uint32_t evdev, bool pressed);

    Host m_host;
    std::shared_ptr<FrameStore> m_frames;
    std::unique_ptr<QemuDisplay> m_display;
    ConsoleNode* m_node = nullptr;
    std::string m_problem;
    // The text last shared with the guest, in either direction.
    std::string m_hostText;
    bool m_ready = false;
    bool m_active = true;
    // The size last asked of the guest, in guest pixels.
    std::uint32_t m_askedWidth = 0, m_askedHeight = 0;
    // The guest's pointer as it was when the console was attached or its picture last started
    // over. A text console has no pointer and cannot change its size: it turns its picture off
    // when asked. Only a guest that defined a pointer since, a desktop, follows the window.
    std::uint64_t m_cursorBefore = 0;
    bool m_followsWithoutPointer = false;
    Timer m_resizeTimer;
    Timer m_statusTimer;
    std::uint64_t m_framesAtLastStatus = 0;
    std::chrono::steady_clock::time_point m_lastStatus{};
    double m_fps = 0.0;
    bool m_grabbed = false;
    // Ctrl and Alt are down with nothing else pressed since: letting one go releases the grab.
    bool m_chordArmed = false;
    // Keys whose press went to the guest; their release follows even after the grab ended.
    std::unordered_set<std::uint32_t> m_keysDown;
    std::unordered_set<std::uint32_t> m_buttonsDown;
    // The host's pointer is hidden because the guest draws its own.
    bool m_hostCursorHidden = false;
    // Movement of a locked pointer not yet sent, as the guest takes whole steps only.
    double m_pendingDx = 0.0, m_pendingDy = 0.0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace coffret
