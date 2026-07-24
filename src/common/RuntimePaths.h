#pragma once

#include <filesystem>

namespace RuntimePaths {

// Directory containing the running executable.
std::filesystem::path ExecutableDir();

// Runtime data roots. Development builds use the staged directories beside
// the executable; installed builds use ../share/designer.
std::filesystem::path DataDir();
std::filesystem::path ShaderDir();

} // namespace RuntimePaths
