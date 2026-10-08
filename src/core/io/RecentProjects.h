#pragma once

#include <string>
#include <vector>

// The projects this user opened or saved, most recent first.
//
// STATE, not config (docs/ui/settings.md#three-buckets): the app writes it,
// the user never edits it, and losing it costs a convenience rather than a
// choice. It lives beside the autosave in the user state directory
// (`$XDG_STATE_HOME/designer`; `%LOCALAPPDATA%\Lobole\state` on Windows),
// not in the project and not in the config dir.
//
// Deliberately dumb: paths as written, no validation at record time. A
// project on an unplugged drive is still a project you had open, so the list
// keeps it and the caller decides how to present something that is missing
// right now — silently dropping entries would make the list lie about your
// own history.
namespace RecentProjects {

// How many are kept. Ten is the length of a menu nobody has to read.
inline constexpr size_t kMax = 10;

// Records `path` at the front, removing any earlier entry for the same file
// so re-opening reorders rather than duplicates. Empty paths (the unsaved
// slot) are ignored. Failure to write is not an error worth surfacing.
void Remember(const std::string& path);

// Most recent first. Missing files are INCLUDED — see the note above.
std::vector<std::string> List();

// Drops one entry (the menu offers this when a path no longer opens), or all.
void Forget(const std::string& path);
void Clear();

// The file backing the list, for tests and for anyone debugging it.
std::string FilePath();

} // namespace RecentProjects
