// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/StartupReport.h"

#include "common/DevicePreference.h"
#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(HAVE_DBUS)
#include <dbus/dbus.h>
#endif

namespace fs = std::filesystem;

namespace startup {

namespace {

constexpr const char* kHelpUrl =
    "https://github.com/eliasaronson/lobole/blob/main/docs/packaging/"
    "README.md#when-it-will-not-start";

bool IsDisplayStep(const std::string& step) {
  return step == step::kGlfwInit || step == step::kWindow;
}

bool IsDriverStep(const std::string& step) {
  return step == step::kVulkanLoader || step == step::kInstance ||
         step == step::kNoDevice;
}

std::string Tips(const Failure& f) {
  std::ostringstream o;
  const auto tip = [&](const std::string& t) { o << "  - " << t << "\n"; };
  if (IsDisplayStep(f.step)) {
    tip("Loböle's window needs a desktop session (X11 or Wayland). On one, "
        "check that DISPLAY or WAYLAND_DISPLAY is set in the environment it "
        "was started from.");
  }
  if (IsDriverStep(f.step) || f.step == step::kDevice ||
      f.step == step::kSwapchain || f.step == step::kSurface ||
      f.step == step::kMeshRenderer) {
#if defined(_WIN32)
    tip("Install or update the graphics driver from the GPU's maker (NVIDIA, "
        "AMD or Intel). The driver Windows installs by itself often has no "
        "Vulkan.");
#else
    tip("Install or update the graphics driver and the Vulkan loader:\n"
        "      Debian, Ubuntu:  sudo apt install libvulkan1 "
        "mesa-vulkan-drivers\n"
        "      Fedora:          sudo dnf install vulkan-loader "
        "mesa-vulkan-drivers\n"
        "      Arch:            sudo pacman -S vulkan-icd-loader (and "
        "vulkan-intel, vulkan-radeon or nvidia-utils)");
#endif
    tip("In a virtual machine, turn on 3D acceleration in its display "
        "settings.");
#if !defined(_WIN32)
    tip("Render in software, on Mesa's lavapipe (slower, and no path-traced "
        "stills): LOBOLE_RENDERER=software lobole");
#endif
  }
  tip("Start with default settings and the default layout, opening no "
      "file: lobole --safe-mode");
  tip("Without a window, the scripting interface still works: lobole "
      "--offscreen --remote 0 renders, lobole --headless --remote 0 does not "
      "need a GPU at all.");
  return o.str();
}

#if !defined(_WIN32)
// Runs `argv` and waits; the exit status, or -1 when it could not start.
int RunAndWait(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
    return -1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

pid_t Spawn(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
    return -1;
  return pid;
}

// A box tool's verdict: shown (0 or 1: a button was pressed or the box
// closed), or not shown at all (127 not found, 255 its own error).
bool Shown(int status) { return status == 0 || status == 1; }
#endif

// The last resort, which asks nothing: a desktop notification. Inside a
// Flatpak the portal's, since the sandbox does not reach the session's
// notification service by name.
bool Notify(const std::string& title, const std::string& body) {
#if defined(HAVE_DBUS)
  DBusError err;
  dbus_error_init(&err);
  DBusConnection* bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (bus == nullptr) {
    dbus_error_free(&err);
    return false;
  }
  dbus_connection_set_exit_on_disconnect(bus, FALSE);
  const bool flatpak = fs::exists("/.flatpak-info");
  DBusMessage* msg =
      flatpak
          ? dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                         "/org/freedesktop/portal/desktop",
                                         "org.freedesktop.portal.Notification",
                                         "AddNotification")
          : dbus_message_new_method_call("org.freedesktop.Notifications",
                                         "/org/freedesktop/Notifications",
                                         "org.freedesktop.Notifications",
                                         "Notify");
  bool ok = false;
  if (msg != nullptr) {
    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    const char* t = title.c_str();
    const char* b = body.c_str();
    if (flatpak) {
      // AddNotification(s id, a{sv} notification)
      const char* id = "lobole-startup";
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &id);
      DBusMessageIter dict;
      dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
      const auto entry = [&](const char* key, const char* value) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr,
                                         &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &value);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
      };
      entry("title", t);
      entry("body", b);
      dbus_message_iter_close_container(&it, &dict);
    } else {
      // Notify(s app, u replaces, s icon, s summary, s body, as actions,
      //        a{sv} hints, i timeout)
      const char* app = "Loböle";
      const char* icon = "org.lobole.Lobole";
      dbus_uint32_t replaces = 0;
      dbus_int32_t timeout = 0; // 0: stays until dismissed
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &t);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &b);
      DBusMessageIter actions, hints;
      dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &actions);
      dbus_message_iter_close_container(&it, &actions);
      dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &hints);
      dbus_message_iter_close_container(&it, &hints);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);
    }
    // Bounded: a session bus that answers nothing must not hang an exit.
    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(bus, msg, 2000, &err);
    ok = reply != nullptr;
    if (reply != nullptr)
      dbus_message_unref(reply);
    dbus_message_unref(msg);
  }
  dbus_error_free(&err);
  dbus_connection_close(bus);
  dbus_connection_unref(bus);
  return ok;
