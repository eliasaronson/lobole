// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/Platform.h"

#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <shellapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace platform {

FileLock::FileLock(FileLock&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kNone)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    Release();
    m_handle = std::exchange(other.m_handle, kNone);
  }
  return *this;
}

#if defined(_WIN32)

bool FileLock::TryLock(const std::filesystem::path& path, bool create) {
  Release();
  // Every share flag: another process must be able to open the file to TRY
  // the lock, and the holder must be able to rename and delete it while it
  // is open.
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  OVERLAPPED at{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                  MAXDWORD, MAXDWORD, &at)) {
    CloseHandle(h);
    return false;
  }
  m_handle = reinterpret_cast<intptr_t>(h);
  return true;
}

bool FileLock::Replace(std::string_view content) {
  if (!Held())
    return false;
  HANDLE h = reinterpret_cast<HANDLE>(m_handle);
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
    return false;
  DWORD written = 0;
  return WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                   &written, nullptr) &&
         written == content.size();
}

std::optional<std::string> FileLock::ReadAll() const {
  if (!Held())
    return std::nullopt;
  HANDLE h = reinterpret_cast<HANDLE>(m_handle);
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN))
    return std::nullopt;
  std::string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
    out.append(buf, got);
  return out;
}

void FileLock::Release() {
  if (!Held())
    return;
  HANDLE h = reinterpret_cast<HANDLE>(m_handle);
  OVERLAPPED at{};
  UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
  CloseHandle(h);
  m_handle = kNone;
}

bool RevealInFileManager(const std::filesystem::path& path,
                         std::string* error) {
  const auto result = reinterpret_cast<intptr_t>(
      ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL));
  if (result > 32)
    return true;
  if (error)
    *error = "the shell could not open " + path.string();
  return false;
}

std::string OsDescription() {
  std::string out = "Windows";
  // RtlGetVersion tells the truth; GetVersionEx answers what the manifest
  // says it may.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll) {
    if (auto get = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
      OSVERSIONINFOW v{};
      v.dwOSVersionInfoSize = sizeof v;
      if (get(&v) == 0)
        out += " " + std::to_string(v.dwMajorVersion) + "." +
               std::to_string(v.dwMinorVersion) + "." +
               std::to_string(v.dwBuildNumber);
    }
    // Wine says so, which decides whether a crash is ours or the emulator's.
    using WineVersionFn = const char*(CDECL*)();
    if (auto wine = reinterpret_cast<WineVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version"))))
      out += std::string(" (Wine ") + wine() + ")";
  }
  return out;
}

int CurrentPid() { return static_cast<int>(GetCurrentProcessId()); }

#else // POSIX

bool FileLock::TryLock(const std::filesystem::path& path, bool create) {
  Release();
  // CLOEXEC: a child we spawn (ffmpeg, git) must not inherit the lock and
  // keep a dead session looking alive.
  const int fd =
      ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0644);
  if (fd < 0)
    return false;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  m_handle = fd;
  return true;
}

bool FileLock::Replace(std::string_view content) {
  if (!Held())
    return false;
  const int fd = static_cast<int>(m_handle);
  if (::ftruncate(fd, 0) != 0)
    return false;
  size_t done = 0;
  while (done < content.size()) {
    const ssize_t n = ::pwrite(fd, content.data() + done, content.size() - done,
                               static_cast<off_t>(done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

std::optional<std::string> FileLock::ReadAll() const {
  if (!Held())
    return std::nullopt;
  const int fd = static_cast<int>(m_handle);
  std::string out;
  char buf[4096];
  off_t at = 0;
  for (;;) {
    const ssize_t n = ::pread(fd, buf, sizeof buf, at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    out.append(buf, static_cast<size_t>(n));
    at += n;
  }
  return out;
}

void FileLock::Release() {
  if (!Held())
    return;
  const int fd = static_cast<int>(m_handle);
  ::flock(fd, LOCK_UN);
  ::close(fd);
  m_handle = kNone;
}

bool RevealInFileManager(const std::filesystem::path& path,
                         std::string* error) {
#if defined(__APPLE__)
  const char* program = "open";
#else
  const char* program = "xdg-open";
#endif
  std::vector<std::string> args = {program, path.string()};
  std::vector<char*> argv;
  for (std::string& a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (posix_spawnp(&pid, program, nullptr, nullptr, argv.data(), environ) !=
      0) {
    if (error)
      *error = std::string("could not start ") + program;
    return false;
  }
  // Reaped off the frame: xdg-open returns once the file manager is asked,
  // and a child nobody waits for is a zombie until the app exits.
  std::thread([pid] {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
  return true;
}

std::string OsDescription() {
  utsname u{};
  if (::uname(&u) != 0)
    return "unknown";
  return std::string(u.sysname) + " " + u.release + " " + u.machine;
}

int CurrentPid() { return static_cast<int>(::getpid()); }

#endif

} // namespace platform
