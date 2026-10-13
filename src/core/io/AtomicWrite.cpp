// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Serialization.h"

#include <fstream>
#include <iterator>

// Split out of Serialization.cpp so it is reachable: it lived in that file's
// anonymous namespace, and the settings store (core/io/Settings.cpp) needs
// the same guarantee — an interrupted write must never leave a truncated
// file that silently resets someone's preferences.
namespace DocumentIO {

bool AtomicTargetOk(const std::filesystem::path& target, std::string* error) {
  // An empty name wrote "<cwd>/.tmp" and then failed to rename it; a
  // directory was read as the file it would replace ("basic_filebuf::
  // underflow error reading the file: Is a directory"). Both refused before
  // anything is created (hostile_params_smoke).
  std::error_code ec;
  std::string why;
  if (target.empty() || !target.has_filename())
    why = "'" + target.string() + "' names no file";
  else if (std::filesystem::is_directory(target, ec))
    why = "'" + target.string() + "' is a directory, not a file";
  else if (const size_t n = target.filename().string().size(); n > 255)
    why = "its file name is " + std::to_string(n) +
          " bytes, and a file system takes at most 255";
  else if (const auto dir = target.parent_path();
           !dir.empty() && !std::filesystem::is_directory(dir, ec))
    why = "the directory '" + dir.string() + "' does not exist";
  if (why.empty())
    return true;
  if (error)
    *error = "cannot write " + target.string() + ": " + why;
  return false;
}

bool WriteTextAtomic(const std::filesystem::path& target,
                     const std::string& text, std::string* error) {
  if (!AtomicTargetOk(target, error))
    return false;
  // Skip identical content: an untouched feature keeps its mtime, so git
  // status, rsync, and build tools see exactly the files that changed.
  {
    std::ifstream existing(target, std::ios::binary);
    if (existing) {
      std::string current((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
      if (current == text)
        return true;
    }
  }
  const std::filesystem::path tmp = target.string() + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + tmp.string();
      return false;
    }
    out << text;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    if (error)
      *error = "rename failed: " + ec.message();
    std::filesystem::remove(tmp, ec); // never leave the half behind
    return false;
  }
  return true;
}

} // namespace DocumentIO
