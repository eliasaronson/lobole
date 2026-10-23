// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a start that cannot draw says, and the choices the environment makes
// (app/StartupReport.h, docs/architecture.md#when-the-renderer-cannot-start).
// tests/startup_failure_smoke.py drives the same through real failures.

#include <catch2/catch_test_macros.hpp>

#include "app/StartupReport.h"

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
