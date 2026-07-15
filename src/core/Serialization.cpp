#include "core/Serialization.h"
#include "core/SerializationInternal.h"

#include "Logger.h"
#include "core/BooleanFeature.h"
#include "core/DatumAxisFeature.h"
#include "core/DatumPlaneFeature.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/DraftFeature.h"
#include "core/HoleFeature.h"
#include "core/LoftFeature.h"
#include "core/SweepFeature.h"
#include "core/Occurrence.h"
#include "core/PatternFeature.h"
#include "core/RevolveFeature.h"
#include "core/ShellFeature.h"
#include "core/SketchFeature.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using nlohmann::json;


namespace DocumentIO {


json OccurrenceToJson(const Occurrence& o) {
  json j;
  j["type"] = "Occurrence";
  j["id"] = o.id;
  j["name"] = o.name;
  j["part_id"] = o.partId;
  j["part_version"] = o.partVersion ? json(*o.partVersion) : json(nullptr);
  j["transform"] = {{"position", Vec3ToJson(o.position)},
                    {"orientation", QuatToJson(o.orientation)}};
  j["visible"] = o.visible;
  j["fixed"] = o.fixed;
  return j;
}

Occurrence OccurrenceFromJson(const json& j) {
  Occurrence o;
  o.id = j.at("id").get<uint64_t>();
  o.name = j.value("name", std::string{});
  o.partId = j.at("part_id").get<uint64_t>();
  if (j.contains("part_version") && !j.at("part_version").is_null())
    o.partVersion = j.at("part_version").get<std::string>();
  const json& t = j.at("transform");
  o.position = Vec3FromJson(t.at("position"));
  o.orientation = QuatFromJson(t.at("orientation"));
  o.visible = j.value("visible", true);
  o.fixed = j.value("fixed", false);
  return o;
}

// ---- Mates (A3) -----------------------------------------------------------

namespace {

const char* MateTypeToString(MateType t) {
  switch (t) {
    case MateType::Fastened: return "fastened";
    case MateType::Revolute: return "revolute";
    case MateType::Slider: return "slider";
    case MateType::Cylindrical: return "cylindrical";
    case MateType::Planar: return "planar";
    case MateType::Ball: return "ball";
  }
  return "fastened";
}

MateType MateTypeFromString(const std::string& s) {
  if (s == "revolute") return MateType::Revolute;
  if (s == "slider") return MateType::Slider;
  if (s == "cylindrical") return MateType::Cylindrical;
  if (s == "planar") return MateType::Planar;
  if (s == "ball") return MateType::Ball;
  return MateType::Fastened; // default / "fastened"
}

json ConnectorToJson(const Connector& c) {
  json j;
  j["occurrence"] = c.occurrenceId;
  if (c.anchor.origin) {
    j["anchor"] = {{"origin", true}};
  } else {
    json anchor = {{"face", {{"feature_id", c.anchor.face.featureId},
                             {"element", c.anchor.face.name}}}};
    if (c.anchor.snapEdge.IsSet()) // inference snap (v26); omit when unset
      anchor["snap_edge"] = {{"feature_id", c.anchor.snapEdge.featureId},
                             {"element", c.anchor.snapEdge.name}};
    j["anchor"] = std::move(anchor);
  }
  return j;
}

Connector ConnectorFromJson(const json& j) {
  Connector c;
  c.occurrenceId = j.at("occurrence").get<uint64_t>();
  const json& a = j.at("anchor");
  if (a.value("origin", false)) {
    c.anchor.origin = true;
  } else if (a.contains("face")) {
    const json& f = a.at("face");
    c.anchor.face = ElementRef{f.value("feature_id", uint64_t{0}),
                               f.value("element", std::string{})};
    if (a.contains("snap_edge")) {
      const json& s = a.at("snap_edge");
      c.anchor.snapEdge = ElementRef{s.value("feature_id", uint64_t{0}),
                                     s.value("element", std::string{})};
    }
  }
  return c;
}

} // namespace

json MateToJson(const Mate& m) {
  json j;
  j["type"] = "Mate";
  j["id"] = m.id;
  j["name"] = m.name;
  j["mate_type"] = MateTypeToString(m.type);
  j["a"] = ConnectorToJson(m.a);
  j["b"] = ConnectorToJson(m.b);
  j["flipped"] = m.flipped;
  j["rotation"] = m.rotation;
  j["offset"] = m.offset;
  // Additive DOF for the wider mate set (v25); omit when zero/identity so
  // fastened/revolute/slider files stay byte-identical to v24.
  if (m.dx != 0.0f) j["dx"] = m.dx;
  if (m.dy != 0.0f) j["dy"] = m.dy;
  if (m.ballRotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    j["ball_rotation"] = QuatToJson(m.ballRotation);
  return j;
}

Mate MateFromJson(const json& j) {
  Mate m;
  m.id = j.at("id").get<uint64_t>();
  m.name = j.value("name", std::string{});
  m.type = MateTypeFromString(j.value("mate_type", std::string{"fastened"}));
  m.a = ConnectorFromJson(j.at("a"));
  m.b = ConnectorFromJson(j.at("b"));
  m.flipped = j.value("flipped", true);
  m.rotation = j.value("rotation", 0.0f);
  m.offset = j.value("offset", 0.0f);
  m.dx = j.value("dx", 0.0f);
  m.dy = j.value("dy", 0.0f);
  if (j.contains("ball_rotation")) m.ballRotation = QuatFromJson(j.at("ball_rotation"));
  return m;
}

// ---- Exploded views (v27, A4) ---------------------------------------------

json ExplodedViewToJson(const ExplodedView& v) {
  json j;
  j["type"] = "ExplodedView";
  j["id"] = v.id;
  j["name"] = v.name;
  j["steps"] = json::array();
  for (const ExplodeStep& s : v.steps) {
    j["steps"].push_back(
        {{"occurrence", s.occurrenceId},
         {"offset", {s.offset.x, s.offset.y, s.offset.z}}});
  }
  return j;
}

ExplodedView ExplodedViewFromJson(const json& j) {
  ExplodedView v;
  v.id = j.at("id").get<uint64_t>();
  v.name = j.value("name", std::string{});
  for (const json& sj : j.value("steps", json::array())) {
    ExplodeStep s;
    s.occurrenceId = sj.at("occurrence").get<uint64_t>();
    const auto& o = sj.at("offset");
    s.offset = glm::vec3(o.at(0).get<float>(), o.at(1).get<float>(),
                         o.at(2).get<float>());
    v.steps.push_back(std::move(s));
  }
  return v;
}

// ---- Configurations (v31) -------------------------------------------------
// A configuration persists like a study: tagged JSON, only-non-empty fields.
// Which configuration is active lives in the document envelope (see
// DocumentEnvelope), the rollback-bar pattern.

json ConfigurationToJson(const Configuration& c) {
  json j;
  j["type"] = "Configuration";
  j["id"] = c.id;
  j["name"] = c.name;
  if (!c.parameterOverrides.empty()) {
    j["parameter_overrides"] = json::array();
    for (const auto& [name, expr] : c.parameterOverrides)
      j["parameter_overrides"].push_back({{"name", name}, {"expression", expr}});
  }
  if (!c.suppressedFeatures.empty())
    j["suppressed_features"] = c.suppressedFeatures;
  return j;
}

Configuration ConfigurationFromJson(const json& j) {
  Configuration c;
  c.id = j.at("id").get<uint64_t>();
  c.name = j.value("name", std::string{});
  for (const json& oj : j.value("parameter_overrides", json::array()))
    c.parameterOverrides.emplace_back(oj.value("name", std::string{}),
                                      oj.value("expression", std::string{}));
  c.suppressedFeatures =
      j.value("suppressed_features", std::vector<uint64_t>{});
  return c;
}

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
  if (p.filename() == "index.json" && p.parent_path().string().ends_with(".dsgn"))
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
  while (!out.empty() && out.back() == '-') out.pop_back();
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
      if (error) *error = "Cannot open file for writing: " + tmp.string();
      return false;
    }
    out << text;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    if (error) *error = "rename failed: " + ec.message();
    return false;
  }
  return true;
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
        const std::string file =
            std::to_string(s.id) + "-" + LabelSlug(s.name) + ".json";
        if (!WriteTextAtomic(studyDir / file, StudyToJson(s).dump(2) + "\n", error))
          return false;
        index["studies"].push_back(s.id);
        wantedStudies.insert(file);
      }
      for (const auto& entry : fs::directory_iterator(studyDir, ec)) {
        if (entry.path().extension() == ".json" &&
            !wantedStudies.contains(entry.path().filename().string()))
          fs::remove(entry.path(), ec);
      }
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
        if (!WriteTextAtomic(cfgDir / file, ConfigurationToJson(c).dump(2) + "\n",
                             error))
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
    // Parameters live in the index envelope (small, diff-friendly there).
    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(),
             dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

