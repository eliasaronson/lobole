#include "core/io/ProjectMerge.h"

#include "core/io/Serialization.h"

#include "common/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ProjectMerge {

namespace {

// The id-keyed collections of the directory format: the sub-directory a file
// lives in, the key its ordered id list has in index.json, and what to call
// one of them to a person. Adding a collection to the format means adding a
// row here — the same discipline the `describe` RPC table uses.
struct Collection {
  const char* dir;
  const char* indexKey;
  const char* label;
};
constexpr Collection kCollections[] = {
    {"features", "features", "feature"},
    {"studies", "studies", "study"},
    {"configurations", "configurations", "configuration"},
    {"occurrences", "occurrences", "occurrence"},
    {"mates", "mates", "mate"},
    {"views", "views", "view"},
    {"dims", "dims", "dimension"},
};

const Collection* CollectionForDir(const std::string& dir) {
  for (const Collection& c : kCollections)
    if (dir == c.dir)
      return &c;
  return nullptr;
}

// "parts" -> "part". The top-level folder a document lives in says its kind,
// which is what the tab bar shows.
std::string DocumentKindFromFolder(const std::string& folder) {
  if (folder == "parts")
    return "part";
  if (folder == "assemblies")
    return "assembly";
  if (folder == "drawings")
    return "drawing";
  return "";
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < path.size()) {
    const size_t slash = path.find('/', pos);
    parts.push_back(
        path.substr(pos, slash == std::string::npos ? slash : slash - pos));
    if (slash == std::string::npos)
      break;
    pos = slash + 1;
  }
  return parts;
}

// The id a collection file's name starts with, or 0 when it does not.
uint64_t IdFromFilename(const std::string& filename) {
  size_t end = 0;
  while (end < filename.size() && filename[end] >= '0' && filename[end] <= '9')
    ++end;
  if (end == 0 || end == filename.size() || filename[end] != '-')
    return 0;
  try {
    return std::stoull(filename.substr(0, end));
  } catch (const std::exception&) {
    return 0;
  }
}

json ParseOrNull(const std::string& text) {
  try {
    return json::parse(text);
  } catch (const std::exception&) {
    return json();
  }
}

// A value as a person would read it. Numbers lose their trailing zeros
// (12.000000 is noise), strings lose their quotes, and anything structured
// falls back to compact JSON — it is a label, not a round-trip.
std::string Describe(const json& value) {
  if (value.is_null())
    return "—";
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_boolean())
    return value.get<bool>() ? "yes" : "no";
  if (value.is_number_float()) {
    std::string s = std::to_string(value.get<double>());
    while (s.size() > 1 && s.back() == '0')
      s.pop_back();
    if (!s.empty() && s.back() == '.')
      s.pop_back();
    return s;
  }
  if (value.is_number())
    return std::to_string(value.get<int64_t>());
  return value.dump();
}

// A field name as a label: "sketch_id" -> "sketch id".
std::string FieldLabel(const std::string& field) {
  std::string out = field;
  std::replace(out.begin(), out.end(), '_', ' ');
  return out;
}

std::string ItemName(const json& item) {
  if (item.is_object()) {
    for (const char* key : {"name", "label", "title"}) {
      auto it = item.find(key);
      if (it != item.end() && it->is_string())
        return it->get<std::string>();
    }
  }
  return "";
}

// The feature's own type ("Extrude") beats the generic collection label
// ("feature") — it is what the feature tree shows.
std::string ItemKind(const json& item, const Collection* collection) {
  if (item.is_object()) {
    auto it = item.find("type");
    if (it != item.end() && it->is_string())
      return it->get<std::string>();
  }
  return collection ? collection->label : "item";
}

// Every key where the two sides differ. Fields both sides changed the same
// way are not conflicts — git would have merged those itself.
std::vector<FieldDiff> DiffFields(const json& base, const json& ours,
                                  const json& theirs) {
  std::vector<FieldDiff> out;
  if (!ours.is_object() || !theirs.is_object())
    return out;
  std::set<std::string> keys;
  for (auto it = ours.begin(); it != ours.end(); ++it)
    keys.insert(it.key());
  for (auto it = theirs.begin(); it != theirs.end(); ++it)
    keys.insert(it.key());
  for (const std::string& key : keys) {
    const json o = ours.contains(key) ? ours[key] : json();
    const json t = theirs.contains(key) ? theirs[key] : json();
    if (o == t)
      continue;
    FieldDiff diff;
    diff.field = key;
    diff.ours = Describe(o);
    diff.theirs = Describe(t);
    diff.inBase = base.is_object() && base.contains(key);
    if (diff.inBase)
      diff.base = Describe(base[key]);
    out.push_back(diff);
  }
  return out;
}

