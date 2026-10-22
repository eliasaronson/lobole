// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/RuntimePaths.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h> // _NSGetExecutablePath
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace RuntimePaths {

std::filesystem::path ExecutableDir() {
#if defined(_WIN32)
  // The wide API: the path is UTF-16 whatever the code page, and the
  // install folder is "Loböle".
  wchar_t result[MAX_PATH];
  const DWORD count = GetModuleFileNameW(nullptr, result, MAX_PATH);
  if (count > 0 && count < MAX_PATH)
    return std::filesystem::path(result).parent_path();
  return std::filesystem::current_path();
#elif defined(__APPLE__)
  char result[PATH_MAX];
  uint32_t size = sizeof(result);
  if (_NSGetExecutablePath(result, &size) == 0)
    return std::filesystem::canonical(result).parent_path();
  return std::filesystem::current_path();
#else
  char result[PATH_MAX];
  const ssize_t count = readlink("/proc/self/exe", result, sizeof(result) - 1);
  if (count > 0) {
    result[count] = '\0';
    return std::filesystem::path(result).parent_path();
  }
  return std::filesystem::current_path();
#endif
}

std::filesystem::path DataDir() {
  if (const char* overrideDir = std::getenv("LOBOLE_DATA_DIR"))
    return overrideDir;

  const std::filesystem::path executable = ExecutableDir();
  const std::filesystem::path development = executable / "assets";
  if (std::filesystem::is_directory(development))
    return development;

  // One level up, for a binary that lives UNDER the staging root rather than
  // in it: build/tests/designer_tests has to find build/assets. Added when a
  // golden document carrying a Fastener regenerated to zero bodies in the test
  // binary and to a screw in the app — the data was staged correctly and only
  // the finder was short. An installed /usr/bin/designer looks for /usr/assets
  // here, does not find it, and falls through to share/lobole as before.
  const std::filesystem::path parent =
      (executable / ".." / "assets").lexically_normal();
  if (std::filesystem::is_directory(parent))
    return parent;

  return (executable / ".." / "share" / "lobole").lexically_normal();
}

std::filesystem::path EnvironmentHdri() {
  return DataDir() / "hdri" / kEnvironmentHdriFile;
}

std::filesystem::path PartsDir() { return DataDir() / "parts"; }

namespace {

// The per-user root the three directories below hang off. Windows gets ONE
// folder — %LOCALAPPDATA%\Lobole — so there is a single thing to find when
// a setting misbehaves and a single thing to delete for a clean slate. On
// Linux each kind has its own XDG variable and they are resolved separately.
#if defined(_WIN32)
std::filesystem::path WindowsAppDir() {
  if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
    return std::filesystem::path(local) / "Lobole";
  if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
    return std::filesystem::path(profile) / "AppData" / "Local" / "Lobole";
  return std::filesystem::temp_directory_path() / "Lobole";
}
#endif

} // namespace

std::filesystem::path UserConfigDir() {
#if defined(_WIN32)
  return WindowsAppDir() / "config";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / "lobole";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".config" / "lobole";
  return std::filesystem::temp_directory_path() / "lobole-config";
#endif
}

std::filesystem::path UserStateDir() {
#if defined(_WIN32)
  return WindowsAppDir() / "state";
#else
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / "lobole";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".local" / "state" / "lobole";
  return std::filesystem::temp_directory_path() / "lobole-state";
#endif
}

std::filesystem::path UserCacheDir() {
#if defined(_WIN32)
  return WindowsAppDir() / "cache";
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / "lobole";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".cache" / "lobole";
  return std::filesystem::temp_directory_path() / "lobole";
#endif
}

std::filesystem::path ShaderDir() {
  const std::filesystem::path development =
      ExecutableDir() / "libraries" / "shaders";
  if (std::filesystem::is_directory(development))
    return development;
  return DataDir() / "shaders";
}

} // namespace RuntimePaths
