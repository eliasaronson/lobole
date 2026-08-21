#include "core/io/Serialization.h"
#include "core/io/GeometryCache.h"
#include "core/io/SerializationInternal.h"

#include "common/Logger.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SweepFeature.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

using nlohmann::json;

namespace DocumentIO {

namespace {

// Path classification (F3): a `.dsgn` path or existing directory is the
// directory format; picking the index.json inside one opens the project.
bool IsDirFormatPath(const std::string& path) {
  if (path.ends_with(".dsgn"))
    return true;
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

std::string NormalizeProjectPath(const std::string& path) {
  const std::filesystem::path p(path);
  if (p.filename() == "index.json" &&
      p.parent_path().string().ends_with(".dsgn"))
    return p.parent_path().string();
  return path;
}

// "DatumPlane" -> "datum-plane": readable feature filenames. The stable id
// prefix is the identity; this suffix is cosmetic.
std::string KebabType(const std::string& type) {
  std::string out;
  for (char c : type) {
    if (std::isupper(static_cast<unsigned char>(c))) {
      if (!out.empty())
        out += '-';
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      out += c;
    }
  }
  return out;
}

json DocumentEnvelope(const Document& doc) {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  if (doc.Rollback() >= 0) {
    j["rollback"] = doc.Rollback();
  }
  // Omitted at the default (true) so existing projects re-save byte-identically
  // and the goldens stay valid — the recipe format is untouched by baking.
  if (!doc.CommitGeometry()) {
    j["commit_geometry"] = false;
  }
  if (!doc.GetParameters().empty()) {
    j["parameters"] = json::array();
    for (const Parameter& p : doc.GetParameters()) {
      json pj = {{"name", p.name}, {"expression", p.expression}};
      if (!p.comment.empty()) {
        pj["comment"] = p.comment;
      }
      j["parameters"].push_back(std::move(pj));
    }
  }
  // The active configuration (v31), omitted at base (0) so pre-v31 documents
  // re-save byte-identically. The configurations themselves are written per
  // save path (inline / configurations/ dir), like studies.
  if (doc.ActiveConfiguration() != 0) {
    j["active_configuration"] = doc.ActiveConfiguration();
  }
  return j;
}

// ---- Assemblies (A2) ------------------------------------------------------
// An assembly shares the document format string + version, discriminated by
// `kind: "assembly"`. OccurrenceToJson/FromJson are public (defined near
// FeatureToJson); these are the internal file-layout helpers. See
// docs/assemblies/a2-design.md.

json AssemblyEnvelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "assembly";
  return j;
}

// A filesystem-safe, lowercase [a-z0-9-] slug for an occurrence's or mate's
// cosmetic label (user-set, unlike a feature's PascalCase TypeName). The
// stable id is the real identity — files load by the numeric prefix — so a
// lossy slug is fine; this only keeps the filename readable and safe.
std::string LabelSlug(const std::string& label) {
  std::string out;
  for (char c : label) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc))
      out += static_cast<char>(std::tolower(uc));
    else if (!out.empty() && out.back() != '-')
      out += '-';
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out;
}

