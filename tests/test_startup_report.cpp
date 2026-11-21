// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a start that cannot draw says, and the choices the environment makes
// (app/StartupReport.h, docs/architecture.md#when-the-renderer-cannot-start).
// tests/startup_failure_smoke.py drives the same through real failures.

#include <catch2/catch_test_macros.hpp>

#include "app/StartupReport.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using startup::Failure;

namespace {
bool Has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}
} // namespace

TEST_CASE("The failure text names the step, the result, the log and the way "
          "out",
          "[startup]") {
  Failure f;
  f.step = startup::step::kInstance;
  f.result = startup::VkResultName(-9);
  const std::string text =
      startup::FailureText(f, "/state/logs/20260101-000000-1.log",
                           "/state/last-startup-failure.txt");
  CHECK(Has(text, "could not create the Vulkan instance"));
  CHECK(Has(text, "VK_ERROR_INCOMPATIBLE_DRIVER"));
  CHECK(Has(text, "/state/logs/20260101-000000-1.log"));
  CHECK(Has(text, "/state/last-startup-failure.txt"));
  CHECK(Has(text, "--safe-mode"));
  CHECK(Has(text, "--offscreen"));
  CHECK(Has(text, "--headless"));
  CHECK(Has(text, "Graphics devices found: none."));
  // The driver step says what to install.
  CHECK(Has(text, "libvulkan1"));
  CHECK(Has(text, "LOBOLE_RENDERER=software"));
}

TEST_CASE("The failure text lists the devices and what each did", "[startup]") {
  Failure f;
  f.step = startup::step::kDevice;
  f.result = "VK_ERROR_INITIALIZATION_FAILED";
  f.devices = {"NVIDIA GeForce RTX 4060 (discrete)", "llvmpipe (software)"};
  f.attempts = {"NVIDIA GeForce RTX 4060: VK_ERROR_INITIALIZATION_FAILED at "
                "device creation",
                "llvmpipe: VK_ERROR_OUT_OF_HOST_MEMORY at device creation"};
  const std::string text = startup::FailureText(f, "", "/f.txt");
  CHECK(Has(text, "could not create the graphics device"));
  CHECK(Has(text, "  - NVIDIA GeForce RTX 4060 (discrete)"));
  CHECK(Has(text, "Tried, in order:"));
  CHECK(Has(text, "llvmpipe: VK_ERROR_OUT_OF_HOST_MEMORY at device creation"));
  CHECK(Has(text, "The log of this start: (none)"));
}

TEST_CASE("A display step talks about the display, not drivers", "[startup]") {
  Failure f;
  f.step = startup::step::kGlfwInit;
  f.detail = "GLFW said: X11: The DISPLAY environment variable is missing";
  const std::string text = startup::FailureText(f, "/l.log", "/f.txt");
  CHECK(Has(text, "could not start the windowing system"));
  CHECK(Has(text, "DISPLAY environment variable is missing"));
  CHECK(Has(text, "WAYLAND_DISPLAY"));
  CHECK_FALSE(Has(text, "Graphics devices found"));
  CHECK(Has(text, "--safe-mode"));
}

TEST_CASE("VkResult names, never the number alone", "[startup]") {
  CHECK(startup::VkResultName(-4) == "VK_ERROR_DEVICE_LOST");
  CHECK(startup::VkResultName(-1000000000) == "VK_ERROR_SURFACE_LOST_KHR");
  CHECK(startup::VkResultName(-1000001004) == "VK_ERROR_OUT_OF_DATE_KHR");
  CHECK(startup::VkResultName(0) == "VK_SUCCESS");
  CHECK(startup::VkResultName(-777) == "VkResult -777");
}