#else
  (void)title;
  (void)body;
  return false;
#endif
}

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      w.data(), n);
  return w;
}
#endif

} // namespace

StartupError::StartupError(Failure f)
    : std::runtime_error("could not " + f.step +
                         (f.result.empty() ? "" : ": " + f.result)),
      m_failure(std::move(f)) {}

std::string VkResultName(int r) {
  static constexpr struct {
    int value;
    const char* name;
  } kNames[] = {
      {0, "VK_SUCCESS"},
      {1, "VK_NOT_READY"},
      {2, "VK_TIMEOUT"},
      {3, "VK_EVENT_SET"},
      {4, "VK_EVENT_RESET"},
      {5, "VK_INCOMPLETE"},
      {-1, "VK_ERROR_OUT_OF_HOST_MEMORY"},
      {-2, "VK_ERROR_OUT_OF_DEVICE_MEMORY"},
      {-3, "VK_ERROR_INITIALIZATION_FAILED"},
      {-4, "VK_ERROR_DEVICE_LOST"},
      {-5, "VK_ERROR_MEMORY_MAP_FAILED"},
      {-6, "VK_ERROR_LAYER_NOT_PRESENT"},
      {-7, "VK_ERROR_EXTENSION_NOT_PRESENT"},
      {-8, "VK_ERROR_FEATURE_NOT_PRESENT"},
      {-9, "VK_ERROR_INCOMPATIBLE_DRIVER"},
      {-10, "VK_ERROR_TOO_MANY_OBJECTS"},
      {-11, "VK_ERROR_FORMAT_NOT_SUPPORTED"},
      {-12, "VK_ERROR_FRAGMENTED_POOL"},
      {-13, "VK_ERROR_UNKNOWN"},
      {-1000069000, "VK_ERROR_OUT_OF_POOL_MEMORY"},
      {-1000072003, "VK_ERROR_INVALID_EXTERNAL_HANDLE"},
      {-1000161000, "VK_ERROR_FRAGMENTATION"},
      {-1000257000, "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS"},
      {-1000000000, "VK_ERROR_SURFACE_LOST_KHR"},
      {-1000000001, "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR"},
      {1000001003, "VK_SUBOPTIMAL_KHR"},
      {-1000001004, "VK_ERROR_OUT_OF_DATE_KHR"},
      {-1000003001, "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR"},
      {-1000011001, "VK_ERROR_VALIDATION_FAILED_EXT"},
  };
  for (const auto& n : kNames)
    if (n.value == r)
      return n.name;
  return "VkResult " + std::to_string(r);
}