std::unique_ptr<Document> LoadDir(const std::string& dirPath,
                                  std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error) *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = json::parse(in);
    if (index.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error) *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "part") {
      if (error)
        *error = "This is a " + index.value("kind", "part") +
                 ", not a part: " + dirPath;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
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
      byId[std::stoull(entry.path().filename().string())] = entry.path();
    }
    for (const auto& idJson : index.at("features")) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing feature file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      doc->AddFeature(FeatureFromJson(json::parse(fin)));
    }
    // FEM studies (v29): id-prefix files under studies/, index order. A pre-v29
    // document has no `studies` key ⇒ the loop is empty.
    {
      std::map<uint64_t, fs::path> studyById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "studies", ec)) {
        if (entry.path().extension() != ".json") continue;
        studyById[std::stoull(entry.path().filename().string())] = entry.path();
      }
      for (const auto& idJson : index.value("studies", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = studyById.find(id);
        if (it == studyById.end()) {
          if (error) *error = "missing study file for id " + std::to_string(id);
          return nullptr;
        }
        std::ifstream sin(it->second);
        doc->RestoreStudy(StudyFromJson(json::parse(sin)));
      }
    }
    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json") continue;
        cfgById[std::stoull(entry.path().filename().string())] = entry.path();
      }
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        doc->RestoreConfiguration(ConfigurationFromJson(json::parse(cin)));
      }
    }
    doc->SetRollback(index.value("rollback", -1));
    // Apply the active configuration last so the final regeneration reflects it.
    doc->SetActiveConfiguration(index.value("active_configuration", uint64_t{0}));
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             dirPath);
    return doc;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
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
    if (!asmDoc.GetMates().empty()) fs::create_directories(mateDir);
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
      if (!WriteTextAtomic(viewDir / file,
                           ExplodedViewToJson(v).dump(2) + "\n", error))
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
    if (error) *error = e.what();
    return false;
  }
}

