// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The simulation document on disk (SerializeSimulation.h,
// docs/simulation/README.md#how-a-simulation-is-saved).

#include "core/io/SerializeSimulation.h"

#include "common/Logger.h"
#include "core/InputHash.h"
#include "core/fem/FemSolver.h" // StudyRuntime
#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using nlohmann::json;
namespace fs = std::filesystem;

namespace DocumentIO {

using namespace shared;

namespace {

constexpr const char* kResultsDir = "results";
constexpr const char* kMeshesDir = "meshes";

// "0x0123..." -> "0123...": a key as a file name.
std::string KeyFileStem(uint64_t key) { return inputhash::Hex(key).substr(2); }

uint64_t KeyFromHex(const json& v, const std::string& what) {
  uint64_t key = 0;
  if (!v.is_string() || !inputhash::FromHex(v.get<std::string>(), key) ||
      key == 0)
    throw std::runtime_error(what + " " + v.dump() +
                             " is not a mesh key (0x and 16 hex digits)");
  return key;
}

json KeysToJson(const std::vector<uint64_t>& keys) {
  json out = json::array();
  for (uint64_t k : keys)
    out.push_back(inputhash::Hex(k));
  return out;
}

std::vector<uint64_t> KeysFromJson(const json& j, const std::string& what) {
  if (!j.is_array())
    throw std::runtime_error(what + " is a list, not " + j.type_name());
  std::vector<uint64_t> keys;
  for (const json& k : j)
    keys.push_back(KeyFromHex(k, what));
  return keys;
}

json Envelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "simulation";
  return j;
}

// The geometry step (docs/simulation/README.md#the-geometry-step), vNEXT:
// absent when there is none.
json GeometryJson(const fem::SimGeometry& g) {
  json out = json::object();
  if (!g.suppressed.empty()) {
    out["suppressed"] = json::array();
    for (const fem::SuppressedFeature& f : g.suppressed) {
      json fj = {{"feature", f.feature}};
      if (f.document != 0)
        fj["document"] = f.document;
      out["suppressed"].push_back(std::move(fj));
    }
  }
  if (g.symmetry.On()) {
    json sj = json::object();
    if (g.symmetry.datum != 0)
      sj["datum"] = g.symmetry.datum;
    else
      sj["face"] = ElementRefJson(g.symmetry.face);
    if (g.symmetry.flip)
      sj["flip"] = true;
    out["symmetry"] = std::move(sj);
  }
  return out;
}

fem::SimGeometry GeometryFromJson(const json& j, fem::SubjectKind subject) {
  readctx::RefuseUnknownKeys(j, {"suppressed", "symmetry"},
                             "simulation geometry");
  fem::SimGeometry g;
  if (j.contains("suppressed")) {
    const json& list = j.at("suppressed");
    if (!list.is_array())
      throw std::runtime_error(std::string("geometry: `suppressed` is a list, "
                                           "not ") +
                               list.type_name());
    for (size_t i = 0; i < list.size(); ++i) {
      const json& fj = list[i];
      const std::string at = "geometry.suppressed[" + std::to_string(i) + "]";
      if (!fj.is_object())
        throw std::runtime_error(at + " is not an object");
      readctx::RefuseUnknownKeys(fj, {"feature", "document"}, at);
      if (!fj.contains("feature") || !readctx::IsWhole(fj.at("feature")) ||
          fj.at("feature").get<uint64_t>() == 0)
        throw std::runtime_error(at + ": `feature` is not a feature's id (a "
                                      "whole number above 0)");
      fem::SuppressedFeature f;
      f.feature = fj.at("feature").get<uint64_t>();
      if (fj.contains("document")) {
        if (!readctx::IsWhole(fj.at("document")) ||
            fj.at("document").get<uint64_t>() == 0)
          throw std::runtime_error(at + ": `document` is not a document's "
                                        "id (a whole number above 0)");
        f.document = fj.at("document").get<uint64_t>();
      }
      // A part's are its own; an assembly's name the part they are in.
      if (subject == fem::SubjectKind::Part && f.document != 0)
        throw std::runtime_error(at + ": a part's simulation suppresses its "
                                      "own features, with no `document`");
      if (subject == fem::SubjectKind::Assembly && f.document == 0)
        throw std::runtime_error(at + ": an assembly's simulation names the "
                                      "part (`document`) a feature is in");
      if (g.IsSuppressed(f.document, f.feature))
        throw std::runtime_error(at + ": feature " + std::to_string(f.feature) +
                                 " is listed twice");
      g.suppressed.push_back(f);
    }
  }
  if (j.contains("symmetry")) {
    const json& sj = j.at("symmetry");
    if (!sj.is_object())
      throw std::runtime_error("geometry: `symmetry` is not an object");
    readctx::RefuseUnknownKeys(sj, {"datum", "face", "flip"},
                               "geometry.symmetry");
    if (subject != fem::SubjectKind::Part)
      throw std::runtime_error("geometry.symmetry: a symmetry cut is a part "
                               "simulation's");
    if (sj.contains("datum") == sj.contains("face"))
      throw std::runtime_error("geometry.symmetry: names its plane by "
                               "exactly one of `datum` and `face`");
    if (sj.contains("datum")) {
      if (!readctx::IsWhole(sj.at("datum")) ||
          sj.at("datum").get<uint64_t>() == 0)
        throw std::runtime_error("geometry.symmetry: `datum` is not a "
                                 "feature's id");
      g.symmetry.datum = sj.at("datum").get<uint64_t>();
    } else {
      g.symmetry.face = ElementRefFromJson(sj.at("face"));
      if (!g.symmetry.face.IsSet())
        throw std::runtime_error("geometry.symmetry: `face` names no face");
    }
    if (sj.contains("flip")) {
      if (!sj.at("flip").is_boolean())
        throw std::runtime_error("geometry.symmetry: `flip` is true or "
                                 "false");
      g.symmetry.flip = sj.at("flip").get<bool>();
    }
  }
  return g;
}

// The document without its derived data: what both forms carry.
json BodyJson(const fem::SimDocument& sim) {
  json j = Envelope();
  const fem::SimSubject& subj = sim.Subject();
  json sj = {{"kind", fem::SubjectKindToString(subj.kind)},
             {"document", subj.docId},
             {"name", subj.name}};
  if (subj.configuration != 0)
    sj["configuration"] = subj.configuration;
  j["subject"] = std::move(sj);
  if (!sim.Roles().empty()) {
    j["roles"] = json::array();
    for (const fem::Participant& p : sim.Roles())
      j["roles"].push_back(ParticipantToJson(p));
  }
  json mj = json::object();
  WriteExpr(mj, "size", sim.Mesh().size);
  if (!sim.Mesh().refinements.empty()) {
    mj["refinements"] = json::array();
    for (const fem::Refinement& r : sim.Mesh().refinements)
      mj["refinements"].push_back(RefinementToJson(r));
  }
  j["mesh"] = std::move(mj);
  if (!sim.Geometry().Empty())
    j["geometry"] = GeometryJson(sim.Geometry());
  j["studies"] = json::array();
  for (const fem::Study& s : sim.GetStudies())
    j["studies"].push_back(StudyToJson(s));
  if (!sim.DocumentMeshKeys().empty())
    j["mesh_keys"] = KeysToJson(sim.DocumentMeshKeys());
  return j;
}

// A stored result as saved: StudyResultToJson's, with the meshes it was
// solved on. False when it holds nothing solved.
bool ResultJson(const fem::StudyRuntime& rt, json& out) {
  if (!StudyResultToJson(rt, out))
    return false;
  if (!rt.meshKeys.empty())
    out["mesh_keys"] = KeysToJson(rt.meshKeys);
  return true;
}

fem::StudyRuntime ResultFromJson(json j) {
  std::vector<uint64_t> keys;
  if (j.contains("mesh_keys")) {
    keys = KeysFromJson(j.at("mesh_keys"), "mesh_keys");
    j.erase("mesh_keys");
  }
  fem::StudyRuntime rt = StudyResultFromJson(j);
  rt.meshKeys = std::move(keys);
  return rt;
}

// A derived value inline in the single file: its bytes base64 under
// kResultData, checked against its `bin_bytes`.
json InlineBytes(json j) {
  if (j.contains(kResultData) && j.at(kResultData).is_binary())
    j[kResultData] = Base64Encode(j.at(kResultData).get_binary());
  return j;
}

json InlineBytesBack(json j) {
  if (!j.is_object())
    throw std::runtime_error(std::string("expected an object, got ") +
                             j.type_name());
  if (!j.contains(kResultData))
    return j;
  if (!j.at(kResultData).is_string())
    throw std::runtime_error("its data is not base64 text");
  std::vector<unsigned char> bytes =
      Base64Decode(j.at(kResultData).get<std::string>());
  if (j.contains("bin_bytes") &&
      (!readctx::IsWhole(j.at("bin_bytes")) ||
       j.at("bin_bytes").get<size_t>() != bytes.size()))
    throw std::runtime_error("its data is " + std::to_string(bytes.size()) +
                             " bytes (expected " + j.at("bin_bytes").dump() +
                             ")");
  j[kResultData] = json::binary(std::move(bytes));
  return j;
}

// Drops a derived item that did not read, naming it, as a study result that
// does not read is dropped: the study runs again.
// `rel` is its file (the directory form), `pointer` where it is in the
// file (the single file).
template <class F>
void OrDrop(const std::string& rel, const std::string& pointer, F&& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    const auto* named = dynamic_cast<const readctx::Error*>(&e);
    readctx::Dropped(pointer, named ? named->Reason() : e.what(), true, rel);
  }
}