TEST_CASE("LOBOLE_RENDERER takes software or hardware, and refuses the rest",
          "[startup]") {
  startup::RendererChoice c = startup::RendererChoice::Hardware;
  std::string error;
  CHECK(startup::RendererChoiceFromString("", &c, &error));
  CHECK(c == startup::RendererChoice::Automatic);
  CHECK(startup::RendererChoiceFromString("software", &c, &error));
  CHECK(c == startup::RendererChoice::Software);
  CHECK(startup::RendererChoiceFromString("hardware", &c, &error));
  CHECK(c == startup::RendererChoice::Hardware);
  for (const char* bad : {"lavapipe", "Software", "gpu", "1"}) {
    error.clear();
    CHECK_FALSE(startup::RendererChoiceFromString(bad, &c, &error));
    CHECK(Has(error, "software"));
    CHECK(Has(error, "hardware"));
    CHECK(Has(error, bad));
  }
}

TEST_CASE("LOBOLE_SAFE_MODE is 0 or 1, and nothing else", "[startup]") {
  bool on = true;
  std::string error;
  CHECK(startup::ZeroOneFromString("LOBOLE_SAFE_MODE", "", &on, &error));
  CHECK_FALSE(on);
  CHECK(startup::ZeroOneFromString("LOBOLE_SAFE_MODE", "1", &on, &error));
  CHECK(on);
  CHECK(startup::ZeroOneFromString("LOBOLE_SAFE_MODE", "0", &on, &error));
  CHECK_FALSE(on);
  for (const char* bad : {"yes", "true", "2", "on"}) {
    error.clear();
    CHECK_FALSE(
        startup::ZeroOneFromString("LOBOLE_SAFE_MODE", bad, &on, &error));
    CHECK(Has(error, "LOBOLE_SAFE_MODE"));
    CHECK(Has(error, bad));
  }
}

TEST_CASE("LOBOLE_FAULT needs LOBOLE_ALLOW_DEBUG_CRASH and names its kinds",
          "[startup]") {
  startup::Fault f;
  std::string error;
  CHECK(startup::FaultFromString("", false, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::None);
  CHECK_FALSE(startup::FaultFromString("device_create", false, &f, &error));
  CHECK(Has(error, "LOBOLE_ALLOW_DEBUG_CRASH=1"));
  CHECK(startup::FaultFromString("device_create", true, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::DeviceCreate);
  CHECK(startup::FaultFromString("swapchain", true, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::Swapchain);
  CHECK(startup::FaultFromString("rebuild", true, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::Rebuild);
  CHECK(startup::FaultFromString("renderer_init:outline", true, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::RendererInit);
  CHECK(f.renderer == "outline");
  error.clear();
  CHECK_FALSE(
      startup::FaultFromString("renderer_init:teapot", true, &f, &error));
  CHECK(Has(error, "device_create"));
  CHECK(Has(error, "renderer_init:<mesh|outline"));
  CHECK_FALSE(startup::FaultFromString("device_lost", true, &f, &error));
  REQUIRE(startup::FaultFromString("software_restart", true, &f, &error));
  CHECK(f.kind == startup::Fault::Kind::SoftwareRestart);
}

TEST_CASE("The device ladder: the choice first, hardware next, software last",
          "[startup]") {
  using startup::Device;
  using startup::RendererChoice;
  const std::vector<Device> devices = {
      {"llvmpipe (LLVM 21)", true, 0},
      {"Intel(R) Arc(tm) Graphics", false, 1},
      {"NVIDIA GeForce RTX 4060", false, 2},
  };
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic) ==
        std::vector<int>{2, 1, 0});
  // A preference goes first; the rest keep their order.
  CHECK(startup::DeviceLadder(devices, "arc", RendererChoice::Automatic) ==
        std::vector<int>{1, 2, 0});
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Software) ==
        std::vector<int>{0});
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Hardware) ==
        std::vector<int>{2, 1});
  // A preference for a software device is honoured, and is then not a
  // fallback (it is the first rung).
  CHECK(startup::DeviceLadder(devices, "llvmpipe", RendererChoice::Automatic)
            .front() == 0);
  // A device that cannot show the window is not a rung at all.
  const std::vector<Device> xvfb = {
      {"Intel(R) Arc(tm) Graphics", false, -1},
      {"llvmpipe (LLVM 21)", true, 0},
  };
  CHECK(startup::DeviceLadder(xvfb, "", RendererChoice::Automatic) ==
        std::vector<int>{1});
  CHECK(startup::DeviceLadder(xvfb, "", RendererChoice::Hardware).empty());
  CHECK(startup::DeviceLadder({}, "", RendererChoice::Automatic).empty());
}