// "Extrude 'Gusset 3' in part Stand — distance 12 vs 18"
std::string Summarize(const Conflict& c) {
  std::string s = c.itemKind;
  if (!c.name.empty())
    s += " \"" + c.name + "\"";
  else if (c.id != 0)
    s += " " + std::to_string(c.id);
  if (!c.document.empty())
    s += " in " + (c.documentKind.empty() ? "" : c.documentKind + " ") +
         c.document;
  if (c.oursDeleted)
    return s + " — they changed it, you deleted it";
  if (c.theirsDeleted)
    return s + " — you changed it, they deleted it";
  if (!c.fields.empty()) {
    s += " — ";
    for (size_t i = 0; i < c.fields.size() && i < 3; ++i) {
      if (i)
        s += ", ";
      s += FieldLabel(c.fields[i].field) + " " + c.fields[i].ours + " vs " +
           c.fields[i].theirs;
    }
    if (c.fields.size() > 3)
      s += ", and " + std::to_string(c.fields.size() - 3) + " more";
  }
  return s;
}

// Fill in document/kind/collection from a repo-relative path. Handles both
// shapes the format has: a multi-document project (parts/Stand/features/…)
// and a lone document saved as its own directory (features/…).
const Collection* Locate(const std::vector<std::string>& parts, Conflict& out) {
  if (parts.size() >= 4) {
    out.documentKind = DocumentKindFromFolder(parts[0]);
    if (!out.documentKind.empty()) {
      out.document = parts[1];
      return CollectionForDir(parts[2]);
    }
  }
  if (parts.size() >= 2)
    return CollectionForDir(parts[0]); // a lone document at the root
  return nullptr;
}

// Newline-split, dropping blanks — the same shape GitVault's Lines has, but
// that one is private to it.
std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    const std::string line =
        text.substr(pos, eol == std::string::npos ? eol : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    if (!line.empty())
      out.push_back(line);
  }
  return out;
}

bool ReadFileInto(const fs::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  *out = std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  return true;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return "";
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// Two files claiming one id — the class git merges cleanly and therefore
// never reports. Found by walking the tree, so it is caught whether the
// project arrived by merge, by rsync or in a zip.
void ScanDuplicateIds(const std::string& projectDir,
                      std::vector<Conflict>& out) {
  std::error_code ec;
  // Every directory that is one of the id-keyed collections, at either
  // nesting depth.
  std::vector<fs::path> collectionDirs;
  for (auto it = fs::recursive_directory_iterator(
           projectDir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_directory(ec))
      continue;
    const std::string name = it->path().filename().string();
    if (name == ".git") {
      it.disable_recursion_pending();
      continue;
    }
    if (CollectionForDir(name))
      collectionDirs.push_back(it->path());
  }

  for (const fs::path& dir : collectionDirs) {
    const Collection* collection = CollectionForDir(dir.filename().string());
    std::map<uint64_t, fs::path> byId;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec)
        break;
      if (entry.path().extension() != ".json")
        continue;
      const std::string filename = entry.path().filename().string();
      // A study's ".result.json" sits beside its study and shares the id on
      // purpose — derived data, not a second study.
      if (filename.find(".result.json") != std::string::npos)
        continue;
      const uint64_t id = IdFromFilename(filename);
      if (id == 0)
        continue;
      const auto [it, inserted] = byId.emplace(id, entry.path());
      if (inserted)
        continue;

      Conflict c;
      c.kind = ConflictKind::DuplicateId;
      c.id = id;
      const fs::path rel = fs::relative(entry.path(), projectDir, ec);
      const fs::path relOther = fs::relative(it->second, projectDir, ec);
      c.path = rel.generic_string();
      c.otherPath = relOther.generic_string();
      const auto parts = SplitPath(c.path);
      Locate(parts, c);
      const json a = ParseOrNull(ReadFile(it->second));
      const json b = ParseOrNull(ReadFile(entry.path()));
      c.itemKind = collection ? collection->label : "item";
      const std::string nameA = ItemName(a);
      const std::string nameB = ItemName(b);
      c.name = nameA;
      c.summary = "Two " +
                  std::string(collection ? collection->label : "item") +
                  "s claim id " + std::to_string(id);
      if (!c.document.empty())
        c.summary += " in " +
                     (c.documentKind.empty() ? "" : c.documentKind + " ") +
                     c.document;
      if (!nameA.empty() || !nameB.empty())
        c.summary += ": \"" + nameA + "\" and \"" + nameB + "\"";
      c.summary += " — git merged both without a word";
      out.push_back(std::move(c));
    }
  }
}

} // namespace

int Report::NeedsDecision() const {
  int n = 0;
  for (const Conflict& c : conflicts)
    if (!c.autoResolvable)
      ++n;
  return n;
}