// A stored mesh read back that nothing keeps (its result was dropped, or
// the document's mesh moved on): reported, since the next save leaves it out.
// `where` names it as the reader found it (a file, or a pointer in the
// single file).
void ReportUnkept(
    const fem::SimDocument& sim,
    const std::map<uint64_t, std::pair<std::string, std::string>>& where) {
  const std::vector<uint64_t> kept = sim.KeptMeshKeys();
  for (const auto& [key, at] : where)
    if (std::find(kept.begin(), kept.end(), key) == kept.end() &&
        sim.FindMesh(key) != nullptr)
      readctx::Dropped(at.second,
                       "mesh " + inputhash::Hex(key) +
                           " is read by no result and is not the document's "
                           "mesh, so it is not kept",
                       true, at.first);
}

void CheckSimEnvelope(const json& j) {
  CheckEnvelope(j, "simulation");
  readctx::RefuseUnknownKeys(j,
                             {"format", "version", "kind", "subject", "roles",
                              "mesh", "geometry", "studies", "mesh_keys",
                              "results", "meshes"},
                             "simulation");
  RequireLists(j, {{"studies", 223}});
  if (!j.contains("subject"))
    throw std::runtime_error("has no `subject`: a simulation names what it "
                             "analyses");
  if (!j.contains("mesh"))
    throw std::runtime_error("has no `mesh`, which every simulation writes");
}

