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
  char result[MAX_PATH];
  const DWORD count = GetModuleFileNameA(nullptr, result, sizeof(result));
  if (count > 0 && count < sizeof(result))
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
  if (const char* overrideDir = std::getenv("DESIGNER_DATA_DIR"))
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
  // here, does not find it, and falls through to share/designer as before.
  const std::filesystem::path parent =
      (executable / ".." / "assets").lexically_normal();
  if (std::filesystem::is_directory(parent))
    return parent;

  return (executable / ".." / "share" / "designer").lexically_normal();
}

std::filesystem::path EnvironmentHdri() {
  return DataDir() / "hdri" / kEnvironmentHdriFile;
}

std::filesystem::path PartsDir() { return DataDir() / "parts"; }

std::filesystem::path ShaderDir() {
  const std::filesystem::path development =
      ExecutableDir() / "libraries" / "shaders";
  if (std::filesystem::is_directory(development))
    return development;
  return DataDir() / "shaders";
}

} // namespace RuntimePaths