Report Scan(const std::string& projectDir) {
  Report report;
  report.inMerge = GitVault::InMerge(projectDir);

  for (const std::string& path : GitVault::Unmerged(projectDir)) {
    Conflict c;
    c.path = path;
    const auto parts = SplitPath(path);
    const std::string filename = parts.empty() ? path : parts.back();

    GitVault::Stages stages;
    GitVault::ReadStages(projectDir, path, &stages);
    c.oursDeleted = !stages.hasOurs;
    c.theirsDeleted = !stages.hasTheirs;

    if (path == "project.json") {
      c.kind = ConflictKind::Project;
      c.itemKind = "Project";
      c.summary = "Which documents the project has";
      report.conflicts.push_back(std::move(c));
      continue;
    }
    if (filename.ends_with(".brep") || parts.front() == "geometry" ||
        (parts.size() > 1 && parts[parts.size() - 2] == "geometry")) {
      c.kind = ConflictKind::Derived;
      c.autoResolvable = true;
      c.itemKind = "Built geometry";
      c.summary = "Built geometry — rebuilt from the recipe, either side does";
      report.conflicts.push_back(std::move(c));
      continue;
    }
    if (filename == "index.json") {
      c.kind = ConflictKind::Order;
      c.autoResolvable = true;
      if (parts.size() >= 3) {
        c.documentKind = DocumentKindFromFolder(parts[0]);
        c.document = parts[1];
      }
      c.itemKind = "Order";
      c.summary = "The order of things";
      if (!c.document.empty())
        c.summary += " in " +
                     (c.documentKind.empty() ? "" : c.documentKind + " ") +
                     c.document;
      c.summary += " — merged from both sides, nothing dropped";
      report.conflicts.push_back(std::move(c));
      continue;
    }

    const Collection* collection = Locate(parts, c);
    if (!collection) {
      c.kind = ConflictKind::Other;
      c.itemKind = "File";
      c.name = filename;
      c.summary = path;
      report.conflicts.push_back(std::move(c));
      continue;
    }

    c.kind = ConflictKind::Item;
    c.id = IdFromFilename(filename);
    const json base = ParseOrNull(stages.base);
    const json ours = ParseOrNull(stages.ours);
    const json theirs = ParseOrNull(stages.theirs);
    const json& present = ours.is_object() ? ours : theirs;
    c.itemKind = ItemKind(present, collection);
    c.name = ItemName(present);
    c.fields = DiffFields(base, ours, theirs);
    c.summary = Summarize(c);
    report.conflicts.push_back(std::move(c));
  }

  ScanDuplicateIds(projectDir, report.conflicts);
  return report;
}

// ---------------------------------------------------------------------------
// Describing a change (F11 G1)
// ---------------------------------------------------------------------------

namespace {

// "part Stand", "the project", "assembly Dock".
std::string DocumentLabel(const std::vector<std::string>& parts) {
  if (parts.size() < 2)
    return "";
  const std::string kind = DocumentKindFromFolder(parts[0]);
  if (kind.empty())
    return "";
  return kind + " " + parts[1];
}

std::string Plural(size_t n, const std::string& one) {
  return std::to_string(n) + " " + one + (n == 1 ? "" : "s");
}

} // namespace

bool IsSingleItem(const std::vector<std::string>& paths) {
  if (paths.size() != 1)
    return false;
  const auto parts = SplitPath(paths[0]);
  if (parts.size() < 2)
    return false;
  const std::string filename = parts.back();
  if (filename == "index.json" || filename == "project.json")
    return false;
  // The collection sits directly above the file, at either nesting depth.
  return CollectionForDir(parts[parts.size() - 2]) != nullptr;
}

std::string SummarizePaths(const std::vector<std::string>& paths,
                           const std::string& singleItemKind,
                           const std::string& singleItemName) {
  // Derived data and the repository hints are not changes anyone made.
  std::vector<std::string> real;
  for (const std::string& path : paths) {
    const auto parts = SplitPath(path);
    const std::string filename = parts.empty() ? path : parts.back();
    if (filename == ".gitattributes" || filename == ".gitignore")
      continue;
    bool derived = false;
    for (const std::string& part : parts)
      derived = derived || part == "geometry";
    if (!derived)
      real.push_back(path);
  }
  if (real.empty())
    return "";

  // The case worth naming precisely: one item, and the caller read it.
  if (IsSingleItem(real) && !singleItemKind.empty()) {
    const auto parts = SplitPath(real[0]);
    std::string s = singleItemKind;
    if (!singleItemName.empty())
      s += " \"" + singleItemName + "\"";
    const std::string doc = DocumentLabel(parts);
    return doc.empty() ? s : s + " in " + doc;
  }

  // Otherwise count by collection, per document. Keyed by (rank, label) so
  // documents come out in the order the tab bar shows them — parts, then
  // assemblies, then drawings — rather than alphabetically, which would put
  // a drawing before the part it draws.
  const auto rank = [](const std::vector<std::string>& parts) {
    if (parts.size() < 2)
      return 3;
    if (parts[0] == "parts")
      return 0;
    if (parts[0] == "assemblies")
      return 1;
    if (parts[0] == "drawings")
      return 2;
    return 3;
  };
  std::map<std::pair<int, std::string>, std::map<std::string, size_t>>
      byDocument;
  size_t projectLevel = 0;
  for (const std::string& path : real) {
    const auto parts = SplitPath(path);
    const std::string filename = parts.back();
    if (parts.size() == 1) {
      projectLevel++; // project.json and friends
      continue;
    }
    const std::pair<int, std::string> key{rank(parts), DocumentLabel(parts)};
    if (filename == "index.json") {
      byDocument[key]["order"]++;
      continue;
    }
    const Collection* collection = CollectionForDir(parts[parts.size() - 2]);
    byDocument[key][collection ? collection->label : "file"]++;
  }

  std::vector<std::string> pieces;
  for (const auto& [key, counts] : byDocument) {
    const std::string& doc = key.second;
    std::vector<std::string> bits;
    for (const auto& [label, n] : counts) {
      if (label == "order")
        bits.push_back("the order");
      else
        bits.push_back(Plural(n, label));
    }
    std::string s;
    for (size_t i = 0; i < bits.size(); i++)
      s += (i ? ", " : "") + bits[i];
    if (!doc.empty())
      s += " in " + doc;
    pieces.push_back(s);
  }
  if (projectLevel > 0 && pieces.empty())
    return "the project";

  // Two documents read fine; more is a wall, so say how many.
  if (pieces.size() > 2)
    return Plural(pieces.size(), "document") + " changed";
  std::string out;
  for (size_t i = 0; i < pieces.size(); i++)
    out += (i ? " and " : "") + pieces[i];
  if (projectLevel > 0)
    out += out.empty() ? "the project" : ", and the project";
  return out;
}

