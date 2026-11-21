// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/ParentWatch.h"

#include <cstdlib>
#include <iostream>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <sys/event.h>
#include <thread>
#endif

namespace {
constexpr const char* kEnv = "LOBOLE_EXIT_WITH_PARENT";

#if defined(__linux__)
bool TerminateWhenParentDies() {
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) == 0)
    return true;
  std::cerr << "--exit-with-parent: prctl(PR_SET_PDEATHSIG) failed: "
            << std::strerror(errno) << std::endl;
  return false;
}
#elif defined(__APPLE__)
bool TerminateWhenParentDies() {
  const int queue = kqueue();
  struct kevent watch;
  EV_SET(&watch, getppid(), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);
  if (queue < 0 ||
      (kevent(queue, &watch, 1, nullptr, 0, nullptr) != 0 && errno != ESRCH)) {
    std::cerr << "--exit-with-parent: kqueue watch failed: "
              << std::strerror(errno) << std::endl;
    return false;
  }
  std::thread([queue] {
    struct kevent fired;
    while (kevent(queue, nullptr, 0, &fired, 1, nullptr) < 0 &&
           errno == EINTR) {
    }
    ::kill(getpid(), SIGTERM);
  }).detach();
  return true;
}
#endif
} // namespace

int ArmExitWithParent(bool requestedByFlag) {
  const char* raw = std::getenv(kEnv);
  const std::string value = raw ? raw : "";
  if (raw) {
    // Read once, then gone: a lobole started from this one's terminal tab
    // would otherwise inherit a launcher pid that is not its parent.
#if defined(_WIN32)
    _putenv_s(kEnv, "");
#else
    unsetenv(kEnv);
#endif
  }
  if (!requestedByFlag && value.empty())
    return -1;

#if !defined(__linux__) && !defined(__APPLE__)
  std::cerr << "--exit-with-parent / " << kEnv
            << " needs prctl(PR_SET_PDEATHSIG) or kqueue, which this platform "
               "does not have"
            << std::endl;
  return 2;
#else
  pid_t launcher = getppid();
  if (!value.empty()) {
    const bool digits =
        value.size() <= 9 &&
        value.find_first_not_of("0123456789") == std::string::npos;
    const long parsed = digits ? std::strtol(value.c_str(), nullptr, 10) : 0;
    if (parsed <= 1) {
      std::cerr << kEnv << " takes the launcher's pid (an integer above 1), "
                << "not '" << value << "'" << std::endl;
      return 2;
    }
    launcher = static_cast<pid_t>(parsed);
  }

  if (!TerminateWhenParentDies())
    return 2;

  // Armed. Now the race: if the launcher died before the line above, the
  // kernel has nothing left to signal us about.
  const pid_t parent = getppid();
  if (parent == launcher)
    return -1;
  if (::kill(launcher, 0) != 0 && errno == ESRCH) {
    std::cerr << "exit-with-parent: launcher " << launcher
              << " exited before startup; exiting" << std::endl;
    return 0;
  }
  // Alive but not our parent: a wrapper (a shell, `flatpak run`) sits in
  // between, and the signal follows THAT process. Say so rather than exit —
  // the instance is still owned, by something that may live longer.
  std::cerr << "exit-with-parent: launcher " << launcher
            << " is not this process's parent (" << parent << "); exiting with "
            << parent << " instead" << std::endl;
  return -1;
#endif
}
