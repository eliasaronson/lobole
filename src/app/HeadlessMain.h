#pragma once

#include <string>

// Headless entry point (--headless --remote <port>): document engine + remote
// server with no window, no Vulkan, no display server. Returns the process
// exit code. `fileToLoad` mirrors the GUI's startup-file argument (STEP);
// `autosaveSeconds` mirrors --autosave (0 = off); `recordPath` mirrors
// --record ("" = off). `bindAll` mirrors --remote-bind-all: serve on
// 0.0.0.0 rather than loopback, which only makes sense inside a container.
int RunHeadless(int remotePort, const std::string& fileToLoad,
                double autosaveSeconds, const std::string& recordPath,
                bool bindAll = false);