// Restores occurrences into `asm` in the given order and pins the id counter
// past the highest restored id so future adds never collide.
void LoadOccurrences(AssemblyDocument& asmDoc,
                     const std::vector<json>& occJsons) {
  uint64_t maxId = 0;
  for (const json& oj : occJsons) {
    Occurrence o = OccurrenceFromJson(oj);
    maxId = std::max(maxId, o.id);
    asmDoc.GetOccurrences().push_back(std::move(o));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Restores mates into `asm` in the given order and pins the id counter past
// the highest restored id. Mates share the id space with occurrences, so
// pinning here (after LoadOccurrences) leaves the counter above both.
void LoadMates(AssemblyDocument& asmDoc, const std::vector<json>& mateJsons) {
  uint64_t maxId = 0;
  for (const json& mj : mateJsons) {
    Mate m = MateFromJson(mj);
    maxId = std::max(maxId, m.id);
    asmDoc.GetMates().push_back(std::move(m));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Exploded views (v27) follow the same restore-and-pin discipline.
void LoadExplodedViews(AssemblyDocument& asmDoc,
                       const std::vector<json>& viewJsons) {
  uint64_t maxId = 0;
  for (const json& vj : viewJsons) {
    ExplodedView v = ExplodedViewFromJson(vj);
    maxId = std::max(maxId, v.id);
    asmDoc.GetExplodedViews().push_back(std::move(v));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Binary sibling of WriteTextAtomic for reference-image pixel files (v49):
// same temp+rename + identical-content skip, but byte-exact (no text-mode
// translation).
bool WriteBytesAtomic(const std::filesystem::path& target,
                      const std::vector<unsigned char>& bytes,
                      std::string* error) {
  {
    std::ifstream existing(target, std::ios::binary);
    if (existing) {
      std::string current((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
      if (current.size() == bytes.size() &&
          std::equal(bytes.begin(), bytes.end(), current.begin()))
        return true;
    }
  }
  const std::filesystem::path tmp = target.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + tmp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
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

// ---- Reference-image (v49) asset transport --------------------------------
// Pixel bytes live outside the feature JSON so it stays small and diffable: an
// assets/<name> file in the directory format, a base64 blob under a document-
// root "assets" map in the single-file form. FeatureToJson/FromJson carry only
// the metadata + `asset` name; these helpers move the bytes.

std::string Base64Encode(const std::vector<unsigned char>& in) {
  static constexpr char kTab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) |
                       uint32_t(in[i + 2]);
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back(kTab[(n >> 6) & 63]);
    out.push_back(kTab[n & 63]);
  }
  if (i + 1 == in.size()) {
    const uint32_t n = uint32_t(in[i]) << 16;
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == in.size()) {
    const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back(kTab[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::vector<unsigned char> Base64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1; // '=' padding and any whitespace are skipped
  };
  std::vector<unsigned char> out;
  out.reserve(in.size() / 4 * 3);
  int buf = 0, bits = 0;
  for (char c : in) {
    const int v = val(c);
    if (v < 0)
      continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

// Asset names actually referenced by a reference image somewhere in the doc —
// so serialization persists only live assets and prunes orphans.
std::set<std::string> ReferencedAssets(const Document& doc) {
  std::set<std::string> names;
  for (const auto& f : doc.GetFeatures())
    if (auto* sk = dynamic_cast<const SketchFeature*>(f.get()))
      for (const auto& img : sk->images)
        if (!img.asset.empty())
          names.insert(img.asset);
  return names;
}

// Directory format (F3, docs/versioning/f3-directory-format.md): one file
// per feature named by stable id, an ordered index written LAST (a torn
// save leaves a stale-but-loadable index), document-level parameters file.
bool SaveDir(const Document& doc, const std::string& dirPath,
             std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    const fs::path featuresDir = dir / "features";
    fs::create_directories(featuresDir);

    json index = DocumentEnvelope(doc);
    index["features"] = json::array();
    std::set<std::string> wanted;
    for (const auto& feature : doc.GetFeatures()) {
      const std::string file = std::to_string(feature->GetId()) + "-" +
                               KebabType(feature->TypeName()) + ".json";
      if (!WriteTextAtomic(featuresDir / file,
                           FeatureToJson(*feature).dump(2) + "\n", error))
        return false;
      index["features"].push_back(feature->GetId());
      wanted.insert(file);
    }
    // Deleted features leave no stale files behind.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(featuresDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wanted.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }
    // Baked geometry (docs/versioning/baked-geometry.md): the built bodies
    // beside the recipe, so the next open is a load. Best-effort — a project
    // that cannot bake is still a valid project, just a slower one to open.
    if (doc.CommitGeometry()) {
      std::string bakeError;
      if (!SaveGeometry(doc, dirPath, &bakeError))
        LOG_WARN("Could not bake geometry: {}", bakeError);
    } else {
      RemoveGeometry(dirPath);
    }
    // The app creates and commits the project's repo itself (GitVault), so it
    // is the app's job to say how the blob should be treated there.
    WriteRepositoryHints(dirPath, doc.CommitGeometry());
    // FEM studies (v29): file-per-study under studies/, id list in the index.
    // Both the key and the directory stay absent when there are no studies, so
    // a pre-v29 document re-saves byte-identically (the exploded-views v27
    // discipline). Mesh/results are derived and never written.
    if (!doc.GetStudies().empty()) {
      const fs::path studyDir = dir / "studies";
      fs::create_directories(studyDir);
      index["studies"] = json::array();
      std::set<std::string> wantedStudies;
      for (const fem::Study& s : doc.GetStudies()) {
        const std::string base = std::to_string(s.id) + "-" + LabelSlug(s.name);
        const std::string file = base + ".json";
        if (!WriteTextAtomic(studyDir / file, StudyToJson(s).dump(2) + "\n",
                             error))
          return false;
        index["studies"].push_back(s.id);
        wantedStudies.insert(file);
        // A topology study's optimized result is persisted (v33) — unlike FEM
        // results, which are derived and never written.
        const fem::StudyRuntime* rt = doc.StudyResults(s.id);
        json resultJson;
        if (rt && StudyResultToJson(*rt, resultJson)) {
          const std::string resultFile = base + ".result.json";
          if (!WriteTextAtomic(studyDir / resultFile, resultJson.dump() + "\n",
                               error))
            return false;
          wantedStudies.insert(resultFile);
        }
      }
      for (const auto& entry : fs::directory_iterator(studyDir, ec)) {
        if (entry.path().extension() == ".json" &&
            !wantedStudies.contains(entry.path().filename().string()))
          fs::remove(entry.path(), ec);
      }
    }
    // CAM setups (v58): inline in index.json, not a file each. They are
    // parameters (a few hundred bytes), and index.json is as diffable as any
    // other file in the directory — studies earned their own files by
    // carrying meshes, and a setup carries none.
    if (!doc.GetCamSetups().empty()) {
      index["cam"] = json::array();
      for (const cam::Setup& setup : doc.GetCamSetups())
        index["cam"].push_back(CamSetupToJson(setup));
    }
    // Configurations (v31): file-per-config under configurations/, id list in
    // the index (the active id is in the envelope). Absent when none, so a
    // pre-v31 document re-saves byte-identically.
    if (!doc.GetConfigurations().empty()) {
      const fs::path cfgDir = dir / "configurations";
      fs::create_directories(cfgDir);
      index["configurations"] = json::array();
      std::set<std::string> wantedConfigs;
      for (const Configuration& c : doc.GetConfigurations()) {
        const std::string file =
            std::to_string(c.id) + "-" + LabelSlug(c.name) + ".json";
        if (!WriteTextAtomic(cfgDir / file,
                             ConfigurationToJson(c).dump(2) + "\n", error))
          return false;
        index["configurations"].push_back(c.id);
        wantedConfigs.insert(file);
      }
      for (const auto& entry : fs::directory_iterator(cfgDir, ec)) {
        if (entry.path().extension() == ".json" &&
            !wantedConfigs.contains(entry.path().filename().string()))
          fs::remove(entry.path(), ec);
      }
    }
    // Reference-image pixel files (v49) under assets/, pruned like features/.
    // The dir stays absent when there are no images, so a pre-v49 document
    // re-saves byte-identically.
    {
      const std::set<std::string> wantedAssets = ReferencedAssets(doc);
      if (!wantedAssets.empty()) {
        const fs::path assetDir = dir / "assets";
        fs::create_directories(assetDir);
        for (const std::string& name : wantedAssets) {
          const std::vector<unsigned char>* bytes = doc.GetAsset(name);
          if (!bytes || bytes->empty())
            continue;
          if (!WriteBytesAtomic(assetDir / name, *bytes, error))
            return false;
        }
        for (const auto& entry : fs::directory_iterator(assetDir, ec))
          if (!wantedAssets.contains(entry.path().filename().string()))
            fs::remove(entry.path(), ec);
      }
    }
    // Parameters live in the index envelope (small, diff-friendly there).
    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

namespace {

// "features/3-sketch.json", for messages: the bare filename is ambiguous
// across features/, studies/, views/… and the absolute path is noise.
std::string ProjectRelative(const std::filesystem::path& path) {
  return (path.parent_path().filename() / path.filename()).string();
}

// Reads and parses one project file, saying WHICH file when it cannot.
//
// nlohmann reports a line and column and no filename, and in a project that
// is a git repository the usual cause is not a typo: a merge left conflict
// markers in the file. The directory format merges well precisely so people
// WILL merge these projects (one file per feature, one key per line), which
// makes "unresolved markers" a first-class failure worth naming.
json ParseProjectFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read " + ProjectRelative(path));
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  if (text.starts_with("<<<<<<<") ||
      text.find("\n<<<<<<<") != std::string::npos) {
    throw std::runtime_error(
        ProjectRelative(path) +
        " still has merge conflict markers — finish the merge (git status "
        "shows what is unresolved), then open the project again");
  }
  try {
    return json::parse(text);
  } catch (const std::exception& e) {
    throw std::runtime_error(ProjectRelative(path) + ": " + e.what());
  }
}

// Records `path` under the id its filename starts with, REFUSING a second
// file that claims the same id.
//
// This used to be `byId[std::stoull(name)] = path`, which silently kept the
// last one. Two files never share an id when the app writes them — but a
// merge makes it easily: git takes both "15-box.json" and "15-hole.json"
// without a word when two people each added a feature offline and the id
// counter handed them both 15. The overwrite then dropped one person's work
// and left the document with two entries claiming one id, which every
// reference (sketch_id, body ids, mates, configuration overrides) then
// resolves ambiguously. Loudly refusing is the only safe answer; the fix in
// the file is to renumber one of them.
void InsertIdFile(std::map<uint64_t, std::filesystem::path>& byId,
                  const std::filesystem::path& path, const char* what) {
  const uint64_t id = std::stoull(path.filename().string());
  const auto [it, inserted] = byId.emplace(id, path);
  if (!inserted) {
    throw std::runtime_error(
        "two " + std::string(what) + " files claim id " + std::to_string(id) +
        " (" + ProjectRelative(it->second) + " and " + ProjectRelative(path) +
        ") — a merge kept both; renumber one of them (its \"id\" field and "
        "its filename) and reopen");
  }
}

// The index lists the ids, in order. A duplicate there means the same file
// loads twice — two features, one id — which is the other half of the merge
// hazard above and just as ambiguous downstream.
void CheckIndexIdsUnique(const json& ids, const char* what) {
  std::set<uint64_t> seen;
  for (const auto& idJson : ids) {
    const uint64_t id = idJson.get<uint64_t>();
    if (!seen.insert(id).second) {
      throw std::runtime_error(
          "index.json lists " + std::string(what) + " id " +
          std::to_string(id) +
          " twice — a merge kept both sides; keep one and reopen");
    }
  }
}

} // namespace

std::unique_ptr<Document> LoadDir(const std::string& dirPath,
                                  std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "part") {
      if (error)
        *error = "This is a " + index.value("kind", "part") +
                 ", not a part: " + dirPath;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    doc->SetCommitGeometry(index.value("commit_geometry", true));
    for (const auto& pj : index.value("parameters", json::array())) {
      doc->SetParameter(pj.at("name").get<std::string>(),
                        pj.at("expression").get<std::string>(),
                        pj.value("comment", std::string{}));
    }
    // Feature files are found by their id prefix — the type suffix is
    // cosmetic and may lag a (hypothetical) rename.
    std::map<uint64_t, fs::path> byId;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "features", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(byId, entry.path(), "feature");
    }
    // One regeneration for the whole file, not one per feature (Document::
    // BulkEdit). The guard is released explicitly below — BEFORE the topology
    // results are stamped, since they must carry the settled version.
    auto bulk = std::make_optional<Document::BulkEdit>(*doc);
    CheckIndexIdsUnique(index.at("features"), "feature");
    for (const auto& idJson : index.at("features")) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing feature file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      doc->AddFeature(FeatureFromJson(ParseProjectFile(it->second)));
    }
    // Reference-image bytes (v49) from assets/ into the doc store (absent dir ⇒
    // nothing to do). Only assets an image references are loaded.
    for (const std::string& name : ReferencedAssets(*doc)) {
      std::ifstream ain(fs::path(dirPath) / "assets" / name, std::ios::binary);
      if (ain) {
        std::string data((std::istreambuf_iterator<char>(ain)),
                         std::istreambuf_iterator<char>());
        doc->SetAsset(name,
                      std::vector<unsigned char>(data.begin(), data.end()));
      }
    }
    // Topology results (v33) loaded below but stored after the final regen.
    std::vector<std::pair<uint64_t, fem::StudyRuntime>> pendingResults;
    // FEM studies (v29): id-prefix files under studies/, index order. A pre-v29
    // document has no `studies` key ⇒ the loop is empty.
    {
      std::map<uint64_t, fs::path> studyById, resultById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "studies", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        const std::string stem = entry.path().stem().string(); // strips ".json"
        // A topology result file is "<id>-<slug>.result.json": its stem ends
        // ".result" — route it to the result map, not the study-def map.
        if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, ".result") == 0)
          InsertIdFile(resultById, entry.path(), "study result");
        else
          InsertIdFile(studyById, entry.path(), "study");
      }
      CheckIndexIdsUnique(index.value("studies", json::array()), "study");
      for (const auto& idJson : index.value("studies", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = studyById.find(id);
        if (it == studyById.end()) {
          if (error)
            *error = "missing study file for id " + std::to_string(id);
          return nullptr;
        }
        std::ifstream sin(it->second);
        doc->RestoreStudy(StudyFromJson(ParseProjectFile(it->second)));
        // A persisted topology result loads into the transient runtime cache.
        // Defer storing it until after the final regeneration below so it can
        // be stamped with the settled document version (else the rollback /
        // active-config regen would leave it spuriously stale).
        const auto rit = resultById.find(id);
        if (rit != resultById.end()) {
          std::ifstream rin(rit->second);
          pendingResults.emplace_back(
              id, StudyResultFromJson(ParseProjectFile(rit->second)));
        }
      }
    }
    for (const auto& cj : index.value("cam", json::array())) { // v58
      doc->GetCamSetups().push_back(CamSetupFromJson(cj));
      doc->EnsureNextIdAtLeast(doc->GetCamSetups().back().id + 1);
    }
    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(cfgById, entry.path(), "configuration");
      }
      CheckIndexIdsUnique(index.value("configurations", json::array()),
                          "configuration");
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        doc->RestoreConfiguration(
            ConfigurationFromJson(ParseProjectFile(it->second)));
      }
    }
    doc->SetRollback(index.value("rollback", -1));
    // Apply the active configuration last so the final regeneration reflects
    // it.
    doc->SetActiveConfiguration(
        index.value("active_configuration", uint64_t{0}));
    // Adopt the baked bodies BEFORE the regeneration below: it is that
    // regeneration which decides whether they still describe the recipe.
    LoadGeometry(*doc, dirPath);
    bulk.reset(); // the one regeneration for everything loaded above
    // Now that regeneration has settled, stamp the persisted topology results
    // with the final version so they read as current (v33).
    for (auto& [id, rt] : pendingResults) {
      rt.solvedAtVersion = doc->Version();
      doc->StoreStudyResults(id, std::move(rt));
    }
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             dirPath);
    return doc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load project directory {}: {}", dirPath, e.what());
    return nullptr;
  }
}

// Assembly directory format (mirrors SaveDir): one file per occurrence named
// by its stable id, an ordered index written LAST, `kind: "assembly"`.
bool SaveAssemblyDir(const AssemblyDocument& asmDoc, const std::string& dirPath,
                     std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    const fs::path occDir = dir / "occurrences";
    fs::create_directories(occDir);

    json index = AssemblyEnvelope();
    index["occurrences"] = json::array();
    std::set<std::string> wantedOcc;
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      const std::string file =
          std::to_string(o.id) + "-" + LabelSlug(o.name) + ".json";
      if (!WriteTextAtomic(occDir / file, OccurrenceToJson(o).dump(2) + "\n",
                           error))
        return false;
      index["occurrences"].push_back(o.id);
      wantedOcc.insert(file);
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(occDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedOcc.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Mates mirror the occurrence block: one file per mate under mates/, the
    // ordered id list in the same index.json (written last). A v23 assembly
    // has none — the mates/ dir simply stays empty.
    const fs::path mateDir = dir / "mates";
    index["mates"] = json::array();
    if (!asmDoc.GetMates().empty())
      fs::create_directories(mateDir);
    std::set<std::string> wantedMate;
    for (const Mate& m : asmDoc.GetMates()) {
      const std::string file =
          std::to_string(m.id) + "-" + LabelSlug(m.name) + ".json";
      if (!WriteTextAtomic(mateDir / file, MateToJson(m).dump(2) + "\n", error))
        return false;
      index["mates"].push_back(m.id);
      wantedMate.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(mateDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedMate.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Exploded views (v27) mirror mates: file-per-view under exploded/, the
    // id list in the index. Both the key and the dir are absent when there
    // are none, so a v26 assembly stays byte-identical.
    const fs::path viewDir = dir / "exploded";
    if (!asmDoc.GetExplodedViews().empty()) {
      index["exploded"] = json::array();
      fs::create_directories(viewDir);
    }
    std::set<std::string> wantedView;
    for (const ExplodedView& v : asmDoc.GetExplodedViews()) {
      const std::string file =
          std::to_string(v.id) + "-" + LabelSlug(v.name) + ".json";
      if (!WriteTextAtomic(viewDir / file, ExplodedViewToJson(v).dump(2) + "\n",
                           error))
        return false;
      index["exploded"].push_back(v.id);
      wantedView.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(viewDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedView.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Configurations (v31): file-per-config + active id in the index; absent
    // when none so older assemblies stay byte-identical.
    if (asmDoc.ActiveConfiguration() != 0)
      index["active_configuration"] = asmDoc.ActiveConfiguration();
    const fs::path cfgDir = dir / "configurations";
    if (!asmDoc.GetConfigurations().empty()) {
      index["configurations"] = json::array();
      fs::create_directories(cfgDir);
    }
    std::set<std::string> wantedCfg;
    for (const Configuration& c : asmDoc.GetConfigurations()) {
      const std::string file =
          std::to_string(c.id) + "-" + LabelSlug(c.name) + ".json";
      if (!WriteTextAtomic(cfgDir / file, ConfigurationToJson(c).dump(2) + "\n",
                           error))
        return false;
      index["configurations"].push_back(c.id);
      wantedCfg.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(cfgDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedCfg.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved assembly ({} occurrences, {} mates) to {}",
             asmDoc.OccurrenceCount(), asmDoc.MateCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

std::unique_ptr<AssemblyDocument> LoadAssemblyDir(const std::string& dirPath,
                                                  std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "assembly") {
      if (error)
        *error = "Not an assembly: " + dirPath;
      return nullptr;
    }

    // Occurrence files are found by their id prefix — the label suffix is
    // cosmetic and may lag a rename.
    std::map<uint64_t, fs::path> byId;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "occurrences", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(byId, entry.path(), "occurrence");
    }
    std::vector<json> occJsons;
    CheckIndexIdsUnique(index.value("occurrences", json::array()),
                        "occurrence");
    for (const auto& idJson : index.value("occurrences", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing occurrence file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      occJsons.push_back(ParseProjectFile(it->second));
    }
    auto asmDoc = std::make_unique<AssemblyDocument>();
    LoadOccurrences(*asmDoc, occJsons);

    // Mates (v24): found by id prefix under mates/, loaded in index order. A
    // v23 assembly has no `mates` key ⇒ the loop is empty.
    std::map<uint64_t, fs::path> mateById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "mates", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(mateById, entry.path(), "mate");
    }
    std::vector<json> mateJsons;
    CheckIndexIdsUnique(index.value("mates", json::array()), "mate");
    for (const auto& idJson : index.value("mates", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = mateById.find(id);
      if (it == mateById.end()) {
        if (error)
          *error = "missing mate file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      mateJsons.push_back(ParseProjectFile(it->second));
    }
    LoadMates(*asmDoc, mateJsons);

    // Exploded views (v27): id-prefix files under exploded/, index order. A
    // pre-v27 assembly has no `exploded` key ⇒ the loop is empty.
    std::map<uint64_t, fs::path> viewById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "exploded", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(viewById, entry.path(), "view");
    }
    std::vector<json> viewJsons;
    for (const auto& idJson : index.value("exploded", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = viewById.find(id);
      if (it == viewById.end()) {
        if (error)
          *error = "missing exploded-view file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      viewJsons.push_back(ParseProjectFile(it->second));
    }
    LoadExplodedViews(*asmDoc, viewJsons);

    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(cfgById, entry.path(), "configuration");
      }
      CheckIndexIdsUnique(index.value("configurations", json::array()),
                          "configuration");
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        asmDoc->RestoreConfiguration(
            ConfigurationFromJson(ParseProjectFile(it->second)));
      }
    }
    asmDoc->SetActiveConfiguration(
        index.value("active_configuration", uint64_t{0}));

    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), dirPath);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load assembly directory {}: {}", dirPath, e.what());
    return nullptr;
  }
}

} // namespace

static bool SaveImpl(const Document& doc, const std::string& path,
                     std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveDir(doc, normalized, error);
  }
  try {
    json j = DocumentEnvelope(doc);
    j["features"] = json::array();
    for (const auto& feature : doc.GetFeatures()) {
      j["features"].push_back(FeatureToJson(*feature));
    }
    // FEM studies (v29), inline here (unlike the dir format's studies/ files);
    // absent when none so pre-v29 files stay byte-identical.
    if (!doc.GetStudies().empty()) {
      j["studies"] = json::array();
      for (const fem::Study& s : doc.GetStudies())
        j["studies"].push_back(StudyToJson(s));
    }
    // CAM setups (v58), inline in both formats: a setup is parameters, not a
    // mesh, so it does not earn a file of its own the way a study does.
    if (!doc.GetCamSetups().empty()) {
      j["cam"] = json::array();
      for (const cam::Setup& setup : doc.GetCamSetups())
        j["cam"].push_back(CamSetupToJson(setup));
    }
    // Configurations (v31), inline (the active id is in the envelope above).
    if (!doc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : doc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }
    // Reference-image bytes (v49) base64 under a document-root "assets" map so
    // the single-file form stays self-contained. Absent when there are none.
    {
      json assets = json::object();
      for (const std::string& name : ReferencedAssets(doc)) {
        const std::vector<unsigned char>* bytes = doc.GetAsset(name);
        if (bytes && !bytes->empty())
          assets[name] = Base64Encode(*bytes);
      }
      if (!assets.empty())
        j["assets"] = std::move(assets);
    }

    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save document to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<Document> LoadImpl(const std::string& path,
                                          std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);

    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "part") {
      if (error)
        *error =
            "This is a " + j.value("kind", "part") + ", not a part: " + path;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    doc->SetCommitGeometry(j.value("commit_geometry", true));
    // Parameters first: features' expressions evaluate against them on the
    // regeneration each AddFeature triggers.
    for (const auto& pj : j.value("parameters", json::array())) {
      doc->SetParameter(pj.at("name").get<std::string>(),
                        pj.at("expression").get<std::string>(),
                        pj.value("comment", std::string{}));
    }
    {
      Document::BulkEdit bulk(*doc); // one regeneration, not one per feature
      for (const auto& fj : j.at("features")) {
        doc->AddFeature(FeatureFromJson(fj));
      }
    }
    // Reference-image bytes (v49): decode the base64 "assets" map into the doc
    // asset store.
    if (j.contains("assets"))
      for (const auto& [name, b64] : j.at("assets").items())
        doc->SetAsset(name, Base64Decode(b64.get<std::string>()));
    for (const auto& sj : j.value("studies", json::array())) { // v29
      doc->RestoreStudy(StudyFromJson(sj));
    }
    for (const auto& cj : j.value("cam", json::array())) { // v58
      doc->GetCamSetups().push_back(CamSetupFromJson(cj));
      doc->EnsureNextIdAtLeast(doc->GetCamSetups().back().id + 1);
    }
    for (const auto& cj : j.value("configurations", json::array())) { // v31
      doc->RestoreConfiguration(ConfigurationFromJson(cj));
    }
    doc->SetRollback(j.value("rollback", -1)); // v19; absent = end
    doc->SetActiveConfiguration(j.value("active_configuration", uint64_t{0}));
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             path);
    return doc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load document from {}: {}", path, e.what());
    return nullptr;
  }
}

static bool SaveAssemblyImpl(const AssemblyDocument& asmDoc,
                             const std::string& path, std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveAssemblyDir(asmDoc, normalized, error);
  }
  try {
    json j = AssemblyEnvelope();
    j["occurrences"] = json::array();
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      j["occurrences"].push_back(OccurrenceToJson(o));
    }
    j["mates"] = json::array();
    for (const Mate& m : asmDoc.GetMates()) {
      j["mates"].push_back(MateToJson(m));
    }
    if (!asmDoc.GetExplodedViews().empty()) { // absent when none (v27)
      j["exploded"] = json::array();
      for (const ExplodedView& v : asmDoc.GetExplodedViews())
        j["exploded"].push_back(ExplodedViewToJson(v));
    }
    if (asmDoc.ActiveConfiguration() != 0) // v31
      j["active_configuration"] = asmDoc.ActiveConfiguration();
    if (!asmDoc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : asmDoc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }
    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved assembly ({} occurrences) to {}", asmDoc.OccurrenceCount(),
             path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save assembly to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<AssemblyDocument>
LoadAssemblyImpl(const std::string& path, std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadAssemblyDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);
    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "assembly") {
      if (error)
        *error = "Not an assembly: " + path;
      return nullptr;
    }
    auto asmDoc = std::make_unique<AssemblyDocument>();
    LoadOccurrences(
        *asmDoc,
        j.value("occurrences", json::array()).get<std::vector<json>>());
    LoadMates(*asmDoc,
              j.value("mates", json::array()).get<std::vector<json>>());
    LoadExplodedViews(
        *asmDoc, j.value("exploded", json::array()).get<std::vector<json>>());
    for (const auto& cj : j.value("configurations", json::array())) // v31
      asmDoc->RestoreConfiguration(ConfigurationFromJson(cj));
    asmDoc->SetActiveConfiguration(
        j.value("active_configuration", uint64_t{0}));
    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), path);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load assembly from {}: {}", path, e.what());
    return nullptr;
  }
}