std::string FailureText(const Failure& f, const std::string& logPath,
                        const fs::path& failureFile) {
  std::ostringstream o;
  o << "Loböle could not start: it could not " << f.step << ".\n\n";
  if (!f.result.empty())
    o << "Vulkan answered " << f.result << ".\n";
  if (!f.detail.empty())
    o << f.detail << "\n";
  if (!f.result.empty() || !f.detail.empty())
    o << "\n";
  if (!IsDisplayStep(f.step)) {
    if (f.devices.empty()) {
      o << "Graphics devices found: none.\n";
    } else {
      o << "Graphics devices found:\n";
      for (const std::string& d : f.devices)
        o << "  - " << d << "\n";
    }
    if (!f.attempts.empty()) {
      o << "Tried, in order:\n";
      for (const std::string& a : f.attempts)
        o << "  - " << a << "\n";
    }
    o << "\n";
  }
  o << "What to try:\n" << Tips(f) << "\n";
  o << "The log of this start: " << (logPath.empty() ? "(none)" : logPath)
    << "\n";
  o << "This message is kept in: " << failureFile.string() << "\n";
  o << "More help: " << kHelpUrl << "\n";
  return o.str();
}

fs::path FailureFile(const fs::path& stateDir) {
  return stateDir / "last-startup-failure.txt";
}

int ReportStartupFailure(const Failure& f, const fs::path& stateDir,
                         const std::string& logPath, bool windowed) {
  const fs::path file = FailureFile(stateDir);
  const std::string text = FailureText(f, logPath, file);
  std::fputs(text.c_str(), stderr);
  std::fflush(stderr);
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);)
    if (!line.empty())
      LOG_ERROR("startup: {}", line);
  std::error_code ec;
  fs::create_directories(stateDir, ec);
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out)
      LOG_WARN("Could not write {}", file.string());
  }
  if (windowed && DialogsWanted()) {
    const std::string brief = "It could not " + f.step +
                              (f.result.empty() ? "" : " (" + f.result + ")") +
                              ". What to try is in " + file.string();
    if (!ShowError("Loböle could not start", text, brief))
      LOG_WARN("No way to show the startup failure on screen; it is in {}",
               file.string());
  }
  return kExitNoRenderer;
}

bool DialogsWanted() {
  const char* no = std::getenv("LOBOLE_NO_DIALOG");
  if (no != nullptr && std::string_view(no) == "1")
    return false;
#if defined(_WIN32)
  return !_isatty(_fileno(stderr));
#else
  return !isatty(fileno(stderr));
#endif
}

