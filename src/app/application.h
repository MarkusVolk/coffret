#pragma once

#include <string>
#include <vector>

namespace coffret {
  struct Launch {
    // The QMP socket of a QEMU started by hand, whose console to show.
    std::string qmpSocket;
    // A named action to run once the window is there, with its parameter.
    std::string action;
    std::string parameter;
  };

  // Runs coffret until its window closes. A second launch hands its action to the first.
  int runApplication(const Launch& launch);
} // namespace coffret