// ---- Drawings (v30) --------------------------------------------------------
// A drawing serializes like a part/assembly: kind:"drawing", the sheet inline,
// one file per view and per dim in the dir format. Projected geometry and
// measured dimension values are derived, never stored (design doc §2).

namespace {

json SheetJson(const Sheet& s) {
  json tb = json::object();
  const TitleBlock& t = s.titleBlock;
  if (!t.project.empty())
    tb["project"] = t.project;
  if (!t.document.empty())
    tb["document"] = t.document;
  if (!t.author.empty())
    tb["author"] = t.author;
  if (!t.date.empty())
    tb["date"] = t.date;
  if (!t.notes.empty())
    tb["notes"] = t.notes;
  json j = {{"size", SheetSizeToString(s.size)}, {"landscape", s.landscape}};
  if (!tb.empty())
    j["title_block"] = std::move(tb);
  return j;
}

Sheet SheetFromJson(const json& j) {
  Sheet s;
  SheetSizeFromString(j.value("size", std::string("a4")), s.size);
  s.landscape = j.value("landscape", true);
  const json tb = j.value("title_block", json::object());
  s.titleBlock.project = tb.value("project", std::string{});
  s.titleBlock.document = tb.value("document", std::string{});
  s.titleBlock.author = tb.value("author", std::string{});
  s.titleBlock.date = tb.value("date", std::string{});
  s.titleBlock.notes = tb.value("notes", std::string{});
  return s;
}

json ProjRefJson(const ProjRef& r) {
  json j = {{"view", r.viewId}, {"end", r.end}};
  if (r.occurrenceId)
    j["occurrence"] = r.occurrenceId;
  if (r.edge.IsSet())
    j["edge"] = ElementRefJson(r.edge);
  if (r.silFace.IsSet())
    j["sil_face"] = ElementRefJson(r.silFace);
  return j;
}

ProjRef ProjRefFromJson(const json& j) {
  ProjRef r;
  r.viewId = j.value("view", uint64_t{0});
  r.occurrenceId = j.value("occurrence", uint64_t{0});
  r.end = j.value("end", 0);
  if (j.contains("edge"))
    r.edge = ElementRefFromJson(j.at("edge"));
  if (j.contains("sil_face"))
    r.silFace = ElementRefFromJson(j.at("sil_face"));
  return r;
}

json DrawingEnvelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "drawing";
  return j;
}

} // namespace

