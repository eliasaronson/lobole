#include "common/RuntimePaths.h"

#include <cstdlib>
#include <limits.h>
#include <unistd.h>

namespace RuntimePaths {

std::filesystem::path ExecutableDir() {
  char result[PATH_MAX];
  const ssize_t count = readlink("/proc/self/exe", result, sizeof(result) - 1);
  if (count > 0) {
    result[count] = '\0';
    return std::filesystem::path(result).parent_path();
  }
  return std::filesystem::current_path();
}

std::filesystem::path DataDir() {
  if (const char* overrideDir = std::getenv("DESIGNER_DATA_DIR"))
    return overrideDir;

  const std::filesystem::path executable = ExecutableDir();
  const std::filesystem::path development = executable / "assets";
  if (std::filesystem::is_directory(development))
    return development;

  return (executable / ".." / "share" / "designer").lexically_normal();
}

std::filesystem::path ShaderDir() {
  const std::filesystem::path development =
      ExecutableDir() / "libraries" / "shaders";
  if (std::filesystem::is_directory(development))
    return development;
  return DataDir() / "shaders";
}

} // namespace RuntimePaths
