#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace coffret {
  // pixman format codes QEMU sends for its 32 bit surfaces (little endian: B, G, R, X/A in memory).
  inline constexpr std::uint32_t kPixmanX8R8G8B8 = 0x20020888;
  inline constexpr std::uint32_t kPixmanA8R8G8B8 = 0x20028888;

  struct Rect {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    bool empty() const noexcept { return width <= 0 || height <= 0; }
    // The smallest rectangle around both.
    Rect united(const Rect& other) const noexcept;
    Rect clipped(std::int32_t maxWidth, std::int32_t maxHeight) const noexcept;
    bool operator==(const Rect&) const = default;
  };

  struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
  };

  // What the guest shows right now. Pixels and file descriptors stay valid while a copy of the
  // Scanout lives: `keep` owns the mapping, the bytes or the descriptors.
  struct Scanout {
    enum class Kind : std::uint8_t {
      None,
      // Pixels in memory: a memfd QEMU shares, or a copy of the bytes it sent.
      Pixels,
      // A GPU buffer the guest rendered into.
      Dmabuf,
    };
    Kind kind = Kind::None;
    // Changes with every new scanout, so textures are made anew.
    std::uint64_t serial = 0;
    // The visible size in guest pixels.
    std::uint32_t width = 0, height = 0;

    const std::uint8_t* pixels = nullptr;
    std::uint32_t stride = 0;
    std::uint32_t pixmanFormat = 0;

    std::vector<DmabufPlane> planes;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    bool y0Top = true;
    // The visible part of a larger buffer.
    std::uint32_t x = 0, y = 0, backingWidth = 0, backingHeight = 0;

    std::shared_ptr<void> keep;
  };

  struct GuestCursor {
    std::uint64_t serial = 0;
    std::int32_t width = 0, height = 0, hotX = 0, hotY = 0;
    // ARGB32, premultiplied, width * height * 4 bytes.
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    std::int32_t x = 0, y = 0;
    bool visible = false;
  };

  // The current scanout and what changed in it, handed from the thread that talks to QEMU to the
  // thread that draws.
  class FrameStore {
  public:
    // Runs on whichever thread changed the store, with no lock held.
    void setOnChanged(std::function<void()> callback);

    // QEMU's side.
    void setPixels(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                   std::uint32_t pixmanFormat, std::shared_ptr<void> keep);
    void setDmabuf(Scanout scanout);
    void disable();
    void damage(Rect rect);
    void setCursor(std::int32_t width, std::int32_t height, std::int32_t hotX, std::int32_t hotY, std::vector<std::uint8_t> pixels);
    void moveCursor(std::int32_t x, std::int32_t y, bool visible);

    // The drawing side.
    struct Snapshot {
      Scanout scanout;
      // What changed since the last take(); empty when nothing did. Meaningless for a new serial.
      Rect damage;
    };
    Snapshot take();
    // Without clearing the damage: for sizes and thumbnails.
    Scanout peek() const;
    GuestCursor cursor() const;

  private:
    void changed();

    mutable std::mutex m_mutex;
    Scanout m_scanout;
    // A GPU buffer QEMU has named but not yet said to be drawn: shown from its first update on.
    std::optional<Scanout> m_pending;
    std::chrono::steady_clock::time_point m_pendingSince{};
    Rect m_damage;
    GuestCursor m_cursor;
    std::uint64_t m_nextSerial = 1;
    std::function<void()> m_onChanged;
  };

  // Where a guest picture of guestWidth x guestHeight pixels goes in a node of the given size
  // (logical pixels, `scale` buffer pixels each): pixel for pixel when it fits exactly, else as
  // large as it can be with its shape kept, centred.
  struct Placement {
    float x = 0, y = 0, width = 0, height = 0;
    // No scaling happens: sample without filtering.
    bool exact = false;
  };
  Placement placeFrame(float nodeWidth, float nodeHeight, float scale, std::uint32_t guestWidth, std::uint32_t guestHeight);
  // The guest pixel under a point of the node; nullopt outside the picture.
  std::optional<std::pair<std::uint32_t, std::uint32_t>>
  guestPoint(const Placement& placement, float x, float y, std::uint32_t guestWidth, std::uint32_t guestHeight);
} // namespace coffret