json DrawingViewToJson(const DrawingView& v) {
  json j = {{"id", v.id},
            {"name", v.name},
            {"source", v.sourceDocId},
            {"orientation", ViewOrientationToString(v.orientation)},
            {"scale", v.scale},
            {"position", {v.position.x, v.position.y}},
            {"show_hidden", v.showHidden}};
  if (v.baseViewId)
    j["base_view"] = v.baseViewId;
  if (!v.scaleExpr.empty())
    j["scale_expr"] = v.scaleExpr;
  return j;
}

DrawingView DrawingViewFromJson(const json& j) {
  DrawingView v;
  v.id = j.value("id", uint64_t{0});
  v.name = j.value("name", std::string{});
  v.sourceDocId = j.value("source", uint64_t{0});
  ViewOrientationFromString(j.value("orientation", std::string("front")),
                            v.orientation);
  v.baseViewId = j.value("base_view", uint64_t{0});
  v.scale = j.value("scale", 1.0);
  v.scaleExpr = j.value("scale_expr", std::string{});
  const json p = j.value("position", json::array({0.0, 0.0}));
  if (p.is_array() && p.size() == 2)
    v.position = {p[0].get<double>(), p[1].get<double>()};
  v.showHidden = j.value("show_hidden", true);
  return v;
}