// ---------------------------------------------------------------------------
// Diffing two versions (F11 G2)
// ---------------------------------------------------------------------------

std::vector<ItemChange> DiffVersions(const std::string& projectDir,
                                     const std::string& from,
                                     const std::string& to) {
  std::vector<ItemChange> out;
  // `to` empty = the working tree, which is what "since that version" means.
  const std::string range = to.empty() ? from : from + " " + to;
  std::string text;
  if (!GitVault::DiffNames(projectDir, from, to, &text))
    return out;

  for (const std::string& line : SplitLines(text)) {
    // "<status>\t<path>" — M, A, D, and R<score> for a rename.
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0)
      continue;
    const char status = line[0];
    const std::string path = line.substr(tab + 1);
    const auto parts = SplitPath(path);
    const std::string filename = parts.empty() ? path : parts.back();

    // Derived data and repository hints are not changes anyone made — the
    // same filter the log summary applies.
    bool derived = filename == ".gitattributes" || filename == ".gitignore";
    for (const std::string& part : parts)
      derived = derived || part == "geometry";
    if (derived)
      continue;

    ItemChange change;
    change.path = path;
    change.id = IdFromFilename(filename);
    change.how = status == 'A'   ? ItemChange::How::Added
                 : status == 'D' ? ItemChange::How::Removed
                                 : ItemChange::How::Changed;

    Conflict located; // reuse the path -> document/collection translation
    located.path = path;
    const Collection* collection = Locate(parts, located);
    change.document = located.document;
    change.documentKind = located.documentKind;

    // The order file is a real change, and it is not an item.
    if (filename == "index.json") {
      change.itemKind = "Order";
      change.summary = "the order";
    } else if (path == "project.json") {
      // Which documents the project has — and which tab was open, since the
      // active one is stored here too. Named rather than shown as a
      // filename, the same as the log summary does.
      change.itemKind = "Project";
      change.summary = "the project's documents";
    } else if (!collection) {
      change.itemKind = "File";
      change.name = filename;
      change.summary = path;
    } else {
      // Read whichever side exists to name it, and field-diff when both do.
      // DiffFields is the Conflicts panel's, unchanged: two blobs are two
      // blobs, no merge required.
      std::string beforeText, afterText;
      const bool haveBefore =
          change.how != ItemChange::How::Added &&
          GitVault::ShowFile(projectDir, from, path, &beforeText);
      const bool haveAfter =
          change.how != ItemChange::How::Removed &&
          (to.empty() ? ReadFileInto(fs::path(projectDir) / path, &afterText)
                      : GitVault::ShowFile(projectDir, to, path, &afterText));
      const json beforeJson = ParseOrNull(beforeText);
      const json afterJson = ParseOrNull(afterText);
      const json& present = afterJson.is_object() ? afterJson : beforeJson;
      change.itemKind = ItemKind(present, collection);
      change.name = ItemName(present);
      if (haveBefore && haveAfter)
        change.fields = DiffFields(json(), beforeJson, afterJson);

      std::string s = change.itemKind;
      if (!change.name.empty())
        s += " \"" + change.name + "\"";
      if (change.how == ItemChange::How::Added)
        s += " — added";
      else if (change.how == ItemChange::How::Removed)
        s += " — removed";
      else if (!change.fields.empty()) {
        s += " — ";
        for (size_t i = 0; i < change.fields.size() && i < 3; i++) {
          if (i)
            s += ", ";
          // `ours` is the FROM side and `theirs` the TO side here; the merge
          // vocabulary is wrong for a diff, so the sentence says it plainly.
          s += FieldLabel(change.fields[i].field) + " " +
               change.fields[i].ours + " to " + change.fields[i].theirs;
        }
        if (change.fields.size() > 3)
          s += ", and " + std::to_string(change.fields.size() - 3) + " more";
      }
      change.summary = s;
    }
    if (!change.document.empty()) {
      change.summary +=
          " in " +
          (change.documentKind.empty() ? "" : change.documentKind + " ") +
          change.document;
    }
    out.push_back(std::move(change));
  }
  (void)range;
  return out;
}