fem::SimSubject SubjectFromJson(const json& sj) {
  readctx::RefuseUnknownKeys(sj, {"kind", "document", "configuration", "name"},
                             "simulation subject");
  fem::SimSubject subj;
  const std::string kind = sj.value("kind", std::string{});
  if (!fem::SubjectKindFromString(kind, subj.kind))
    throw std::runtime_error("subject: unknown kind '" + kind +
                             "' (expected one of: " + fem::kSubjectKindNames +
                             ")");
  if (!sj.contains("document") || !readctx::IsWhole(sj.at("document")) ||
      sj.at("document").get<uint64_t>() == 0)
    throw std::runtime_error("subject: `document` is not a document's id (a "
                             "whole number above 0)");
  subj.docId = sj.at("document").get<uint64_t>();
  if (sj.contains("configuration")) {
    if (!readctx::IsWhole(sj.at("configuration")))
      throw std::runtime_error("subject: `configuration` is not a whole "
                               "number");
    subj.configuration = sj.at("configuration").get<uint64_t>();
  }
  if (sj.contains("name") && !sj.at("name").is_string())
    throw std::runtime_error("subject: `name` is not text");
  subj.name = sj.value("name", std::string{});
  return subj;
}

// The document from its body (no derived data).
std::unique_ptr<fem::SimDocument> FromBodyJson(const json& j) {
  auto sim = std::make_unique<fem::SimDocument>();
  sim->SetSubject(SubjectFromJson(j.at("subject")));
  if (j.contains("roles")) {
    const json& roles = j.at("roles");
    if (!roles.is_array())
      throw std::runtime_error(std::string("`roles` is a list, not ") +
                               roles.type_name());
    if (sim->Subject().kind != fem::SubjectKind::Assembly && !roles.empty())
      throw std::runtime_error("roles: a part's simulation has none (its "
                               "bodies are the one meshed participant)");
    std::vector<fem::Participant> table;
    std::set<uint64_t> seen;
    for (size_t i = 0; i < roles.size(); ++i) {
      fem::Participant p =
          AtItem("roles", i, [&] { return ParticipantFromJson(roles[i]); });
      if (!seen.insert(p.occurrenceId).second)
        throw std::runtime_error(
            "roles[" + std::to_string(i) + "]: occurrence " +
            std::to_string(p.occurrenceId) + " is listed twice");
      table.push_back(std::move(p));
    }
    sim->SetRoles(std::move(table));
  }
  const json& mj = j.at("mesh");
  readctx::RefuseUnknownKeys(mj, {"size", "size_expr", "refinements"},
                             "simulation mesh");
  fem::SimMesh mesh;
  ReadExpr(mj, "size", mesh.size);
  if (mesh.size.value < 0.0 || !std::isfinite(mesh.size.value))
    throw std::runtime_error("mesh: size " + std::to_string(mesh.size.value) +
                             " mm is not 0 (auto) or above");
  const json refinements = mj.value("refinements", json::array());
  if (!refinements.is_array())
    throw std::runtime_error("mesh: `refinements` is a list");
  for (size_t i = 0; i < refinements.size(); ++i)
    mesh.refinements.push_back(AtItem("mesh.refinements", i, [&] {
      return RefinementFromJson(refinements[i]);
    }));
  sim->SetMesh(std::move(mesh));
  if (j.contains("geometry")) {
    const json& gj = j.at("geometry");
    if (!gj.is_object())
      throw std::runtime_error(std::string("`geometry` is an object, not ") +
                               gj.type_name());
    sim->SetGeometry(GeometryFromJson(gj, sim->Subject().kind));
  }
  const json studies = InlineList(j, "studies");
  for (size_t i = 0; i < studies.size(); ++i)
    sim->RestoreStudy(
        AtItem("studies", i, [&] { return StudyFromJson(studies[i]); }));
  if (j.contains("mesh_keys"))
    sim->SetDocumentMeshKeys(KeysFromJson(j.at("mesh_keys"), "mesh_keys"));
  return sim;
}

