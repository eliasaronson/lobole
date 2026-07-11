#pragma once

#include <string>

// Headless entry point (--headless --remote <port>): document engine + remote
// server with no window, no Vulkan, no display server. Returns the process
// exit code. `fileToLoad` mirrors the GUI's startup-file argument (STEP).
int RunHeadless(int remotePort, const std::string& fileToLoad);
