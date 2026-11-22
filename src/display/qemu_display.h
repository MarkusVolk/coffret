#pragma once

#include "display/frame_store.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace coffret {
  // One console of a QEMU that runs with `-display dbus`: registers as its listener and fills a
  // FrameStore with what the guest shows, and sends it keys and pointer events. All D-Bus traffic
  // runs on a thread of its own, so a guest that redraws at full speed never holds up the window.
  class QemuDisplay {
  public:
    // These run on the main thread, through the default GLib context.
    struct Callbacks {
      // The listener is registered; pictures follow.
      std::function<void()> ready;
      // The connection is gone, with the reason; nothing more comes.
      std::function<void(const std::string& why)> closed;
      // The guest's pointer device changed: a tablet takes positions, a mouse takes movements.
      std::function<void(bool absolute)> pointerMode;
      // The guest copied this text.
      std::function<void(std::string text)> guestClipboard;
    };

    // busFd is a connected socket of QEMU's peer-to-peer display bus; it is owned from here on.
    QemuDisplay(int busFd, std::shared_ptr<FrameStore> frames, Callbacks callbacks);
    ~QemuDisplay();
    // Offers a text to the guest as its clipboard. Does nothing for a machine without the
    // clipboard channel or a guest without spice-vdagent.
    void offerClipboard(std::string text);

    QemuDisplay(const QemuDisplay&) = delete;
    QemuDisplay& operator=(const QemuDisplay&) = delete;

    // Keys are QEMU key numbers, see input/linux_to_qnum.h.
    void keyPress(std::uint32_t qnum);
    void keyRelease(std::uint32_t qnum);
    // Buttons: 0 left, 1 middle, 2 right, 3 wheel up, 4 wheel down, 5 side, 6 extra.
    void pointerPress(std::uint32_t button);
    void pointerRelease(std::uint32_t button);
    void pointerPosition(std::uint32_t x, std::uint32_t y);
    void pointerMotion(std::int32_t dx, std::int32_t dy);
    // Asks the guest for this resolution; a guest with virtio-gpu follows on its own.
    void requestSize(std::uint32_t width, std::uint32_t height, std::uint16_t widthMm, std::uint16_t heightMm);

    bool pointerAbsolute() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace coffret