void AdoptResult(fem::SimDocument& sim, uint64_t studyId, json j) {
  if (sim.FindStudy(studyId) == nullptr)
    throw std::runtime_error("is the result of study " +
                             std::to_string(studyId) +
                             ", which the simulation does not have");
  sim.RestoreStudyResults(studyId, ResultFromJson(std::move(j)));
}

uint64_t StudyIdOfStem(const std::string& stem) {
  // "7.result"
  const std::string id = stem.substr(0, stem.find('.'));
  if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("is not named <study id>.result.json");
  return std::stoull(id);
}

bool SaveDirForm(const fem::SimDocument& sim, const fs::path& dir,
                 std::string* error) {
  fs::create_directories(dir);
  // The derived files first, the index last: a torn save leaves an index
  // that reads.
  const fs::path resultsDir = dir / kResultsDir;
  std::set<std::string> wantedResults;
  for (const uint64_t id : sim.ResultIds()) {
    const fem::StudyRuntime* rt = sim.StudyResults(id);
    json rj;
    if (rt == nullptr || sim.FindStudy(id) == nullptr || !ResultJson(*rt, rj))
      continue;
    fs::create_directories(resultsDir);
    if (!WriteResultFiles(resultsDir, std::to_string(id), std::move(rj),
                          wantedResults, error, ".result"))
      return false;
  }
  const fs::path meshesDir = dir / kMeshesDir;
  std::set<std::string> wantedMeshes;
  for (const uint64_t key : sim.KeptMeshKeys()) {
    const fem::StoredMesh* m = sim.FindMesh(key);
    if (m == nullptr || !m->mesh)
      continue;
    fs::create_directories(meshesDir);
    if (!WriteResultFiles(meshesDir, KeyFileStem(key),
                          StoredMeshToJson(key, *m), wantedMeshes, error,
                          ".mesh"))
      return false;
  }
  // What is no longer kept goes: a cleared storage leaves no file behind.
  std::error_code ec;
  for (const auto& [sub, wanted] : {std::pair{resultsDir, &wantedResults},
                                    std::pair{meshesDir, &wantedMeshes}}) {
    if (!fs::is_directory(sub, ec))
      continue;
    PruneCollection(sub, *wanted);
    if (fs::is_empty(sub, ec))
      fs::remove(sub, ec);
  }
  return WriteTextAtomic(dir / "index.json", BodyJson(sim).dump(2) + "\n",
                         error);
}