// ---------------------------------------------------------------------------
// The index union merge
// ---------------------------------------------------------------------------

namespace {

std::vector<uint64_t> IdList(const json& doc, const std::string& key) {
  std::vector<uint64_t> out;
  if (!doc.is_object() || !doc.contains(key) || !doc[key].is_array())
    return out;
  for (const auto& value : doc[key])
    if (value.is_number_unsigned() || value.is_number_integer())
      out.push_back(value.get<uint64_t>());
  return out;
}

bool IsIdArray(const json& value) {
  if (!value.is_array())
    return false;
  for (const auto& element : value)
    if (!element.is_number())
      return false;
  return true;
}

// Three-way merge of one ordered id list.
//
// The rule that matters: an id MISSING from a side is only a deletion when
// that side had it to begin with. Otherwise it is simply the other side's
// addition, and dropping it is how "pick mine" loses the other person's
// features while their files sit on disk.
std::vector<uint64_t> MergeIds(const std::vector<uint64_t>& base,
                               const std::vector<uint64_t>& ours,
                               const std::vector<uint64_t>& theirs) {
  const std::set<uint64_t> inBase(base.begin(), base.end());
  const std::set<uint64_t> inOurs(ours.begin(), ours.end());
  const std::set<uint64_t> inTheirs(theirs.begin(), theirs.end());

  const auto kept = [&](uint64_t id) {
    const bool deletedByUs = inBase.count(id) && !inOurs.count(id);
    const bool deletedByThem = inBase.count(id) && !inTheirs.count(id);
    return !deletedByUs && !deletedByThem;
  };

  // Our order is the spine; their additions are woven in after whichever
  // entry preceded them on their side, so a feature they inserted between
  // two of ours lands between those two here as well. Order is not cosmetic
  // in a feature list — an extrude regenerates after the sketch it uses.
  std::vector<uint64_t> result;
  for (uint64_t id : ours)
    if (kept(id))
      result.push_back(id);

  for (size_t i = 0; i < theirs.size(); ++i) {
    const uint64_t id = theirs[i];
    if (!kept(id) || inOurs.count(id))
      continue;
    size_t insertAt = 0;
    for (size_t j = i; j-- > 0;) {
      const auto found = std::find(result.begin(), result.end(), theirs[j]);
      if (found != result.end()) {
        insertAt = static_cast<size_t>(found - result.begin()) + 1;
        break;
      }
    }
    // Then step over OUR additions that already sit after that anchor. Both
    // sides appended after the same entry, and our spine keeps its order:
    // without this, their addition lands in front of ours (base [1,2], ours
    // [1,2,10], theirs [1,2,20] came out [1,2,20,10]).
    while (insertAt < result.size() && !inTheirs.count(result[insertAt]))
      ++insertAt;
    result.insert(result.begin() + static_cast<long>(insertAt), id);
  }
  return result;
}

} // namespace

std::string UnionIndex(const std::string& base, const std::string& ours,
                       const std::string& theirs) {
  const json baseJson = ParseOrNull(base);
  const json oursJson = ParseOrNull(ours);
  const json theirsJson = ParseOrNull(theirs);
  if (!oursJson.is_object())
    return theirs;
  if (!theirsJson.is_object())
    return ours;

  json merged = oursJson;
  std::set<std::string> keys;
  for (auto it = oursJson.begin(); it != oursJson.end(); ++it)
    keys.insert(it.key());
  for (auto it = theirsJson.begin(); it != theirsJson.end(); ++it)
    keys.insert(it.key());

  for (const std::string& key : keys) {
    const bool hasOurs = oursJson.contains(key);
    const bool hasTheirs = theirsJson.contains(key);
    const json o = hasOurs ? oursJson[key] : json();
    const json t = hasTheirs ? theirsJson[key] : json();
    if (o == t)
      continue;

    if ((hasOurs && IsIdArray(o)) || (hasTheirs && IsIdArray(t))) {
      const auto ids = MergeIds(IdList(baseJson, key), IdList(oursJson, key),
                                IdList(theirsJson, key));
      merged[key] = ids;
      continue;
    }
    // A scalar or structured key: whichever side changed it wins, and when
    // both changed it we keep ours — there is no third answer, and the item
    // conflicts are where a person gets asked.
    const bool hasBase = baseJson.is_object() && baseJson.contains(key);
    const json b = hasBase ? baseJson[key] : json();
    if (hasBase && o == b && hasTheirs)
      merged[key] = t;
    else if (!hasOurs && hasTheirs)
      merged[key] = t;
  }
  return merged.dump(2) + "\n";
}

// ---------------------------------------------------------------------------
// Resolutions
// ---------------------------------------------------------------------------

bool TakeSide(const std::string& projectDir, const Conflict& conflict,
              GitVault::MergeSide side, std::string* error) {
  return GitVault::ResolveWith(projectDir, conflict.path, side, error);
}