TEST_CASE("A rebuild after a loss tries the lost device first, then the "
          "usual order",
          "[startup]") {
  using startup::Device;
  using startup::RendererChoice;
  const std::vector<Device> devices = {
      {"llvmpipe (LLVM 21)", true, 0},
      {"Intel(R) Arc(tm) Graphics", false, 1},
      {"NVIDIA GeForce RTX 4060", false, 2},
  };
  // Lost on the Arc (a preference put it there): the Arc again, then the
  // usual order, which the NVIDIA heads, and the software device last.
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic, 1) ==
        std::vector<int>{1, 2, 0});
  // Lost on the usual first rung: the same order, the device once.
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic, 2) ==
        std::vector<int>{2, 1, 0});
  // Lost on the software device it had fallen back to: that first, then
  // the hardware that failed before, which may hold now.
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic, 0) ==
        std::vector<int>{0, 2, 1});
  // A device that is no longer a rung (gone, or outside LOBOLE_RENDERER) is
  // not put first.
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Software, 2) ==
        std::vector<int>{0});
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic, 7) ==
        std::vector<int>{2, 1, 0});
}

TEST_CASE("A second loss inside the window ends at the backstop", "[startup]") {
  using startup::LossVerdict;
  CHECK(startup::JudgeLoss(-1.0) == LossVerdict::Rebuild); // the first one
  CHECK(startup::JudgeLoss(0.5) == LossVerdict::Backstop); // its first frames
  CHECK(startup::JudgeLoss(startup::kRebuildRetryWindowSeconds - 0.01) ==
        LossVerdict::Backstop);
  CHECK(startup::JudgeLoss(startup::kRebuildRetryWindowSeconds) ==
        LossVerdict::Rebuild);
  CHECK(startup::JudgeLoss(3600.0) == LossVerdict::Rebuild);
  CHECK(std::string(startup::RebuildReasonName(
            startup::RebuildReason::DeviceLost)) == "device_lost");
  CHECK(std::string(startup::RebuildReasonName(
            startup::RebuildReason::GpuPreference)) == "gpu_preference");
}

TEST_CASE("The device-loss box says what happened and that the work is saved",
          "[startup]") {
  const std::string text = startup::DeviceLossText(
      "VK_ERROR_DEVICE_LOST at the frame fence", 2, "/state/crashes/x");
  CHECK(Has(text, "VK_ERROR_DEVICE_LOST at the frame fence"));
  CHECK(Has(text, "2 documents"));
  CHECK(Has(text, "/state/crashes/x"));
  CHECK(Has(text, "Restart"));
  CHECK(Has(startup::DeviceLossText("r", 0, ""), "Nothing was unsaved"));
}

namespace {
namespace fs = std::filesystem;

// A scratch directory, gone with the test.
struct Scratch {
  fs::path dir;
  Scratch() {
    std::random_device rd;
    dir = fs::temp_directory_path() /
          ("lobole_startup_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path Write(const std::string& rel, const std::string& text) const {
    const fs::path p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
    return p;
  }
};

std::string Manifest(const std::string& library) {
  return R"({"file_format_version": "1.0.1", "ICD": {"library_path": ")" +
         library + R"(", "api_version": "1.4.0"}})";
}

// Sets an environment variable for the test's length.
struct EnvVar {
  std::string name;
  std::string old;
  bool had;
  EnvVar(const char* n, const char* value) : name(n) {
    const char* o = std::getenv(n);
    had = o != nullptr;
    old = o ? o : "";
    if (value)
      setenv(n, value, 1);
    else
      unsetenv(n);
  }
  ~EnvVar() {
    if (had)
      setenv(name.c_str(), old.c_str(), 1);
    else
      unsetenv(name.c_str());
  }
};
} // namespace