json SingleFileJson(const fem::SimDocument& sim) {
  json j = BodyJson(sim);
  json results = json::array();
  for (const uint64_t id : sim.ResultIds()) {
    const fem::StudyRuntime* rt = sim.StudyResults(id);
    json rj;
    if (rt == nullptr || sim.FindStudy(id) == nullptr || !ResultJson(*rt, rj))
      continue;
    rj["study"] = id;
    results.push_back(InlineBytes(std::move(rj)));
  }
  if (!results.empty())
    j["results"] = std::move(results);
  json meshes = json::array();
  for (const uint64_t key : sim.KeptMeshKeys())
    if (const fem::StoredMesh* m = sim.FindMesh(key); m && m->mesh)
      meshes.push_back(InlineBytes(StoredMeshToJson(key, *m)));
  if (!meshes.empty())
    j["meshes"] = std::move(meshes);
  return j;
}

void ReadInlineDerived(fem::SimDocument& sim, const json& j,
                       const std::string& file) {
  const json results = j.value("results", json::array());
  if (!results.is_array())
    throw std::runtime_error("`results` is a list");
  for (size_t i = 0; i < results.size(); ++i)
    OrDrop(file, "/results/" + std::to_string(i), [&] {
      json rj = InlineBytesBack(results[i]);
      if (!rj.contains("study") || !readctx::IsWhole(rj.at("study")))
        throw std::runtime_error("has no `study` id");
      const uint64_t id = rj.at("study").get<uint64_t>();
      rj.erase("study");
      AdoptResult(sim, id, std::move(rj));
    });
  const json meshes = j.value("meshes", json::array());
  if (!meshes.is_array())
    throw std::runtime_error("`meshes` is a list");
  std::map<uint64_t, std::pair<std::string, std::string>> where;
  for (size_t i = 0; i < meshes.size(); ++i)
    OrDrop(file, "/meshes/" + std::to_string(i), [&] {
      uint64_t key = 0;
      fem::StoredMesh m = StoredMeshFromJson(InlineBytesBack(meshes[i]), key);
      sim.StoreMesh(key, std::move(m));
      where[key] = {file, "/meshes/" + std::to_string(i)};
    });
  ReportUnkept(sim, where);
}

void ReadDirDerived(fem::SimDocument& sim, const fs::path& dir) {
  std::error_code ec;
  const auto files = [&](const char* sub, const char* suffix) {
    std::vector<std::string> rels;
    if (!fs::is_directory(dir / sub, ec))
      return rels;
    for (const auto& e : fs::directory_iterator(dir / sub, ec)) {
      const std::string name = e.path().filename().string();
      if (name.ends_with(suffix))
        rels.push_back(std::string(sub) + "/" + name);
    }
    std::sort(rels.begin(), rels.end());
    return rels;
  };
  for (const std::string& rel : files(kResultsDir, ".result.json")) {
    std::optional<json> j = ReadResultFiles(dir, rel);
    if (!j)
      continue;
    OrDrop(rel, "", [&] {
      AdoptResult(sim, StudyIdOfStem(fs::path(rel).filename().string()),
                  std::move(*j));
    });
  }
  std::map<uint64_t, std::pair<std::string, std::string>> where;
  for (const std::string& rel : files(kMeshesDir, ".mesh.json")) {
    std::optional<json> j = ReadResultFiles(dir, rel);
    if (!j)
      continue;
    OrDrop(rel, "", [&] {
      uint64_t key = 0;
      fem::StoredMesh m = StoredMeshFromJson(*j, key);
      const std::string stem = fs::path(rel).filename().string();
      if (stem.substr(0, stem.find('.')) != KeyFileStem(key))
        throw std::runtime_error("holds mesh " + inputhash::Hex(key) +
                                 ", not the one its name says");
      sim.StoreMesh(key, std::move(m));
      where[key] = {rel, ""};
    });
  }
  ReportUnkept(sim, where);
}

} // namespace