bool AutoResolve(const std::string& projectDir, const Conflict& conflict,
                 std::string* error) {
  if (conflict.kind == ConflictKind::Derived) {
    // Keyed by the chain hash: a stale blob is ignored and rebuilt, so which
    // side wins cannot make the document wrong.
    return GitVault::ResolveWith(projectDir, conflict.path,
                                 GitVault::MergeSide::Ours, error);
  }
  if (conflict.kind != ConflictKind::Order) {
    if (error)
      *error = "that conflict needs a decision";
    return false;
  }
  GitVault::Stages stages;
  if (!GitVault::ReadStages(projectDir, conflict.path, &stages)) {
    if (error)
      *error = "could not read " + conflict.path;
    return false;
  }
  const std::string merged =
      UnionIndex(stages.base, stages.ours, stages.theirs);
  return GitVault::ResolveWithContent(projectDir, conflict.path, merged, error);
}

bool TakeField(const std::string& projectDir, const Conflict& conflict,
               const std::string& field, GitVault::MergeSide side,
               std::string* error) {
  GitVault::Stages stages;
  if (!GitVault::ReadStages(projectDir, conflict.path, &stages)) {
    if (error)
      *error = "could not read " + conflict.path;
    return false;
  }
  json ours = ParseOrNull(stages.ours);
  const json theirs = ParseOrNull(stages.theirs);
  if (!ours.is_object() || !theirs.is_object()) {
    if (error)
      *error = "that file is not a document item";
    return false;
  }
  // Start from ours and pull the one field across, so the other side's
  // untouched edits to this item survive the decision.
  if (side == GitVault::MergeSide::Theirs) {
    if (theirs.contains(field))
      ours[field] = theirs[field];
    else
      ours.erase(field);
  }
  return GitVault::ResolveWithContent(projectDir, conflict.path,
                                      ours.dump(2) + "\n", error);
}

// ---------------------------------------------------------------------------
// Duplicate ids
// ---------------------------------------------------------------------------

namespace {

// Id spaces that are NOT the document's feature/study/configuration counter.
// A key absent from this list is treated as a possible reference, so a
// feature type added later is over-reported rather than missed — the safe
// direction, because over-reporting refuses a renumber that a person can
// still do by hand, while under-reporting silently re-parents geometry.
bool IsForeignIdSpace(const std::string& key) {
  static const std::set<std::string> kForeign = {
      "entity_id", // sketch-local entity numbering
      "next_entity_id", "next_image_id", "image_id",
      "body_id", // toponaming body index
      "gen_ids", // toponaming generation
      "part_id", // a document id in project.json, not a feature
  };
  return kForeign.count(key) > 0;
}

bool IsReferenceKey(const std::string& key) {
  if (key == "id")
    return false; // identity, not a reference
  if (IsForeignIdSpace(key))
    return false;
  return key.ends_with("_id") || key.ends_with("_ids");
}

// Does `node` refer to `id` anywhere below it?
bool ReferencesId(const json& node, uint64_t id) {
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (IsReferenceKey(it.key())) {
        if (it->is_number_unsigned() && it->get<uint64_t>() == id)
          return true;
        if (it->is_array()) {
          for (const auto& element : *it)
            if (element.is_number_unsigned() && element.get<uint64_t>() == id)
              return true;
        }
      }
      if (ReferencesId(*it, id))
        return true;
    }
    return false;
  }
  if (node.is_array()) {
    for (const auto& element : node)
      if (ReferencesId(element, id))
        return true;
  }
  return false;
}

// Which side of the last merge contributed which file (F9 C3).
//
// The duplicate-id case is a merge git was HAPPY with — two adds, no overlap
// — so it is already committed and MERGE_HEAD is gone. The attribution lives
// in the merge commit's two parents instead: everything in parent 1's tree
// and not in the base was added by us, everything in parent 2's likewise by
// them.
//
// `valid` is false for a project that did not arrive by merge at all (rsync,
// a zip, a hand-edit), where nothing can be attributed and the honest answer
// is the refusal that was there before.
struct SideAttribution {
  bool valid = false;
  std::set<std::string> addedByOurs;
  std::set<std::string> addedByTheirs;
  std::set<std::string> inBase;
};

SideAttribution AttributeSides(const std::string& projectDir) {
  SideAttribution out;
  const auto parents = GitVault::Parents(projectDir, "HEAD");
  if (parents.size() < 2)
    return out; // not a merge commit: nothing to attribute against
  const std::string base =
      GitVault::MergeBase(projectDir, parents[0], parents[1]);
  if (base.empty())
    return out;

  const auto baseFiles = GitVault::ListTree(projectDir, base);
  out.inBase.insert(baseFiles.begin(), baseFiles.end());
  for (const std::string& path : GitVault::ListTree(projectDir, parents[0]))
    if (!out.inBase.count(path))
      out.addedByOurs.insert(path);
  for (const std::string& path : GitVault::ListTree(projectDir, parents[1]))
    if (!out.inBase.count(path))
      out.addedByTheirs.insert(path);
  out.valid = true;
  return out;
}

