// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Document;

// Named checkpoints (versioning F4) — the NO-GIT FALLBACK since F6. Each is
// a copy of ONE document under `<project>.checkpoints/<name>.json`: the
// active part, not the project, so on a multi-part project it captures
// neither the assembly nor the other tabs. Where git is available a version
// is a commit + tag of the whole project (ui/HistoryPanel.h), which is what
// the History panel and the checkpoint_* RPCs use; this remains only so a
// machine without git still gets something.
namespace Checkpoints {

struct Info {
  std::string name;
  int64_t modifiedUnix = 0; // file mtime, seconds since epoch
};

// `<projectPath>.checkpoints` — one directory per project, sibling to it.
std::string Dir(const std::string& projectPath);

// Path of one checkpoint file (empty when the name is invalid: names must be
// non-empty and free of path separators).
std::string PathOf(const std::string& projectPath, const std::string& name);

// Snapshot the document. Fails (false + error) on an invalid name or when
// projectPath is empty — checkpoints need a saved project as their home.
bool Create(const Document& doc, const std::string& projectPath,
            const std::string& name, std::string* error = nullptr);

// All checkpoints of a project, newest first.
std::vector<Info> List(const std::string& projectPath);

bool Remove(const std::string& projectPath, const std::string& name);

} // namespace Checkpoints