json StoredMeshToJson(uint64_t key, const fem::StoredMesh& stored) {
  const fem::FemMesh& m = *stored.mesh;
  ResultArrayWriter w;
  std::vector<double> nodes;
  nodes.reserve(m.nodes.size() * 3);
  for (const glm::dvec3& p : m.nodes)
    nodes.insert(nodes.end(), {p.x, p.y, p.z});
  std::vector<int> tets;
  tets.reserve(m.tets.size() * 10);
  for (const std::array<int, 10>& t : m.tets)
    tets.insert(tets.end(), t.begin(), t.end());
  std::vector<int> tris, faces;
  for (const fem::FemMesh::BoundaryTri& tr : m.boundary) {
    tris.insert(tris.end(), tr.nodes.begin(), tr.nodes.end());
    faces.push_back(tr.faceIndex);
  }
  json j = {{"key", inputhash::Hex(key)},
            {"body", stored.body},
            {"node_count", m.nodes.size()},
            {"tet_count", m.tets.size()},
            {"nodes", w.F64(nodes)},
            {"tets", w.Index(tets)},
            {"boundary", w.Index(tris)},
            {"boundary_face", w.Index(faces)},
            {"stats",
             {{"worst_quality", m.stats.worstQuality},
              {"straightened_nodes", m.stats.straightenedNodes},
              {"volume_mm3", m.stats.volumeMm3},
              {"shape_volume_mm3", m.stats.shapeVolumeMm3},
              {"shape_volume_error_mm3", m.stats.shapeVolumeErrorMm3}}}};
  if (!m.tetDomain.empty())
    j["tet_domain"] = w.Index(m.tetDomain);
  j[kResultData] = w.Take();
  j["bin_bytes"] = j[kResultData].get_binary().size();
  return j;
}

fem::StoredMesh StoredMeshFromJson(const json& j, uint64_t& key) {
  readctx::RefuseUnknownKeys(j,
                             {"key", "body", "node_count", "tet_count", "nodes",
                              "tets", "boundary", "boundary_face", "tet_domain",
                              "stats", kResultData, "bin_bytes"},
                             "stored mesh");
  if (!j.contains("key"))
    throw std::runtime_error("stored mesh: has no `key`");
  key = KeyFromHex(j.at("key"), "stored mesh: key");
  const ResultArrayReader bin(j, "stored mesh");
  auto mesh = std::make_shared<fem::FemMesh>();
  mesh->nodes = bin.Vec3s(j.at("nodes"), "nodes");
  const std::vector<int> tets = bin.Indices(j.at("tets"), "tets");
  const std::vector<int> tris = bin.Indices(j.at("boundary"), "boundary");
  const std::vector<int> faces =
      bin.Indices(j.at("boundary_face"), "boundary_face");
  if (tets.size() % 10 != 0 || tris.size() != faces.size() * 6)
    throw std::runtime_error("stored mesh: its tets or boundary are not whole "
                             "elements");
  const int nodeCount = static_cast<int>(mesh->nodes.size());
  const auto inRange = [&](const std::vector<int>& v) {
    return std::all_of(v.begin(), v.end(),
                       [&](int i) { return i >= 0 && i < nodeCount; });
  };
  if (!inRange(tets) || !inRange(tris))
    throw std::runtime_error("stored mesh: an element names a node past its " +
                             std::to_string(nodeCount));
  if (j.value("node_count", size_t{0}) != mesh->nodes.size() ||
      j.value("tet_count", size_t{0}) != tets.size() / 10)
    throw std::runtime_error("stored mesh: its counts disagree with its "
                             "arrays");
  for (size_t i = 0; i < tets.size(); i += 10) {
    std::array<int, 10> t{};
    std::copy(tets.begin() + i, tets.begin() + i + 10, t.begin());
    mesh->tets.push_back(t);
  }
  for (size_t i = 0; i < faces.size(); ++i) {
    fem::FemMesh::BoundaryTri tr{};
    std::copy(tris.begin() + i * 6, tris.begin() + i * 6 + 6, tr.nodes.begin());
    tr.faceIndex = faces[i];
    mesh->boundary.push_back(tr);
  }
  if (j.contains("tet_domain")) {
    mesh->tetDomain = bin.Indices(j.at("tet_domain"), "tet_domain");
    if (mesh->tetDomain.size() != mesh->tets.size())
      throw std::runtime_error("stored mesh: tet_domain is not one a tet");
  }
  mesh->stats.nodeCount = mesh->nodes.size();
  mesh->stats.tetCount = mesh->tets.size();
  if (j.contains("stats")) {
    const json& s = j.at("stats");
    readctx::RefuseUnknownKeys(s,
                               {"worst_quality", "straightened_nodes",
                                "volume_mm3", "shape_volume_mm3",
                                "shape_volume_error_mm3"},
                               "stored mesh stats");
    mesh->stats.worstQuality = s.value("worst_quality", 0.0);
    mesh->stats.straightenedNodes = s.value("straightened_nodes", size_t{0});
    mesh->stats.volumeMm3 = s.value("volume_mm3", 0.0);
    mesh->stats.shapeVolumeMm3 = s.value("shape_volume_mm3", 0.0);
    mesh->stats.shapeVolumeErrorMm3 = s.value("shape_volume_error_mm3", 0.0);
  }
  return {std::move(mesh), j.value("body", std::string{})};
}

