#include "core/io/Serialization.h"

#include <fstream>
#include <iterator>

// Split out of Serialization.cpp so it is reachable: it lived in that file's
// anonymous namespace, and the settings store (core/io/Settings.cpp) needs
// the same guarantee — an interrupted write must never leave a truncated
// file that silently resets someone's preferences.
namespace DocumentIO {

bool WriteTextAtomic(const std::filesystem::path& target,
                     const std::string& text, std::string* error) {
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
    return false;
  }
  return true;
}

} // namespace DocumentIO