std::unique_ptr<AssemblyDocument> LoadAssemblyDir(const std::string& dirPath,
                                                  std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error) *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = json::parse(in);
    if (index.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error) *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "assembly") {
      if (error) *error = "Not an assembly: " + dirPath;
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
      byId[std::stoull(entry.path().filename().string())] = entry.path();
    }
    std::vector<json> occJsons;
    for (const auto& idJson : index.value("occurrences", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing occurrence file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      occJsons.push_back(json::parse(fin));
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
      mateById[std::stoull(entry.path().filename().string())] = entry.path();
    }
    std::vector<json> mateJsons;
    for (const auto& idJson : index.value("mates", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = mateById.find(id);
      if (it == mateById.end()) {
        if (error) *error = "missing mate file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      mateJsons.push_back(json::parse(fin));
    }
    LoadMates(*asmDoc, mateJsons);

    // Exploded views (v27): id-prefix files under exploded/, index order. A
    // pre-v27 assembly has no `exploded` key ⇒ the loop is empty.
    std::map<uint64_t, fs::path> viewById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "exploded", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      viewById[std::stoull(entry.path().filename().string())] = entry.path();
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
      viewJsons.push_back(json::parse(fin));
    }
    LoadExplodedViews(*asmDoc, viewJsons);

    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json") continue;
        cfgById[std::stoull(entry.path().filename().string())] = entry.path();
      }
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        asmDoc->RestoreConfiguration(ConfigurationFromJson(json::parse(cin)));
      }
    }
    asmDoc->SetActiveConfiguration(index.value("active_configuration", uint64_t{0}));

    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), dirPath);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to load assembly directory {}: {}", dirPath, e.what());
    return nullptr;
  }
}

} // namespace

