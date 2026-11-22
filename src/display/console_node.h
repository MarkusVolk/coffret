#pragma once

#include "display/frame_store.h"
#include "render/scene/custom_draw_node.h"

#include <cstdint>
#include <memory>
#include <string>

namespace coffret {
  // Shows what a guest draws: the picture of a FrameStore, as large as it fits with its shape kept,
  // pixel for pixel when the guest runs at the node's own resolution. Shared pixels are uploaded
  // rectangle by rectangle; a GPU buffer of the guest is imported as it is, without a copy.
  class ConsoleNode final : public CustomDrawNode {
  public:
    explicit ConsoleNode(std::shared_ptr<FrameStore> frames);
    ~ConsoleNode() override;

    void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

    // Where the picture sits in the node, for turning pointer positions into guest pixels.
    // `scale` is buffer pixels per logical pixel.
    Placement placement(float scale) const;

    struct Stats {
      // Frames drawn with new content since the node was made.
      std::uint64_t frames = 0;
      // "dmabuf", "shared memory" or "none"; with the reason when a GPU buffer could not be shown.
      std::string path = "none";
      std::uint32_t width = 0, height = 0;
    };
    Stats stats() const { return m_stats; }

  private:
    std::shared_ptr<FrameStore> m_frames;
    std::uint64_t m_id;
    mutable Stats m_stats;
  };
} // namespace coffret