json DrawingDimToJson(const DrawingDim& d) {
  json refs = json::array();
  for (const ProjRef& r : d.refs)
    refs.push_back(ProjRefJson(r));
  return json{{"id", d.id},
              {"kind", DrawingDimKindToString(d.kind)},
              {"view", d.viewId},
              {"refs", std::move(refs)},
              {"label_offset", {d.labelOffset.x, d.labelOffset.y}}};
}

DrawingDim DrawingDimFromJson(const json& j) {
  DrawingDim d;
  d.id = j.value("id", uint64_t{0});
  // An unknown kind string used to silently default to Distance — surface it
  // instead (the version gate is the real forward-compat guard, so this only
  // fires on corrupt/hand-edited files).
  const std::string kind = j.value("kind", std::string("distance"));
  if (!DrawingDimKindFromString(kind, d.kind))
    throw std::runtime_error("unknown drawing dimension kind '" + kind + "'");
  d.viewId = j.value("view", uint64_t{0});
  for (const json& rj : j.value("refs", json::array()))
    d.refs.push_back(ProjRefFromJson(rj));
  const json o = j.value("label_offset", json::array({0.0, 0.0}));
  if (o.is_array() && o.size() == 2)
    d.labelOffset = {o[0].get<double>(), o[1].get<double>()};
  return d;
}