bool ShowError(const std::string& title, const std::string& text,
               const std::string& brief) {
#if defined(_WIN32)
  (void)brief;
  MessageBoxW(nullptr, Wide(text).c_str(), Wide(title).c_str(),
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  return true;
#else
  if (Shown(RunAndWait({"zenity", "--error", "--no-markup", "--title", title,
                        "--text", text})))
    return true;
  if (Shown(RunAndWait({"kdialog", "--title", title, "--error", text})))
    return true;
  return Notify(title, brief);
#endif
}

// ---------------------------------------------------------------------------
// Question

#if defined(_WIN32)
struct Question::Impl {
  // Shared with the box's thread, which outlives this object when the app
  // quits with the box still open.
  std::shared_ptr<std::atomic<int>> answer =
      std::make_shared<std::atomic<int>>(0); // 0 pending, 1 restart, 2 quit
};

Question::Question(const std::string& title, const std::string& text)
    : m_impl(std::make_unique<Impl>()) {
  // Its own thread: MessageBoxW runs a modal loop, and the frame loop (and
  // the remote it pumps) must go on while the box is up. Detached: an app
  // that quits with the box open must not wait for a click.
  std::thread([answer = m_impl->answer, t = Wide(title), x = Wide(text)] {
    const int r = MessageBoxW(nullptr, x.c_str(), t.c_str(),
                              MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND);
    *answer = r == IDYES ? 1 : 2;
  }).detach();
}

Question::~Question() = default;

Question::Answer Question::Poll() {
  switch (m_impl->answer->load()) {
  case 0:
    return Answer::Pending;
  case 1:
    return Answer::Restart;
  default:
    return Answer::Quit;
  }
}
#else
struct Question::Impl {
  std::string title, text;
  pid_t pid = -1;
  int tool = 0; // 0 zenity, 1 kdialog, 2 none left
  bool done = false;
  Answer answer = Answer::Pending;

  void Start() {
    while (tool < 2) {
      if (tool == 0)
        pid = Spawn({"zenity", "--question", "--no-markup", "--title", title,
                     "--text", text, "--ok-label", "Restart", "--cancel-label",
                     "Quit"});
      else
        pid = Spawn({"kdialog", "--title", title, "--yes-label", "Restart",
                     "--no-label", "Quit", "--yesno", text});
      if (pid > 0)
        return;
      ++tool;
    }
    // No box to ask with: say it as a notification, which cannot answer.
    // The app goes on serving; closing its window quits.
    done = true;
    answer = Answer::Unanswerable;
    if (!Notify(title, text))
      LOG_WARN("No way to show the device loss on screen");
  }
};

Question::Question(const std::string& title, const std::string& text)
    : m_impl(std::make_unique<Impl>()) {
  m_impl->title = title;
  m_impl->text = text;
  m_impl->Start();
}

Question::~Question() {
  if (m_impl->pid > 0 && !m_impl->done) {
    kill(m_impl->pid, SIGTERM);
    int status = 0;
    waitpid(m_impl->pid, &status, 0);
  }
}

Question::Answer Question::Poll() {
  Impl& q = *m_impl;
  if (q.done)
    return q.answer;
  int status = 0;
  const pid_t r = waitpid(q.pid, &status, WNOHANG);
  if (r == 0)
    return Answer::Pending;
  const int code = r > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  q.pid = -1;
  if (code == 0) {
    q.done = true;
    q.answer = Answer::Restart;
  } else if (code == 1) {
    q.done = true;
    q.answer = Answer::Quit;
  } else {
    // That tool could not show it (not installed: 127; no display: its own
    // error). The next one, then the notification.
    ++q.tool;
    q.Start();
  }
  return q.done ? q.answer : Answer::Pending;
}
#endif

// ---------------------------------------------------------------------------
// The environment

bool RendererChoiceFromString(std::string_view text, RendererChoice* out,
                              std::string* error) {
  if (text.empty()) {
    *out = RendererChoice::Automatic;
    return true;
  }
  if (text == "software") {
    *out = RendererChoice::Software;
    return true;
  }
  if (text == "hardware") {
    *out = RendererChoice::Hardware;
    return true;
  }
  if (error)
    *error = "LOBOLE_RENDERER is '" + std::string(text) +
             "'; it takes software (a CPU device such as Mesa's lavapipe) or "
             "hardware (never one), and unset chooses";
  return false;
}

bool ZeroOneFromString(std::string_view name, std::string_view text, bool* out,
                       std::string* error) {
  if (text.empty() || text == "0") {
    *out = false;
    return true;
  }
  if (text == "1") {
    *out = true;
    return true;
  }
  if (error)
    *error = std::string(name) + " is '" + std::string(text) +
             "'; it takes 1 (on) or 0 (off)";
  return false;
}

bool FaultFromString(std::string_view text, bool allowed, Fault* out,
                     std::string* error) {
  *out = Fault{};
  if (text.empty())
    return true;
  std::string accepted = "device_create, swapchain, renderer_init:<";
  for (size_t i = 0; i < std::size(kFaultRenderers); i++)
    accepted += (i ? "|" : "") + std::string(kFaultRenderers[i]);
  accepted += ">";
  if (!allowed) {
    if (error)
      *error = "LOBOLE_FAULT is refused: it breaks start-up on purpose, so it "
               "is honoured only with LOBOLE_ALLOW_DEBUG_CRASH=1 in the "
               "environment";
    return false;
  }
  if (text == "device_create") {
    out->kind = Fault::Kind::DeviceCreate;
    return true;
  }
  if (text == "swapchain") {
    out->kind = Fault::Kind::Swapchain;
    return true;
  }
  constexpr std::string_view kInit = "renderer_init:";
  if (text.substr(0, kInit.size()) == kInit) {
    const std::string_view name = text.substr(kInit.size());
    for (const char* r : kFaultRenderers)
      if (name == r) {
        out->kind = Fault::Kind::RendererInit;
        out->renderer = std::string(name);
        return true;
      }
  }
  if (error)
    *error =
        "LOBOLE_FAULT is '" + std::string(text) + "'; it takes " + accepted;
  return false;
}

std::vector<int> DeviceLadder(const std::vector<Device>& devices,
                              const std::string& preferred,
                              RendererChoice choice) {
  std::vector<std::string> names;
  std::vector<int> scores;
  for (const Device& d : devices) {
    const bool wanted = choice == RendererChoice::Automatic ||
                        (choice == RendererChoice::Software) == d.cpu;
    names.push_back(d.name);
    scores.push_back(wanted ? d.score : -1);
  }
  std::vector<int> ladder;
  const int first = devicepref::Choose(names, scores, preferred);
  if (first < 0)
    return ladder;
  ladder.push_back(first);
  std::vector<int> rest;
  for (int i = 0; i < static_cast<int>(devices.size()); i++)
    if (i != first && scores[static_cast<size_t>(i)] >= 0)
      rest.push_back(i);
  std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) {
    const Device& da = devices[static_cast<size_t>(a)];
    const Device& db = devices[static_cast<size_t>(b)];
    if (da.cpu != db.cpu)
      return !da.cpu; // hardware first, software last
    return da.score > db.score;
  });
  ladder.insert(ladder.end(), rest.begin(), rest.end());
  return ladder;
}

