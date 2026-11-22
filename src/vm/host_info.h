#pragma once

#include <string>

namespace coffret {
  // The GPU guests render on: the first render node of a real device, e.g. "/dev/dri/renderD129";
  // empty when there is none. Test devices like vgem come first on some hosts and cannot do it.
  std::string defaultRenderNode();
  // virtiofsd where distributions put it, or in the PATH; empty when there is none.
  std::string findVirtiofsd();
  // The folder with the viogpu3d driver for Windows guests (viogpu3d.inf and its files); empty
  // when there is none, or when QEMU cannot hand Direct3D to the host (no Neptune). Asks QEMU once.
  std::string direct3dDriverFolder();
  unsigned hostProcessors();
  unsigned hostMemoryMiB();
} // namespace coffret