namespace {

bool SaveDrawingDir(const DrawingDocument& dwg, const std::string& dirPath,
                    std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    fs::create_directories(dir);

    json index = DrawingEnvelope();
    index["sheet"] = SheetJson(dwg.GetSheet());

    const fs::path viewDir = dir / "views";
    index["views"] = json::array();
    if (!dwg.GetViews().empty())
      fs::create_directories(viewDir);
    std::set<std::string> wantedView;
    for (const DrawingView& v : dwg.GetViews()) {
      const std::string file =
          std::to_string(v.id) + "-" + LabelSlug(v.name) + ".json";
      if (!WriteTextAtomic(viewDir / file, DrawingViewToJson(v).dump(2) + "\n",
                           error))
        return false;
      index["views"].push_back(v.id);
      wantedView.insert(file);
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(viewDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedView.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    const fs::path dimDir = dir / "dims";
    index["dims"] = json::array();
    if (!dwg.GetDims().empty())
      fs::create_directories(dimDir);
    std::set<std::string> wantedDim;
    for (const DrawingDim& d : dwg.GetDims()) {
      const std::string file =
          std::to_string(d.id) + "-" + DrawingDimKindToString(d.kind) + ".json";
      if (!WriteTextAtomic(dimDir / file, DrawingDimToJson(d).dump(2) + "\n",
                           error))
        return false;
      index["dims"].push_back(d.id);
      wantedDim.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(dimDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedDim.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved drawing ({} views, {} dims) to {}", dwg.ViewCount(),
             dwg.DimCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

std::unique_ptr<DrawingDocument> LoadDrawingDir(const std::string& dirPath,
                                                std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "drawing") {
      if (error)
        *error = "Not a drawing: " + dirPath;
      return nullptr;
    }

    auto dwg = std::make_unique<DrawingDocument>();
    dwg->GetSheet() = SheetFromJson(index.value("sheet", json::object()));

    std::map<uint64_t, fs::path> viewById;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "views", ec)) {
      if (entry.path().extension() == ".json")
        InsertIdFile(viewById, entry.path(), "view");
    }
    uint64_t maxId = 0;
    CheckIndexIdsUnique(index.value("views", json::array()), "view");
    for (const auto& idJson : index.value("views", json::array())) {
      const auto it = viewById.find(idJson.get<uint64_t>());
      if (it == viewById.end())
        continue;
      std::ifstream vin(it->second);
      DrawingView v = DrawingViewFromJson(ParseProjectFile(it->second));
      maxId = std::max(maxId, v.id);
      dwg->GetViews().push_back(std::move(v));
    }

    std::map<uint64_t, fs::path> dimById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "dims", ec)) {
      if (entry.path().extension() == ".json")
        InsertIdFile(dimById, entry.path(), "dimension");
    }
    for (const auto& idJson : index.value("dims", json::array())) {
      const auto it = dimById.find(idJson.get<uint64_t>());
      if (it == dimById.end())
        continue;
      std::ifstream din(it->second);
      DrawingDim d = DrawingDimFromJson(ParseProjectFile(it->second));
      maxId = std::max(maxId, d.id);
      dwg->GetDims().push_back(std::move(d));
    }
    dwg->EnsureNextIdAtLeast(maxId + 1);
    LOG_INFO("Loaded drawing ({} views, {} dims) from {}", dwg->ViewCount(),
             dwg->DimCount(), dirPath);
    return dwg;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return nullptr;
  }
}

} // namespace