// ---------------------------------------------------------------------------
// Device loss

std::string DeviceLossText(const std::string& reason, int autosaved,
                           const fs::path& reportDir) {
  std::ostringstream o;
  o << "The graphics device stopped working (" << reason
    << "): its driver reset it, or the GPU went away. Loböle cannot draw "
       "again without starting over.\n\n";
  if (autosaved > 0)
    o << "Your work is saved for recovery (" << autosaved << " document"
      << (autosaved == 1 ? "" : "s")
      << "): Restart reopens Loböle and offers to recover it.\n\n";
  else
    o << "Nothing was unsaved.\n\n";
  if (!reportDir.empty())
    o << "A report is in " << reportDir.string() << "\n\n";
  o << "Restart now?";
  return o.str();
}

bool Relaunch(const std::vector<std::string>& args, std::string* error) {
  std::vector<std::string> argv = args;
#if defined(_WIN32)
  wchar_t exe[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
  if (n == 0) {
    if (error)
      *error = "cannot name this executable";
    return false;
  }
  // CommandLineToArgvW's rules, backwards: quote every argument, and double
  // the backslashes that end up before a quote.
  const auto quote = [](const std::wstring& a) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
      if (c == L'\\') {
        ++slashes;
      } else {
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
      }
      q.push_back(c);
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
  };
  std::wstring cmd = quote(std::wstring(exe, n));
  for (const std::string& a : argv)
    cmd += L" " + quote(Wide(a));
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &si, &pi)) {
    if (error)
      *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  std::string self;
#if defined(__linux__)
  std::error_code ec;
  self = fs::read_symlink("/proc/self/exe", ec).string();
#endif
  if (self.empty()) {
    if (error)
      *error = "cannot name this executable";
    return false;
  }
  std::vector<char*> cargv;
  cargv.push_back(self.data());
  for (std::string& a : argv)
    cargv.push_back(a.data());
  cargv.push_back(nullptr);
  execv(self.c_str(), cargv.data());
  if (error)
    *error = std::string("execv failed: ") + std::strerror(errno);
  return false;
#endif
}

} // namespace startup