// Point every reference to `oldId` at `newId`, under the same key rules
// ReferencesId reads. Returns true when anything changed.
bool RewriteId(json& node, uint64_t oldId, uint64_t newId) {
  bool changed = false;
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (IsReferenceKey(it.key())) {
        if (it->is_number_unsigned() && it->get<uint64_t>() == oldId) {
          *it = newId;
          changed = true;
          continue;
        }
        if (it->is_array()) {
          for (auto& element : *it) {
            if (element.is_number_unsigned() &&
                element.get<uint64_t>() == oldId) {
              element = newId;
              changed = true;
            }
          }
        }
      }
      if (RewriteId(*it, oldId, newId))
        changed = true;
    }
    return changed;
  }
  if (node.is_array()) {
    for (auto& element : node)
      if (RewriteId(element, oldId, newId))
        changed = true;
  }
  return changed;
}

// The document directory a collection file sits in: the parent of its
// collection folder ("parts/Stand" for "parts/Stand/features/15-box.json").
fs::path DocumentDirOf(const std::string& projectDir, const std::string& path) {
  return (fs::path(projectDir) / path).parent_path().parent_path();
}

// One past the highest id used anywhere in the document — the same rule
// Document::NoteId applies when it loads, so the fresh id cannot collide
// with anything already there.
uint64_t NextFreeId(const fs::path& documentDir) {
  uint64_t highest = 0;
  std::error_code ec;
  for (const Collection& collection : kCollections) {
    const fs::path dir = documentDir / collection.dir;
    if (!fs::is_directory(dir, ec))
      continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec)
        break;
      highest =
          std::max(highest, IdFromFilename(entry.path().filename().string()));
    }
  }
  return highest + 1;
}

bool WriteText(const fs::path& path, const std::string& text,
               std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) {
    if (error)
      *error = "could not write " + path.filename().string();
    return false;
  }
  return true;
}

} // namespace

bool RenumberDuplicate(const std::string& projectDir, const Conflict& conflict,
                       std::string* error) {
  if (conflict.kind != ConflictKind::DuplicateId) {
    if (error)
      *error = "that conflict is not a duplicate id";
    return false;
  }
  std::error_code ec;
  const fs::path documentDir = DocumentDirOf(projectDir, conflict.path);
  const uint64_t oldId = conflict.id;

  // Which side added which file. When we know, a reference is not ambiguous
  // at all: their extrude meant their sketch, ours meant ours. When we do
  // not, the refusal below is still the only safe answer.
  const SideAttribution sides = AttributeSides(projectDir);
  // Renumber THEIRS and keep ours stable — least surprising for the person
  // who ran the merge. `path` is the second file the scan found, so which of
  // the two is theirs has to be asked, not assumed.
  std::string renumberPath = conflict.path;
  std::string keepPath = conflict.otherPath;
  bool attributed = false;
  if (sides.valid) {
    const bool pathIsTheirs = sides.addedByTheirs.count(conflict.path) > 0;
    const bool otherIsTheirs =
        sides.addedByTheirs.count(conflict.otherPath) > 0;
    // Exactly one side each, and the id must be genuinely NEW — an id that
    // already existed in the base is a different problem wearing the same
    // clothes.
    const bool idIsNew = !std::any_of(
        sides.inBase.begin(), sides.inBase.end(), [&](const std::string& p) {
          return IdFromFilename(fs::path(p).filename().string()) == oldId;
        });
    if (idIsNew && pathIsTheirs != otherIsTheirs) {
      attributed = true;
      if (otherIsTheirs)
        std::swap(renumberPath, keepPath);
    }
  }

  const fs::path full = fs::path(projectDir) / renumberPath;
  const fs::path other = fs::path(projectDir) / keepPath;
  if (!fs::exists(full, ec) || !fs::exists(other, ec)) {
    if (error)
      *error = "both files must still exist to renumber one of them";
    return false;
  }

  // Anything that still points at the contested id and CANNOT be attributed
  // to the side being renumbered makes this unresolvable automatically.
  std::vector<fs::path> rewrite;
  for (const Collection& collection : kCollections) {
    const fs::path dir = documentDir / collection.dir;
    if (!fs::is_directory(dir, ec))
      continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec)
        break;
      if (entry.path() == full || entry.path() == other)
        continue;
      if (entry.path().extension() != ".json")
        continue;
      const json item = ParseOrNull(ReadFile(entry.path()));
      if (item.is_null() || !ReferencesId(item, oldId))
        continue;

      const std::string rel =
          fs::relative(entry.path(), projectDir, ec).generic_string();
      if (attributed && sides.addedByTheirs.count(rel)) {
        // Added by the same side as the file being renumbered, so it meant
        // THAT one. Follow it across.
        rewrite.push_back(entry.path());
        continue;
      }
      if (attributed && sides.addedByOurs.count(rel)) {
        continue; // meant the file that keeps the id — nothing to do
      }
      if (error) {
        *error =
            entry.path().filename().string() + " still refers to id " +
            std::to_string(oldId) +
            (attributed
                 ? ", and neither side added it — it cannot say which of the "
                   "two it meant; open the two files and decide by hand"
                 : ", and it cannot say which of the two it meant — open the "
                   "two files and decide by hand");
      }
      return false;
    }
  }

  const uint64_t newId = NextFreeId(documentDir);
  json item = ParseOrNull(ReadFile(full));
  if (!item.is_object()) {
    if (error)
      *error = "could not read " + conflict.path;
    return false;
  }
  item["id"] = newId;

  // The name carries the id as its prefix — the id alone is identity, so the
  // file has to move with it.
  const std::string filename = full.filename().string();
  const size_t dash = filename.find('-');
  const std::string suffix =
      dash == std::string::npos ? ".json" : filename.substr(dash);
  const fs::path renamed =
      full.parent_path() / (std::to_string(newId) + suffix);

  if (!WriteText(renamed, item.dump(2) + "\n", error))
    return false;
  fs::remove(full, ec);

  // And the index has to list it. When both sides added the id, the merged
  // list holds it TWICE (which the loader refuses); the second occurrence is
  // the one that becomes the new id. Otherwise it goes in beside the first.
  const fs::path indexPath = documentDir / "index.json";
  json index = ParseOrNull(ReadFile(indexPath));
  if (index.is_object()) {
    const Collection* collection =
        CollectionForDir(full.parent_path().filename().string());
    const std::string key = collection ? collection->indexKey : "features";
    if (index.contains(key) && index[key].is_array()) {
      std::vector<uint64_t> ids;
      bool seenOnce = false, replaced = false;
      for (const auto& value : index[key]) {
        if (!value.is_number())
          continue;
        const uint64_t id = value.get<uint64_t>();
        if (id == oldId) {
          if (seenOnce && !replaced) {
            ids.push_back(newId); // the duplicate entry becomes the new id
            replaced = true;
            continue;
          }
          seenOnce = true;
        }
        ids.push_back(id);
      }
      if (!replaced)
        ids.push_back(newId);
      index[key] = ids;
      if (!WriteText(indexPath, index.dump(2) + "\n", error))
        return false;
    }
  }

  // Every reference on the renumbered side follows it across.
  for (const fs::path& path : rewrite) {
    json item2 = ParseOrNull(ReadFile(path));
    if (!item2.is_object() || !RewriteId(item2, oldId, newId))
      continue;
    if (!WriteText(path, item2.dump(2) + "\n", error))
      return false;
  }

  if (GitVault::IsRepo(projectDir)) {
    std::string ignored;
    GitVault::ResolveWithContent(
        projectDir, fs::relative(renamed, projectDir, ec).generic_string(),
        item.dump(2) + "\n", &ignored);
    GitVault::ResolveAsDeleted(projectDir, renumberPath, &ignored);
    for (const fs::path& path : rewrite) {
      GitVault::ResolveWithContent(
          projectDir, fs::relative(path, projectDir, ec).generic_string(),
          ReadFile(path), &ignored);
    }
  }
  LOG_INFO("Renumbered {} from id {} to {} ({} reference(s) followed)",
           renumberPath, oldId, newId, rewrite.size());
  return true;
}

