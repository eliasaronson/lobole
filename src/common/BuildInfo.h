#pragma once

#include <string>

// What this binary IS: the answer to the first question anyone asks about a
// bug report, and the content of Help > About.
//
// Deliberately not just a version number. "Designer 0.1.0 misbehaves" is not
// reproducible; "0.1.0 at d9c30aa, Release, OpenCASCADE 7.9.3, RTX 4060" is —
// and this project has already spent a day on a bug that was really a kernel
// version difference, and another on one that only appeared in a sanitized
// build.
namespace BuildInfo {

// "0.1.0"
std::string Version();

// Short git hash, or "unknown" outside a checkout (a release tarball). Taken
// at CONFIGURE time, so it names the commit the build tree was configured on,
// not necessarily the one it was last compiled from.
std::string Commit();

// "Release", "Debug", plus what is compiled in that changes behaviour:
// sanitizers and the Dear ImGui Test Engine (which the --gui-tests and remote
// ui_action tiers need, and which a company over the licensing threshold
// would build without).
std::string Configuration();

// "7.9.3" — the geometry kernel, which decides what a given model does.
std::string KernelVersion();

// Every line above plus the renderer, as a block to paste into an issue.
// `gpu` and `vulkanApi` come from the running renderer ("" when headless).
std::string Summary(const std::string& gpu = "",
                    const std::string& vulkanApi = "");

} // namespace BuildInfo