TEST_CASE("The software driver is found where the loader looks, by its "
          "library",
          "[startup]") {
  Scratch s;
  const fs::path none = s.dir / "none";
  fs::create_directories(none);
  CHECK(startup::FindSoftwareDriverIn({none, s.dir / "missing"}).empty());

  // lavapipe among others, and a malformed file before it in name order.
  const fs::path icd = s.dir / "share" / "vulkan" / "icd.d";
  s.Write("share/vulkan/icd.d/a_broken.json", "{ not json");
  s.Write("share/vulkan/icd.d/intel_icd.x86_64.json",
          Manifest("/usr/lib/libvulkan_intel.so"));
  const fs::path lvp = s.Write("share/vulkan/icd.d/lvp_icd.x86_64.json",
                               Manifest("/usr/lib/x86_64/libvulkan_lvp.so"));
  s.Write("share/vulkan/icd.d/notes.txt", Manifest("libvulkan_lvp.so"));
  CHECK(startup::FindSoftwareDriverIn({none, icd}) == lvp);
  // The first directory that has one wins, as the loader's order says.
  const fs::path early =
      s.Write("etc/vulkan/icd.d/zz_lvp.json", Manifest("libvulkan_lvp.so"));
  CHECK(startup::FindSoftwareDriverIn({s.dir / "etc/vulkan/icd.d", icd}) ==
        early);
  CHECK(startup::ManifestLibrary(s.dir / "share/vulkan/icd.d/a_broken.json")
            .empty());
  CHECK(startup::ManifestLibrary(lvp) == "/usr/lib/x86_64/libvulkan_lvp.so");

  // The search directories come from the environment, XDG_DATA_DIRS split.
  {
    EnvVar data("XDG_DATA_DIRS", (s.dir / "share").c_str());
    EnvVar config("XDG_CONFIG_HOME", (s.dir / "cfg").c_str());
    EnvVar home("XDG_DATA_HOME", (s.dir / "home").c_str());
    const std::vector<fs::path> dirs = startup::DriverSearchDirs();
    REQUIRE(dirs.size() == 5);
    CHECK(dirs[0] == s.dir / "cfg" / "vulkan" / "icd.d");
    CHECK(dirs[1] == fs::path("/etc/vulkan/icd.d"));
    CHECK(dirs[2] == s.dir / "home" / "vulkan" / "icd.d");
    CHECK(dirs[3] == icd);
    CHECK(dirs[4] == fs::path("/run/opengl-driver/share/vulkan/icd.d"));
  }

  // LOBOLE_SOFTWARE_DRIVER: a manifest, none, or refused naming both.
  startup::SoftwareDriverChoice c;
  std::string error;
  REQUIRE(startup::SoftwareDriverFromString("", &c, &error));
  CHECK(c.kind == startup::SoftwareDriverChoice::Kind::Automatic);
  REQUIRE(startup::SoftwareDriverFromString("none", &c, &error));
  CHECK(c.kind == startup::SoftwareDriverChoice::Kind::None);
  CHECK(startup::FindSoftwareDriver(c).empty());
  REQUIRE(startup::SoftwareDriverFromString(lvp.string(), &c, &error));
  CHECK(c.kind == startup::SoftwareDriverChoice::Kind::Manifest);
#if defined(__linux__)
  CHECK(startup::FindSoftwareDriver(c) == lvp);
#else
  // Only Linux restarts on software (lavapipe); elsewhere nothing is found,
  // whatever the variable names.
  CHECK(startup::FindSoftwareDriver(c).empty());
#endif
  for (const std::string bad :
       {std::string("off"), (s.dir / "nothing.json").string(),
        (s.dir / "share/vulkan/icd.d/a_broken.json").string()}) {
    error.clear();
    CHECK_FALSE(startup::SoftwareDriverFromString(bad, &c, &error));
    CHECK(Has(error, "LOBOLE_SOFTWARE_DRIVER"));
    CHECK(Has(error, "none"));
    CHECK(Has(error, "manifest"));
  }
}

