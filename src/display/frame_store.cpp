#include "display/frame_store.h"

#include <algorithm>
#include <cmath>

coffret::Rect coffret::Rect::united(const Rect& other) const noexcept {
  if (empty()) {
    return other;
  }
  if (other.empty()) {
    return *this;
  }
  const std::int32_t left = std::min(x, other.x), top = std::min(y, other.y);
  const std::int32_t right = std::max(x + width, other.x + other.width);
  const std::int32_t bottom = std::max(y + height, other.y + other.height);
  return {left, top, right - left, bottom - top};
}

coffret::Rect coffret::Rect::clipped(std::int32_t maxWidth, std::int32_t maxHeight) const noexcept {
  const std::int32_t left = std::clamp(x, 0, maxWidth), top = std::clamp(y, 0, maxHeight);
  const std::int32_t right = std::clamp(x + width, 0, maxWidth), bottom = std::clamp(y + height, 0, maxHeight);
  return {left, top, right - left, bottom - top};
}

void coffret::FrameStore::setOnChanged(std::function<void()> callback) {
  std::lock_guard lock(m_mutex);
  m_onChanged = std::move(callback);
}

void coffret::FrameStore::changed() {
  std::function<void()> callback;
  {
    std::lock_guard lock(m_mutex);
    callback = m_onChanged;
  }
  if (callback) {
    callback();
  }
}

void coffret::FrameStore::setPixels(
    const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t pixmanFormat,
    std::shared_ptr<void> keep
) {
  {
    std::lock_guard lock(m_mutex);
    m_scanout = Scanout{};
    m_scanout.kind = Scanout::Kind::Pixels;
    m_scanout.serial = m_nextSerial++;
    m_scanout.width = width;
    m_scanout.height = height;
    m_scanout.pixels = pixels;
    m_scanout.stride = stride;
    m_scanout.pixmanFormat = pixmanFormat;
    m_scanout.keep = std::move(keep);
    m_damage = {};
  }
  changed();
}

void coffret::FrameStore::setDmabuf(Scanout scanout) {
  {
    std::lock_guard lock(m_mutex);
    scanout.kind = Scanout::Kind::Dmabuf;
    scanout.serial = m_nextSerial++;
    m_scanout = std::move(scanout);
    m_damage = {};
  }
  changed();
}

void coffret::FrameStore::disable() {
  {
    std::lock_guard lock(m_mutex);
    m_scanout = Scanout{};
    m_scanout.serial = m_nextSerial++;
    m_damage = {};
  }
  changed();
}

void coffret::FrameStore::damage(Rect rect) {
  {
    std::lock_guard lock(m_mutex);
    if (m_scanout.kind == Scanout::Kind::None) {
      return;
    }
    m_damage = m_damage.united(rect.clipped(static_cast<std::int32_t>(m_scanout.width), static_cast<std::int32_t>(m_scanout.height)));
  }
  changed();
}

void coffret::FrameStore::setCursor(
    std::int32_t width, std::int32_t height, std::int32_t hotX, std::int32_t hotY, std::vector<std::uint8_t> pixels
) {
  {
    std::lock_guard lock(m_mutex);
    m_cursor.serial++;
    m_cursor.width = width;
    m_cursor.height = height;
    m_cursor.hotX = hotX;
    m_cursor.hotY = hotY;
    m_cursor.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
  }
  changed();
}

void coffret::FrameStore::moveCursor(std::int32_t x, std::int32_t y, bool visible) {
  {
    std::lock_guard lock(m_mutex);
    if (m_cursor.x == x && m_cursor.y == y && m_cursor.visible == visible) {
      return;
    }
    m_cursor.x = x;
    m_cursor.y = y;
    m_cursor.visible = visible;
  }
  changed();
}

coffret::FrameStore::Snapshot coffret::FrameStore::take() {
  std::lock_guard lock(m_mutex);
  Snapshot out{m_scanout, m_damage};
  m_damage = {};
  return out;
}

coffret::Scanout coffret::FrameStore::peek() const {
  std::lock_guard lock(m_mutex);
  return m_scanout;
}

coffret::GuestCursor coffret::FrameStore::cursor() const {
  std::lock_guard lock(m_mutex);
  return m_cursor;
}

coffret::Placement
coffret::placeFrame(float nodeWidth, float nodeHeight, float scale, std::uint32_t guestWidth, std::uint32_t guestHeight) {
  if (nodeWidth <= 0 || nodeHeight <= 0 || guestWidth == 0 || guestHeight == 0 || scale <= 0) {
    return {};
  }
  const float logicalWidth = static_cast<float>(guestWidth) / scale, logicalHeight = static_cast<float>(guestHeight) / scale;
  // A pixel of slack: the node's size in buffer pixels is rounded.
  const float slack = 1.0F / scale;
  if (logicalWidth <= nodeWidth + slack && logicalHeight <= nodeHeight + slack
      && (logicalWidth >= nodeWidth - slack || logicalHeight >= nodeHeight - slack)) {
    // One guest pixel is one buffer pixel; keep the origin on the pixel grid.
    const float x = std::floor(std::max(0.0F, nodeWidth - logicalWidth) * 0.5F * scale) / scale;
    const float y = std::floor(std::max(0.0F, nodeHeight - logicalHeight) * 0.5F * scale) / scale;
    return {x, y, logicalWidth, logicalHeight, true};
  }
  const float factor = std::min(nodeWidth / logicalWidth, nodeHeight / logicalHeight);
  const float width = logicalWidth * factor, height = logicalHeight * factor;
  return {(nodeWidth - width) * 0.5F, (nodeHeight - height) * 0.5F, width, height, false};
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
coffret::guestPoint(const Placement& placement, float x, float y, std::uint32_t guestWidth, std::uint32_t guestHeight) {
  if (placement.width <= 0 || placement.height <= 0 || x < placement.x || y < placement.y || x >= placement.x + placement.width
      || y >= placement.y + placement.height) {
    return std::nullopt;
  }
  const auto gx = static_cast<std::uint32_t>((x - placement.x) / placement.width * static_cast<float>(guestWidth));
  const auto gy = static_cast<std::uint32_t>((y - placement.y) / placement.height * static_cast<float>(guestHeight));
  return std::pair{std::min(gx, guestWidth - 1), std::min(gy, guestHeight - 1)};
}
