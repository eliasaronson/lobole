// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>

// Headless entry point (--headless --remote <port>): document engine + remote
// server with no window, no Vulkan, no display server. Returns the process
// exit code. `fileToLoad` mirrors the GUI's startup-file argument (STEP);
// `autosaveSeconds` mirrors --autosave (0 = off); `recordPath` mirrors
// --record ("" = off). `bindAll` mirrors --remote-bind-all: serve on
// 0.0.0.0 rather than loopback, which only makes sense inside a container.
// `allowLargeMesh` mirrors --allow-large-mesh: a startup mesh past the
// triangle limit is imported rather than refused.
int RunHeadless(int remotePort, const std::string& fileToLoad,
                double autosaveSeconds, const std::string& recordPath,
                bool bindAll = false, bool allowLargeMesh = false);
