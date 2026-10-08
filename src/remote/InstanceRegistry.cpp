#include "remote/InstanceRegistry.h"

#include "common/Logger.h"
#include "core/io/Autosaver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace remote {

int CurrentPid() {
#ifndef _WIN32
  return static_cast<int>(::getpid());
#else
  return static_cast<int>(::GetCurrentProcessId());
#endif
}

namespace {

// Is that process still there? The only honest liveness test for a file left
// behind by a crash. A pid we cannot signal (another user's) counts as alive:
// refusing to attach is better than deleting someone else's announcement.
bool ProcessAlive(int pid) {
  if (pid <= 0)
    return false;
#ifndef _WIN32
  if (::kill(pid, 0) == 0)
    return true;
  return errno == EPERM;
#else
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
  if (h == nullptr)
    return false;
  DWORD code = 0;
  const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  ::CloseHandle(h);
  return alive;
#endif
}

fs::path FileFor(uint16_t port) {
  return fs::path(InstancesDir()) / (std::to_string(port) + ".json");
}

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

std::string InstancesDir() {
  return (fs::path(Autosaver::StateDir()) / "instances").string();
}

bool AnnounceInstance(uint16_t port, const std::string& mode) {
  try {
    fs::create_directories(InstancesDir());
    // Sweep first: this process may be reusing a port whose previous owner
    // crashed, and a stale file for a dead pid helps nobody.
    LiveInstances();

    const json j{{"app", "lobole"},
                 {"pid", CurrentPid()},
                 {"port", port},
                 {"mode", mode},
                 {"started", NowSeconds()}};
    const fs::path target = FileFor(port);
    const fs::path tmp = target.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << j.dump(2) << "\n";
      if (!out)
        return false;
    }
    fs::rename(tmp, target);
    return true;
  } catch (const std::exception& e) {
    // Discovery is a convenience; losing it must not cost the remote
    // interface, which is the actual product.
    LOG_INFO("Could not announce this instance ({}): {}", port, e.what());
    return false;
  }
}

void WithdrawInstance(uint16_t port) {
  std::error_code ec;
  fs::remove(FileFor(port), ec);
}

std::vector<InstanceInfo> LiveInstances() {
  std::vector<InstanceInfo> live;
  std::error_code ec;
  const fs::path dir(InstancesDir());
  if (!fs::exists(dir, ec))
    return live;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".json")
      continue;
    InstanceInfo info;
    try {
      std::ifstream in(entry.path());
      const json j = json::parse(in);
      info.pid = j.value("pid", 0);
      info.port = static_cast<uint16_t>(j.value("port", 0));
      info.mode = j.value("mode", std::string("headless"));
      info.started = j.value("started", int64_t{0});
    } catch (const std::exception&) {
      fs::remove(entry.path(), ec); // unparseable: it announces nothing
      continue;
    }
    if (info.port == 0 || !ProcessAlive(info.pid)) {
      fs::remove(entry.path(), ec);
      continue;
    }
    live.push_back(info);
  }
  std::sort(live.begin(), live.end(),
            [](const InstanceInfo& a, const InstanceInfo& b) {
              return a.started > b.started; // newest first
            });
  return live;
}

} // namespace remote
