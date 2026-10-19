// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A shared project is someone else's input, and the strings in it became
// paths: a document's name was its folder on save and its autosave file, its
// `dir` was opened as given, an asset name was a file under assets/. With
// `..`, or an absolute path (which `dir / name` returns as it is), each of
// them read, wrote or pruned files anywhere the user could. These are the
// rules every such string goes through
// (docs/versioning/format.md#paths-in-a-project).

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace projectpaths {

// One file or folder name that stays where it is joined: no separator, no
// drive colon, not `.` or `..`, no control character, not empty.
inline bool IsPlainComponent(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\:") == std::string_view::npos &&
         std::none_of(name.begin(), name.end(),
                      [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// The folder a document is saved in. Its name when that is a plain component,
// as nearly every name is; otherwise one made from its id, so a name typed,
// scripted or read from a STEP file ("Bracket 1/2", "../x") is still a name
// and never a path.
inline std::string DocFolder(const std::string& name, uint64_t id) {
  return IsPlainComponent(name) ? name : "document-" + std::to_string(id);
}

// `rel`, a path a project file names, resolved under `root` — or the reason it
// is not under it.
inline std::expected<std::filesystem::path, std::string>
InsideProject(const std::filesystem::path& root, const std::string& rel) {
  namespace fs = std::filesystem;
  const fs::path relPath(rel);
  if (rel.empty() || relPath.is_absolute() || relPath.has_root_name())
    return std::unexpected("'" + rel + "' is not a path inside the project");
  for (const fs::path& part : relPath)
    if (part == "..")
      return std::unexpected("'" + rel + "' leaves the project folder");
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(root, ec);
  const fs::path full = fs::weakly_canonical(root / relPath, ec);
  if (ec)
    return std::unexpected("'" + rel + "': " + ec.message());
  // A symlink inside the project can still point out of it.
  const auto [b, f] =
      std::mismatch(base.begin(), base.end(), full.begin(), full.end());
  if (b != base.end())
    return std::unexpected("'" + rel + "' leaves the project folder");
  return root / relPath;
}

} // namespace projectpaths
