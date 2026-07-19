#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Document;

// Named checkpoints (versioning F4, pre-git v1): each is a full project-JSON
// copy under `<project>.checkpoints/<name>.json` beside the project file.
// With git (F5) a checkpoint becomes a commit/tag; this layout is the
// stepping stone, and restore is just an open_project on the returned path.
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
