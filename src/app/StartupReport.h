// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// What the app does when the graphics path cannot start, or stops: the
// failure as a value, the words a user reads, where they are written, and the
// native box that says them when there is no terminal to read
// (docs/architecture.md#when-the-renderer-cannot-start).
//
// Vulkan-free on purpose, like core: the unit tests check the text, the
// parsers and the device ladder without an instance. app/GpuOwners.cpp
// turns a VkResult into its name (VkResultName) and hands that over.
namespace startup {

// The exit code of a start that found no usable renderer. 1 is a bad flag
// or a taken port, 2 a refused environment variable; a launcher (the
// AppImage smoke, a desktop file's wrapper) can tell this one apart.
inline constexpr int kExitNoRenderer = 3;

// The steps, in words, as the message and the tests both name them.
namespace step {
inline constexpr const char* kGlfwInit = "start the windowing system";
inline constexpr const char* kWindow = "open the main window";
inline constexpr const char* kVulkanLoader = "load Vulkan";
inline constexpr const char* kInstance = "create the Vulkan instance";
inline constexpr const char* kNoDevice = "find a graphics device";
inline constexpr const char* kDevice = "create the graphics device";
inline constexpr const char* kSurface = "open the window surface";
inline constexpr const char* kSwapchain = "set up the swapchain";
inline constexpr const char* kMeshRenderer = "initialise the mesh renderer";
} // namespace step

// One failed start. `result` is a VkResult's NAME ("VK_ERROR_DEVICE_LOST"),
// never the number alone; empty when the step is not a Vulkan call.
struct Failure {
  std::string step;
  std::string result;
  // What else is known: GLFW's last error, the preference that was not
  // honoured.
  std::string detail;
  // Every device the loader offered, as "NVIDIA GeForce RTX 4060 (discrete)".
  std::vector<std::string> devices;
  // Each device tried and how it failed, in the order tried:
  // "NVIDIA GeForce RTX 4060: VK_ERROR_INITIALIZATION_FAILED at device
  // creation".
  std::vector<std::string> attempts;
};

class StartupError : public std::runtime_error {
public:
  explicit StartupError(Failure f);
  const Failure& failure() const { return m_failure; }

private:
  Failure m_failure;
};

// "VK_ERROR_DEVICE_LOST"; "VkResult -1000001004" for one this table does not
// know. Takes the int so this header needs no Vulkan.
std::string VkResultName(int result);

// The message, in words: what failed, what Vulkan said, the devices, what to
// try for that step, the log of this start and where the text is kept.
std::string FailureText(const Failure& f, const std::string& logPath,
                        const std::filesystem::path& failureFile);

// <state>/last-startup-failure.txt: the last start that found no renderer.
std::filesystem::path FailureFile(const std::filesystem::path& stateDir);

// Says it everywhere it can be read: stderr, the session log, the failure
// file, and — for a windowed start whose stderr is not a terminal, unless
// LOBOLE_NO_DIALOG=1 — a native box (DialogsWanted). Returns kExitNoRenderer.
int ReportStartupFailure(const Failure& f,
                         const std::filesystem::path& stateDir,
                         const std::string& logPath, bool windowed);

// --- native message boxes -----------------------------------------------
//
// MessageBoxW on Windows. On Linux `zenity`, else `kdialog`, else a desktop
// notification over D-Bus (the portal's inside a Flatpak), which cannot ask
// anything and so names the text file. None of them is shown when
// LOBOLE_NO_DIALOG=1 (every test launch: tests/smoke_app.py), because a box
// nobody will click is a test that never ends.

// LOBOLE_NO_DIALOG=1, or stderr is a terminal (someone is reading it).
bool DialogsWanted();
// Shows `text` and waits for it to be dismissed. False when nothing could
// show it.
// `brief` is what a desktop notification has room for, which cannot show the
// whole text: it should name where the rest is.
bool ShowError(const std::string& title, const std::string& text,
               const std::string& brief);

// A Restart / Quit question that does not block the frame loop: the remote
// keeps serving while it is open. Poll() each iteration.
class Question {
public:
  enum class Answer { Pending, Restart, Quit, Unanswerable };
  Question(const std::string& title, const std::string& text);
  ~Question();
  Question(const Question&) = delete;
  Question& operator=(const Question&) = delete;
  Answer Poll();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// --- the environment ------------------------------------------------------

// LOBOLE_RENDERER: which rung of the device ladder to use. Unset or empty is
// automatic; anything but `software` or `hardware` is refused, naming them.
enum class RendererChoice { Automatic, Software, Hardware };
[[nodiscard]] bool RendererChoiceFromString(std::string_view text,
                                            RendererChoice* out,
                                            std::string* error);

// A switch in the environment (LOBOLE_SAFE_MODE, LOBOLE_NO_DIALOG): `1` is
// on, `0`, empty or unset is off, and anything else is refused, naming the
// variable and the two values — never read as whichever it looks closer to.
[[nodiscard]] bool ZeroOneFromString(std::string_view name,
                                     std::string_view text, bool* out,
                                     std::string* error);

// LOBOLE_FAULT, honoured only with LOBOLE_ALLOW_DEBUG_CRASH=1: a startup
// failure on purpose, for the tests that judge one.
//   device_create          the first device tried fails vkCreateDevice
//   swapchain              the first device tried cannot set up a swapchain
//   renderer_init:<name>   that renderer's Initialize fails (kFaultRenderers)
//   rebuild                a device REBUILD fails device creation on every
//                          rung (start-up is untouched), so a lost device
//                          ends at the backstop (docs/rendering/README.md
//                          #rebuilding-the-device)
struct Fault {
  enum class Kind { None, DeviceCreate, Swapchain, RendererInit, Rebuild };
  Kind kind = Kind::None;
  std::string renderer; // RendererInit only
};
inline constexpr const char* kFaultRenderers[] = {
    "mesh", "outline", "sketch", "underlay", "field", "stock", "raytracing"};
[[nodiscard]] bool FaultFromString(std::string_view text, bool allowed,
                                   Fault* out, std::string* error);

// --- the device ladder ----------------------------------------------------

struct Device {
  std::string name;
  bool cpu = false; // VK_PHYSICAL_DEVICE_TYPE_CPU: lavapipe, llvmpipe
  int score = 0;    // devicepref's: < 0 ineligible (cannot present here)
};

// The order to try devices in, as indices: the one devicepref::Choose picks
// for `preferred` first, then the other hardware by score (first on a tie),
// then the software ones. Ineligible devices are not rungs. `Software` keeps
// only the CPU devices and `Hardware` only the rest.
//
// `first` >= 0 puts that device on the first rung whatever the preference
// says, when it is eligible: a rebuild after a loss tries the device that
// was lost again first (the spec's lost state belongs to the logical device;
// the physical one is usually fine), then the usual order.
std::vector<int> DeviceLadder(const std::vector<Device>& devices,
                              const std::string& preferred,
                              RendererChoice choice, int first = -1);

// --- rebuilding the device ------------------------------------------------
//
// The two things that rebuild the device in place
// (docs/rendering/README.md#rebuilding-the-device), and nothing else does:
// a VK_ERROR_DEVICE_LOST (or debug_fault's), and a change of the gpu_device
// preference that moves the first rung.
enum class RebuildReason { DeviceLost, GpuPreference };
const char* RebuildReasonName(RebuildReason reason); // "device_lost", ...

// A device that dies again this soon after a rebuild is not one a second
// rebuild will save.
inline constexpr double kRebuildRetryWindowSeconds = 60.0;

// Whether a loss is rebuilt in place or ends at the backstop (the lost
// state, its report and Restart). `secondsSinceLastRebuild` < 0 = there has
// been none.
enum class LossVerdict { Rebuild, Backstop };
LossVerdict JudgeLoss(double secondsSinceLastRebuild);

// --- device loss ----------------------------------------------------------

// The box a lost device shows: what happened, that the work is saved for
// recovery (`autosaved` documents; the report folder), Restart or Quit.
std::string DeviceLossText(const std::string& reason, int autosaved,
                           const std::filesystem::path& reportDir);

// Starts this executable again with `args` (argv[1..], as given), replacing
// this process where the OS can (execv: the pid stays, so a launcher that
// watches it keeps watching) and otherwise starting it and returning true for
// the caller to exit. False with `error` when it could not. The caller has
// already let go of what the new process will want: the remote port, the
// session marker, a terminal panel's child.
bool Relaunch(const std::vector<std::string>& args, std::string* error);

} // namespace startup
