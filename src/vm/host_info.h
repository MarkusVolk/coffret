#pragma once

#include <string>

namespace coffret {
  // The GPU guests render on: the first render node of a real device, e.g. "/dev/dri/renderD129";
  // empty when there is none. Test devices like vgem come first on some hosts and cannot do it.
  std::string defaultRenderNode();
  unsigned hostProcessors();
  unsigned hostMemoryMiB();
} // namespace coffret