TEST_CASE("A start restarts on software only when that can cure it, once",
          "[startup]") {
  using startup::RendererChoice;
  Scratch s;
  const fs::path lvp =
      s.Write("lvp_icd.json", Manifest("/usr/lib/libvulkan_lvp.so"));
  Failure f;
  f.step = startup::step::kInstance;
  // The environment of a start that is not the restart: unclean.
  EnvVar a("VK_LOADER_LAYERS_DISABLE", nullptr);
  EnvVar b("VK_DRIVER_FILES", nullptr);
  EnvVar c("VK_ICD_FILENAMES", "/nonexistent");
  EnvVar d("VK_ADD_DRIVER_FILES", nullptr);
  CHECK(startup::ShouldRestartInSoftware(f, false, RendererChoice::Automatic,
                                         lvp));
  // Not in the restart itself, not when the user chose, not without a
  // driver, and not for a step software does not cure.
  CHECK_FALSE(startup::ShouldRestartInSoftware(f, true,
                                               RendererChoice::Automatic, lvp));
  CHECK_FALSE(startup::ShouldRestartInSoftware(f, false,
                                               RendererChoice::Hardware, lvp));
  CHECK_FALSE(startup::ShouldRestartInSoftware(f, false,
                                               RendererChoice::Software, lvp));
  CHECK_FALSE(startup::ShouldRestartInSoftware(f, false,
                                               RendererChoice::Automatic, {}));
  for (const char* step : {startup::step::kGlfwInit, startup::step::kWindow,
                           startup::step::kVulkanLoader}) {
    Failure g = f;
    g.step = step;
    CHECK_FALSE(startup::ShouldRestartInSoftware(
        g, false, RendererChoice::Automatic, lvp));
  }
  for (const char* step : {startup::step::kNoDevice, startup::step::kDevice,
                           startup::step::kSurface, startup::step::kSwapchain,
                           startup::step::kMeshRenderer}) {
    Failure g = f;
    g.step = step;
    CHECK(startup::ShouldRestartInSoftware(g, false, RendererChoice::Automatic,
                                           lvp));
  }
  // lavapipe failed through an unclean loader: worth the restart; through
  // the loader the restart sets up, not.
  Failure soft = f;
  soft.step = startup::step::kMeshRenderer;
  soft.softwareTried = true;
  CHECK(startup::ShouldRestartInSoftware(soft, false, RendererChoice::Automatic,
                                         lvp));
  CHECK_FALSE(startup::LoaderIsClean(lvp));
  startup::SetSoftwareEnvironment(lvp);
  CHECK(startup::LoaderIsClean(lvp));
  CHECK_FALSE(startup::ShouldRestartInSoftware(soft, false,
                                               RendererChoice::Automatic, lvp));
  CHECK(startup::ShouldRestartInSoftware(f, false, RendererChoice::Automatic,
                                         lvp));
  const std::vector<std::string> env = startup::LoaderEnvironment();
  CHECK(std::find(env.begin(), env.end(),
                  "VK_LOADER_LAYERS_DISABLE=~implicit~") != env.end());
}

