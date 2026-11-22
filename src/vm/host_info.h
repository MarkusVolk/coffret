#pragma once

#include <string>

namespace coffret {
  // The GPU guests render on: the first render node of a real device, e.g. "/dev/dri/renderD129";
  // empty when there is none. Test devices like vgem come first on some hosts and cannot do it.
  std::string defaultRenderNode();
  // virtiofsd where distributions put it, or in the PATH; empty when there is none.
  std::string findVirtiofsd();
  unsigned hostProcessors();
  unsigned hostMemoryMiB();
} // namespace coffret