std::string SimulationFingerprint(const fem::SimDocument& sim) {
  return BodyJson(sim).dump();
}

std::expected<void, std::string> SaveSimulation(const fem::SimDocument& sim,
                                                const std::string& path) {
  const std::string normalized = NormalizeProjectPath(path);
  std::string error;
  try {
    const bool ok =
        IsDirFormatPath(normalized)
            ? SaveDirForm(sim, fs::path(normalized), &error)
            : WriteTextAtomic(fs::path(normalized),
                              SingleFileJson(sim).dump(2) + "\n", &error);
    if (ok) {
      LOG_INFO("Saved simulation ({} studies, {} meshes) to {}",
               sim.GetStudies().size(), sim.KeptMeshKeys().size(), path);
      return {};
    }
  } catch (const std::exception& e) {
    error = e.what();
  }
  LOG_ERROR("Failed to save simulation to {}: {}", path, error);
  return std::unexpected(error);
}

std::expected<void, std::string> SaveSimulationBody(const fem::SimDocument& sim,
                                                    const std::string& path) {
  std::string error;
  try {
    if (WriteTextAtomic(fs::path(NormalizeProjectPath(path)),
                        BodyJson(sim).dump(2) + "\n", &error))
      return {};
  } catch (const std::exception& e) {
    error = e.what();
  }
  LOG_ERROR("Failed to save simulation to {}: {}", path, error);
  return std::unexpected(error);
}

std::expected<std::unique_ptr<fem::SimDocument>, std::string>
LoadSimulation(const std::string& path) {
  const std::string normalized = NormalizeProjectPath(path);
  const bool dirForm = IsDirFormatPath(normalized);
  const fs::path root(normalized);
  const fs::path file = dirForm ? root / "index.json" : fs::path(path);
  try {
    json j;
    auto sim = readctx::InFile(
        dirForm ? std::string("index.json") : file.filename().string(), [&] {
          std::error_code ec;
          if (!fs::is_regular_file(file, ec))
            throw std::runtime_error(
                dirForm ? "missing: this is not a Lobole document directory"
                        : "cannot be read");
          j = ParseFileText(file, dirForm ? &root : nullptr);
          CheckSimEnvelope(j);
          return FromBodyJson(j);
        });
    // The derived files name themselves (a dropped one is reported, never a
    // reason to refuse the document).
    if (dirForm)
      ReadDirDerived(*sim, root);
    else
      readctx::InFile(file.filename().string(), [&] {
        ReadInlineDerived(*sim, j, "");
        return 0;
      });
    return sim;
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to load simulation from {}: {}", file.string(), e.what());
    return std::unexpected(LoadError(e));
  }
}

} // namespace DocumentIO