static bool SaveDrawingImpl(const DrawingDocument& dwg, const std::string& path,
                            std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveDrawingDir(dwg, normalized, error);
  }
  try {
    json j = DrawingEnvelope();
    j["sheet"] = SheetJson(dwg.GetSheet());
    j["views"] = json::array();
    for (const DrawingView& v : dwg.GetViews())
      j["views"].push_back(DrawingViewToJson(v));
    j["dims"] = json::array();
    for (const DrawingDim& d : dwg.GetDims())
      j["dims"].push_back(DrawingDimToJson(d));
    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved drawing ({} views) to {}", dwg.ViewCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save drawing to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<DrawingDocument> LoadDrawingImpl(const std::string& path,
                                                        std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDrawingDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);
    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "drawing") {
      if (error)
        *error = "Not a drawing: " + path;
      return nullptr;
    }
    auto dwg = std::make_unique<DrawingDocument>();
    dwg->GetSheet() = SheetFromJson(j.value("sheet", json::object()));
    uint64_t maxId = 0;
    for (const json& vj : j.value("views", json::array())) {
      DrawingView v = DrawingViewFromJson(vj);
      maxId = std::max(maxId, v.id);
      dwg->GetViews().push_back(std::move(v));
    }
    for (const json& dj : j.value("dims", json::array())) {
      DrawingDim d = DrawingDimFromJson(dj);
      maxId = std::max(maxId, d.id);
      dwg->GetDims().push_back(std::move(d));
    }
    dwg->EnsureNextIdAtLeast(maxId + 1);
    LOG_INFO("Loaded drawing ({} views, {} dims) from {}", dwg->ViewCount(),
             dwg->DimCount(), path);
    return dwg;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load drawing from {}: {}", path, e.what());
    return nullptr;
  }
}

