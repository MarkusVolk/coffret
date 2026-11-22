#include "check.h"
#include "display/frame_store.h"

#include <vector>

int main() {
  using coffret::Rect;
  // Damage adds up to the smallest rectangle around everything.
  CHECK((Rect{10, 10, 5, 5}.united({0, 20, 3, 3}) == Rect{0, 10, 15, 13}));
  CHECK((Rect{}.united({1, 2, 3, 4}) == Rect{1, 2, 3, 4}));
  CHECK((Rect{-5, -5, 20, 20}.clipped(10, 8) == Rect{0, 0, 10, 8}));
  CHECK((Rect{50, 50, 5, 5}.clipped(10, 10).empty()));

  coffret::FrameStore store;
  int changes = 0;
  store.setOnChanged([&changes] { ++changes; });
  CHECK(store.take().scanout.kind == coffret::Scanout::Kind::None);
  // Damage without a picture is nothing.
  store.damage({0, 0, 4, 4});
  CHECK(changes == 0);

  std::vector<std::uint8_t> pixels(64 * 32 * 4);
  store.setPixels(pixels.data(), 64, 32, 64 * 4, coffret::kPixmanX8R8G8B8, nullptr);
  CHECK(changes == 1);
  auto first = store.take();
  CHECK(first.scanout.kind == coffret::Scanout::Kind::Pixels);
  CHECK(first.scanout.width == 64 && first.scanout.height == 32);
  CHECK(first.damage.empty());

  store.damage({4, 4, 8, 8});
  store.damage({60, 30, 100, 100});
  auto second = store.take();
  CHECK(second.scanout.serial == first.scanout.serial);
  CHECK((second.damage == Rect{4, 4, 60, 28}));
  // Taken once.
  CHECK(store.take().damage.empty());

  // A new picture is a new serial, and old damage does not carry over.
  store.damage({0, 0, 1, 1});
  store.setPixels(pixels.data(), 32, 32, 32 * 4, coffret::kPixmanX8R8G8B8, nullptr);
  auto third = store.take();
  CHECK(third.scanout.serial != first.scanout.serial);
  CHECK(third.damage.empty());
  store.disable();
  CHECK(store.take().scanout.kind == coffret::Scanout::Kind::None);

  store.setCursor(2, 2, 1, 1, std::vector<std::uint8_t>(16));
  store.moveCursor(10, 20, true);
  const auto cursor = store.cursor();
  CHECK(cursor.width == 2 && cursor.x == 10 && cursor.y == 20 && cursor.visible && cursor.pixels->size() == 16);

  // The guest runs at the node's resolution: pixel for pixel, also at a fractional scale.
  auto exact = coffret::placeFrame(800, 600, 1.0F, 800, 600);
  CHECK(exact.exact && exact.x == 0 && exact.y == 0 && exact.width == 800 && exact.height == 600);
  exact = coffret::placeFrame(800, 600, 1.5F, 1200, 900);
  CHECK(exact.exact && exact.width == 800 && exact.height == 600);
  // Smaller than the node in one direction only, e.g. a rounded-down mode: still unscaled, centred.
  exact = coffret::placeFrame(800, 600, 1.0F, 800, 590);
  CHECK(exact.exact && exact.y == 5 && exact.height == 590);
  // Anything else is fitted with its shape kept.
  auto fitted = coffret::placeFrame(800, 600, 1.0F, 1600, 900);
  CHECK(!fitted.exact && fitted.width == 800 && fitted.height == 450 && fitted.x == 0 && fitted.y == 75);
  fitted = coffret::placeFrame(800, 600, 1.0F, 320, 200);
  CHECK(!fitted.exact && fitted.width == 800 && fitted.height == 500 && fitted.y == 50);
  CHECK(coffret::placeFrame(0, 600, 1.0F, 800, 600).width == 0);

  // Pointer positions map back to guest pixels; the bars around the picture are outside.
  CHECK(!coffret::guestPoint(fitted, 400, 10, 320, 200).has_value());
  const auto corner = coffret::guestPoint(fitted, 0, 50, 320, 200);
  CHECK(corner && corner->first == 0 && corner->second == 0);
  const auto centre = coffret::guestPoint(fitted, 400, 300, 320, 200);
  CHECK(centre && centre->first == 160 && centre->second == 100);
  const auto last = coffret::guestPoint(fitted, 799.9F, 549.9F, 320, 200);
  CHECK(last && last->first == 319 && last->second == 199);
  return 0;
}