bool Save(const Document& doc, const std::string& path, std::string* error) {
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
    // Configurations (v31), inline (the active id is in the envelope above).
    if (!doc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : doc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }

    std::ofstream out(path);
    if (!out) {
      if (error) *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to save document to {}: {}", path, e.what());
    return false;
  }
}

std::unique_ptr<Document> Load(const std::string& path, std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error) *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = json::parse(in);

    if (j.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "part") {
      if (error)
        *error = "This is a " + j.value("kind", "part") + ", not a part: " + path;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    // Parameters first: features' expressions evaluate against them on the
    // regeneration each AddFeature triggers.
    for (const auto& pj : j.value("parameters", json::array())) {
      doc->SetParameter(pj.at("name").get<std::string>(),
                        pj.at("expression").get<std::string>(),
                        pj.value("comment", std::string{}));
    }
    for (const auto& fj : j.at("features")) {
      doc->AddFeature(FeatureFromJson(fj));
    }
    for (const auto& sj : j.value("studies", json::array())) {  // v29
      doc->RestoreStudy(StudyFromJson(sj));
    }
    for (const auto& cj : j.value("configurations", json::array())) {  // v31
      doc->RestoreConfiguration(ConfigurationFromJson(cj));
    }
    doc->SetRollback(j.value("rollback", -1)); // v19; absent = end
    doc->SetActiveConfiguration(j.value("active_configuration", uint64_t{0}));
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             path);
    return doc;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to load document from {}: {}", path, e.what());
    return nullptr;
  }
}

bool SaveAssembly(const AssemblyDocument& asmDoc, const std::string& path,
                  std::string* error) {
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
    if (asmDoc.ActiveConfiguration() != 0)  // v31
      j["active_configuration"] = asmDoc.ActiveConfiguration();
    if (!asmDoc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : asmDoc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }
    std::ofstream out(path);
    if (!out) {
      if (error) *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved assembly ({} occurrences) to {}", asmDoc.OccurrenceCount(),
             path);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to save assembly to {}: {}", path, e.what());
    return false;
  }
}

std::unique_ptr<AssemblyDocument> LoadAssembly(const std::string& path,
                                               std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadAssemblyDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error) *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = json::parse(in);
    if (j.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error) *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "assembly") {
      if (error) *error = "Not an assembly: " + path;
      return nullptr;
    }
    auto asmDoc = std::make_unique<AssemblyDocument>();
    LoadOccurrences(*asmDoc, j.value("occurrences", json::array())
                                .get<std::vector<json>>());
    LoadMates(*asmDoc,
              j.value("mates", json::array()).get<std::vector<json>>());
    LoadExplodedViews(
        *asmDoc, j.value("exploded", json::array()).get<std::vector<json>>());
    for (const auto& cj : j.value("configurations", json::array()))  // v31
      asmDoc->RestoreConfiguration(ConfigurationFromJson(cj));
    asmDoc->SetActiveConfiguration(j.value("active_configuration", uint64_t{0}));
    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), path);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
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
  if (!t.project.empty()) tb["project"] = t.project;
  if (!t.document.empty()) tb["document"] = t.document;
  if (!t.author.empty()) tb["author"] = t.author;
  if (!t.date.empty()) tb["date"] = t.date;
  if (!t.notes.empty()) tb["notes"] = t.notes;
  json j = {{"size", SheetSizeToString(s.size)}, {"landscape", s.landscape}};
  if (!tb.empty()) j["title_block"] = std::move(tb);
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
  if (r.occurrenceId) j["occurrence"] = r.occurrenceId;
  if (r.edge.IsSet()) j["edge"] = ElementRefJson(r.edge);
  if (r.silFace.IsSet()) j["sil_face"] = ElementRefJson(r.silFace);
  return j;
}

ProjRef ProjRefFromJson(const json& j) {
  ProjRef r;
  r.viewId = j.value("view", uint64_t{0});
  r.occurrenceId = j.value("occurrence", uint64_t{0});
  r.end = j.value("end", 0);
  if (j.contains("edge")) r.edge = ElementRefFromJson(j.at("edge"));
  if (j.contains("sil_face")) r.silFace = ElementRefFromJson(j.at("sil_face"));
  return r;
}

json DrawingEnvelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "drawing";
  return j;
}

}  // namespace

json DrawingViewToJson(const DrawingView& v) {
  json j = {{"id", v.id},
            {"name", v.name},
            {"source", v.sourceDocId},
            {"orientation", ViewOrientationToString(v.orientation)},
            {"scale", v.scale},
            {"position", {v.position.x, v.position.y}},
            {"show_hidden", v.showHidden}};
  if (v.baseViewId) j["base_view"] = v.baseViewId;
  if (!v.scaleExpr.empty()) j["scale_expr"] = v.scaleExpr;
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
  for (const ProjRef& r : d.refs) refs.push_back(ProjRefJson(r));
  return json{{"id", d.id},
              {"kind", DrawingDimKindToString(d.kind)},
              {"view", d.viewId},
              {"refs", std::move(refs)},
              {"label_offset", {d.labelOffset.x, d.labelOffset.y}}};
}

DrawingDim DrawingDimFromJson(const json& j) {
  DrawingDim d;
  d.id = j.value("id", uint64_t{0});
  DrawingDimKindFromString(j.value("kind", std::string("distance")), d.kind);
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
    if (!dwg.GetViews().empty()) fs::create_directories(viewDir);
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
    if (!dwg.GetDims().empty()) fs::create_directories(dimDir);
    std::set<std::string> wantedDim;
    for (const DrawingDim& d : dwg.GetDims()) {
      const std::string file = std::to_string(d.id) + "-" +
                               DrawingDimKindToString(d.kind) + ".json";
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
    if (error) *error = e.what();
    return false;
  }
}

std::unique_ptr<DrawingDocument> LoadDrawingDir(const std::string& dirPath,
                                                std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error) *error = "Not a designer project directory: " + dirPath;
      return nullptr;
    }
    json index = json::parse(in);
    if (index.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error) *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "drawing") {
      if (error) *error = "Not a drawing: " + dirPath;
      return nullptr;
    }

    auto dwg = std::make_unique<DrawingDocument>();
    dwg->GetSheet() = SheetFromJson(index.value("sheet", json::object()));

    std::map<uint64_t, fs::path> viewById;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "views", ec)) {
      if (entry.path().extension() == ".json")
        viewById[std::stoull(entry.path().filename().string())] = entry.path();
    }
    uint64_t maxId = 0;
    for (const auto& idJson : index.value("views", json::array())) {
      const auto it = viewById.find(idJson.get<uint64_t>());
      if (it == viewById.end()) continue;
      std::ifstream vin(it->second);
      DrawingView v = DrawingViewFromJson(json::parse(vin));
      maxId = std::max(maxId, v.id);
      dwg->GetViews().push_back(std::move(v));
    }

    std::map<uint64_t, fs::path> dimById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "dims", ec)) {
      if (entry.path().extension() == ".json")
        dimById[std::stoull(entry.path().filename().string())] = entry.path();
    }
    for (const auto& idJson : index.value("dims", json::array())) {
      const auto it = dimById.find(idJson.get<uint64_t>());
      if (it == dimById.end()) continue;
      std::ifstream din(it->second);
      DrawingDim d = DrawingDimFromJson(json::parse(din));
      maxId = std::max(maxId, d.id);
      dwg->GetDims().push_back(std::move(d));
    }
    dwg->EnsureNextIdAtLeast(maxId + 1);
    LOG_INFO("Loaded drawing ({} views, {} dims) from {}", dwg->ViewCount(),
             dwg->DimCount(), dirPath);
    return dwg;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}

}  // namespace

bool SaveDrawing(const DrawingDocument& dwg, const std::string& path,
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
      if (error) *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved drawing ({} views) to {}", dwg.ViewCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to save drawing to {}: {}", path, e.what());
    return false;
  }
}

std::unique_ptr<DrawingDocument> LoadDrawing(const std::string& path,
                                             std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDrawingDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error) *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = json::parse(in);
    if (j.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error) *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "drawing") {
      if (error) *error = "Not a drawing: " + path;
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
    if (error) *error = e.what();
    LOG_ERROR("Failed to load drawing from {}: {}", path, e.what());
    return nullptr;
  }
}

}  // namespace DocumentIO
