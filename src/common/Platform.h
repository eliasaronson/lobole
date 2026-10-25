// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The few things that differ by operating system and that more than one
// subsystem needs: a lock that dies with its process, showing a folder to the
// user, and naming the OS in a report. Everything else platform-specific
// stays `#ifdef _WIN32` in the one file that needs it.
namespace platform {

// An exclusive lock on an open file, held for as long as this object lives:
// `flock` on Linux and macOS, `LockFileEx` on Windows. A lock is released by
// the kernel when its process dies, however it dies, which is what makes it
// an answer to "is the process that wrote this file still running?" — a pid
// is not, since pids are reused.
//
// The lock belongs to the open file, not the process: a second FileLock on
// the same path in the SAME process is refused too (crash reports rely on
// that to never collect their own session).
class FileLock {
public:
  FileLock() = default;
  ~FileLock() { Release(); }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  // Opens `path` (creating it when `create`) and takes the lock without
  // waiting. False when the file cannot be opened or someone holds it.
  [[nodiscard]] bool TryLock(const std::filesystem::path& path, bool create);
  bool Held() const { return m_handle != kNone; }

  // Through the locked handle: on Windows nobody else may read a locked
  // range, so the holder is the only one who can. Replace truncates first.
  [[nodiscard]] bool Replace(std::string_view content);
  [[nodiscard]] std::optional<std::string> ReadAll() const;

  // Unlocks and closes. The file stays.
  void Release();

private:
  static constexpr intptr_t kNone = -1;
  intptr_t m_handle = kNone; // an fd, or a HANDLE
};

// Shows `path` (a folder) in the desktop's file manager: `xdg-open` on Linux,
// `open` on macOS, the shell on Windows. Returns at once; false with `error`
// when nothing could be started.
bool RevealInFileManager(const std::filesystem::path& path,
                         std::string* error = nullptr);

// "Linux 6.9.3 x86_64", "Windows 10.0.19045 (Wine 9.0)", "Darwin 23.4.0
// arm64": what a crash report says the machine was.
std::string OsDescription();

// This process's id.
int CurrentPid();

// The faults in system code debug_crash makes, on a thread of its own
// (docs/architecture.md#crashes-and-stalls). FaultInSystemCopy copies from an
// unmapped address with the system C library's own memcpy (msvcrt.dll's on
// Windows, libc's elsewhere): the fault is in system code, called from ours,
// and it does not return. ProbeWithSystem asks the system whether an
// unmapped address is unreadable with IsBadReadPtr, which faults and catches
// its own fault, into `unreadable`; Windows only, false with `error` elsewhere.
[[noreturn]] void FaultInSystemCopy();
bool ProbeWithSystem(bool* unreadable, std::string* error);

} // namespace platform
