#include "common/BuildInfo.h"

#include "common/DesignerVersion.h" // the DESIGNER_VERSION fallback

#ifdef HAVE_OPENCASCADE
#include <Standard_Version.hxx>
#endif

namespace BuildInfo {

std::string Version() { return DESIGNER_VERSION; }

std::string Commit() {
#ifdef DESIGNER_GIT_COMMIT
  const std::string commit = DESIGNER_GIT_COMMIT;
  return commit.empty() ? "unknown" : commit;
#else
  return "unknown";
#endif
}

std::string Configuration() {
#ifdef NDEBUG
  std::string s = "Release";
#else
  std::string s = "Debug";
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(DESIGNER_SANITIZER_SOURCE_DIR)
  s += ", sanitizers";
#endif
#ifdef HAVE_TEST_ENGINE
  s += ", test engine";
#else
  s += ", no test engine";
#endif
  return s;
}

std::string KernelVersion() {
#ifdef OCC_VERSION_COMPLETE
  return OCC_VERSION_COMPLETE;
#else
  return "none";
#endif
}

std::string Summary(const std::string& gpu, const std::string& vulkanApi) {
  std::string s = "Lobole " + Version() + " (" + Commit() + ")\n";
  s += "Build: " + Configuration() + "\n";
  s += "OpenCASCADE: " + KernelVersion() + "\n";
  if (!gpu.empty()) {
    s += "GPU: " + gpu;
    if (!vulkanApi.empty())
      s += " (Vulkan " + vulkanApi + ")";
    s += "\n";
  }
  return s;
}

} // namespace BuildInfo