// ---------------------------------------------------------------------------
// The integrity pass
// ---------------------------------------------------------------------------

std::vector<Breakage> Verify(const std::string& projectDir) {
  std::vector<Breakage> broken;
  std::error_code ec;

  // What to load, and as what. A multi-document project says so in
  // project.json; a lone document is its own directory.
  struct Target {
    fs::path dir;
    std::string name, kind;
  };
  std::vector<Target> targets;

  const fs::path projectFile = fs::path(projectDir) / "project.json";
  if (fs::exists(projectFile, ec)) {
    const json meta = ParseOrNull(ReadFile(projectFile));
    if (!meta.is_object() || !meta.contains("documents")) {
      broken.push_back({"", "project",
                        "project.json is not readable — the merge left it "
                        "unusable"});
      return broken;
    }
    for (const auto& entry : meta["documents"]) {
      if (!entry.is_object() || !entry.contains("dir"))
        continue;
      targets.push_back({fs::path(projectDir) / entry.value("dir", ""),
                         entry.value("name", ""), entry.value("type", "part")});
    }
  } else {
    const json index =
        ParseOrNull(ReadFile(fs::path(projectDir) / "index.json"));
    targets.push_back(
        {fs::path(projectDir), fs::path(projectDir).filename().string(),
         index.is_object() ? index.value("kind", "part") : "part"});
  }

  for (const Target& target : targets) {
    const std::string path = target.dir.string();
    std::string failure;
    try {
      if (target.kind == "assembly") {
        auto loaded = DocumentIO::LoadAssembly(path);
        if (!loaded)
          failure = loaded.error();
      } else if (target.kind == "drawing") {
        auto loaded = DocumentIO::LoadDrawing(path);
        if (!loaded)
          failure = loaded.error();
      } else {
        // Load already regenerates, so a recipe that no longer builds shows
        // up here rather than the first time the tab is opened.
        auto loaded = DocumentIO::Load(path);
        if (!loaded)
          failure = loaded.error();
        else if (!(*loaded)->Regenerate())
          failure = "the features no longer rebuild — a resolution left a "
                    "reference with nothing to point at";
      }
    } catch (const std::exception& e) {
      failure = e.what();
    }
    if (!failure.empty())
      broken.push_back({target.name, target.kind, failure});
  }
  return broken;
}

} // namespace ProjectMerge