TEST_CASE("The fallback record, the attempt and the skip list round-trip",
          "[startup]") {
  Scratch s;
  CHECK_FALSE(startup::ReadFallbackRecord(s.dir));
  startup::FallbackRecord r;
  r.failure.step = startup::step::kInstance;
  r.failure.result = "VK_ERROR_INCOMPATIBLE_DRIVER";
  r.failure.attempts = {"NVIDIA: VK_ERROR_DEVICE_LOST at device creation"};
  r.manifest = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json";
  r.build = startup::BuildId();
  r.environment = {"VK_ICD_FILENAMES=/nonexistent"};
  REQUIRE(startup::WriteFallbackRecord(s.dir, r));
  const auto back = startup::ReadFallbackRecord(s.dir);
  REQUIRE(back);
  CHECK(back->failure.step == r.failure.step);
  CHECK(back->failure.result == r.failure.result);
  CHECK(back->failure.attempts == r.failure.attempts);
  CHECK(back->manifest == r.manifest);
  CHECK(back->environment == r.environment);
  CHECK(Has(startup::FailureReason(back->failure),
            "could not create the Vulkan instance: "
            "VK_ERROR_INCOMPATIBLE_DRIVER"));
  // What to do names what was in the way, and the way to the report.
  const std::string advice =
      startup::FallbackAdvice(back->failure, back->environment);
  CHECK(Has(advice, "VK_ICD_FILENAMES=/nonexistent"));
  CHECK(Has(advice, "libvulkan1"));
  CHECK(Has(advice, "3D acceleration"));
  CHECK(Has(advice, "LOBOLE_SOFTWARE_DRIVER=none"));
  // No variable set at the instance: a layer, which the restart turned off.
  CHECK(Has(startup::FallbackAdvice(back->failure, {}), "layer"));

  CHECK_FALSE(startup::ReadGraphicsAttempt(s.dir));
  startup::WriteGraphicsAttempt(
      s.dir, {"device", "NVIDIA GeForce RTX 4060", "595.112.07", "b", 4242});
  auto attempt = startup::ReadGraphicsAttempt(s.dir);
  REQUIRE(attempt);
  CHECK(attempt->stage == "device");
  CHECK(attempt->device == "NVIDIA GeForce RTX 4060");
  CHECK(attempt->driver == "595.112.07");
  CHECK(attempt->pid == 4242);
  startup::RemoveGraphicsAttempt(s.dir);
  CHECK_FALSE(startup::ReadGraphicsAttempt(s.dir));
  // A stage it does not know is no attempt: nothing is skipped on it.
  s.Write("graphics-attempt.json", R"({"stage": "dev"})");
  CHECK_FALSE(startup::ReadGraphicsAttempt(s.dir));

  CHECK(startup::ReadSkippedDevices(s.dir).empty());
  REQUIRE(startup::WriteSkippedDevices(
      s.dir, {{"NVIDIA GeForce RTX 4060", "595.112.07", "b", "it crashed"}}));
  const auto skipped = startup::ReadSkippedDevices(s.dir);
  REQUIRE(skipped.size() == 1);
  CHECK(skipped[0].device == "NVIDIA GeForce RTX 4060");
  CHECK(skipped[0].why == "it crashed");
}

TEST_CASE("A skipped device is not a rung", "[startup]") {
  using startup::Device;
  using startup::RendererChoice;
  std::vector<Device> devices = {
      {"llvmpipe (LLVM 21)", true, 0},
      {"Intel(R) Arc(tm) Graphics", false, 1},
      {"NVIDIA GeForce RTX 4060", false, 2},
  };
  devices[2].skipped = true;
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic) ==
        std::vector<int>{1, 0});
  // Not even when it is preferred, or was the device lost.
  CHECK(startup::DeviceLadder(devices, "NVIDIA", RendererChoice::Automatic) ==
        std::vector<int>{1, 0});
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic, 2) ==
        std::vector<int>{1, 0});
  devices[0].skipped = true;
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Software).empty());
  devices[1].skipped = true;
  CHECK(startup::DeviceLadder(devices, "", RendererChoice::Automatic).empty());
}

TEST_CASE("The report after a failed software restart says both", "[startup]") {
  Failure f;
  f.step = startup::step::kInstance;
  f.result = "VK_ERROR_INCOMPATIBLE_DRIVER";
  f.softwareAttempt = "could not create the graphics device: "
                      "VK_ERROR_INITIALIZATION_FAILED (/lvp.json)";
  const std::string text = startup::FailureText(f, "", "/f.txt");
  CHECK(Has(text, "could not create the Vulkan instance"));
  CHECK(Has(text, "Software rendering was tried next, and it failed too: "
                  "could not create the graphics device"));
  // It was tried: the report does not suggest it.
  CHECK_FALSE(Has(text, "LOBOLE_RENDERER=software"));
}
