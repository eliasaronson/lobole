#pragma once

#include <filesystem>

namespace RuntimePaths {

// Directory containing the running executable.
std::filesystem::path ExecutableDir();

// Runtime data roots. Development builds use the staged directories beside
// the executable; installed builds use ../share/designer.
std::filesystem::path DataDir();
std::filesystem::path ShaderDir();

// The studio environment map: image-based lighting for the raster viewport,
// and for the path-traced still both the light and every reflection in it.
// The FILENAME lives here rather than at each use — it was written out at
// four C++ sites and two CMake ones, which is how a resolution nobody
// revisits gets six places to be wrong in.
inline constexpr const char* kEnvironmentHdriFile =
    "brown_photostudio_02_2k.hdr";
std::filesystem::path EnvironmentHdri();

// The vendored standard-parts dimension tables (docs/parts/README.md). Same
// reason the HDRI filename lives here rather than at its uses: a directory
// name spelled out at each site is a directory name with several places to be
// wrong in.
std::filesystem::path PartsDir();

// Writable per-user directories, by what they hold: what the user CHOSE
// (settings), what the app can regenerate but must survive a restart
// (autosaves, the recent list, the instance registry), and what it can throw
// away (the precomputed IBL maps). On Linux these are the XDG directories, and
// the executable's own directory is never one of them — it is read-only under
// Nix, Flatpak and an installed prefix alike.
//
// They live here, together, because Windows has no $HOME and no XDG_*: all
// three used to fall through to `temp_directory_path()`, which on Windows is
// `%TEMP%` — a directory the OS sweeps. Settings, recovery autosaves and the
// running-instance registry all landed somewhere Windows deletes.
std::filesystem::path UserConfigDir();
std::filesystem::path UserStateDir();
std::filesystem::path UserCacheDir();

} // namespace RuntimePaths