// Public boundary: adapt the internal *Impl helpers (which keep the string
// out-param idiom) to std::expected so the error can't be discarded and a
// caller can't read a document off the failure path.
std::expected<void, std::string> Save(const Document& doc,
                                      const std::string& path) {
  std::string error;
  if (SaveImpl(doc, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<Document>, std::string>
Load(const std::string& path) {
  std::string error;
  if (auto doc = LoadImpl(path, &error))
    return doc;
  return std::unexpected(error);
}

std::string ProjectSaveTarget(const std::string& picked) {
  if (picked.empty())
    return picked;
  const std::filesystem::path p(picked);
  const std::string ext = p.extension().string();
  if (ext == ".dsgn")
    return picked;
  // ".json" is REPLACED (it is the flat form we are steering away from);
  // anything else is APPENDED to, so "hinge v2" keeps its dot and becomes
  // "hinge v2.dsgn" rather than losing the ".2".
  if (ext == ".json")
    return std::filesystem::path(picked).replace_extension(".dsgn").string();
  return picked + ".dsgn";
}

std::expected<void, std::string> SaveAssembly(const AssemblyDocument& asmDoc,
                                              const std::string& path) {
  std::string error;
  if (SaveAssemblyImpl(asmDoc, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<AssemblyDocument>, std::string>
LoadAssembly(const std::string& path) {
  std::string error;
  if (auto doc = LoadAssemblyImpl(path, &error))
    return doc;
  return std::unexpected(error);
}

std::expected<void, std::string> SaveDrawing(const DrawingDocument& drawing,
                                             const std::string& path) {
  std::string error;
  if (SaveDrawingImpl(drawing, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<DrawingDocument>, std::string>
LoadDrawing(const std::string& path) {
  std::string error;
  if (auto doc = LoadDrawingImpl(path, &error))
    return doc;
  return std::unexpected(error);
}

} // namespace DocumentIO
