// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Which of our OpenCASCADE patches (nix/patches/) the kernel this build was
// configured against carries. The install lists them in
// share/lobole/occt-patches (nix/occt-overlay.nix, the AppImage's deps.sh,
// the Windows cross build); CMake reads the list at configure time. A build
// against apt's OpenCASCADE (CI's stock Ubuntu jobs) carries none.

#include <string>

namespace occpatch {

// Whether the patch `name` (its file name, with or without ".patch") is in.
bool Carries(const char* name);

// The list, comma-separated: what `ping` reports as `occ_patches`.
std::string List();

} // namespace occpatch
