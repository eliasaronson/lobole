// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/SimDocument.h"

#include "core/InputHash.h"
#include "core/fem/FemSolver.h" // StudyRuntime, KeepWhatIsRead

#include <algorithm>
#include <set>

namespace fem {

struct SimStore {
  std::map<uint64_t, StudyRuntime> results; // by study id
  uint64_t epoch = 0;
  std::map<uint64_t, StoredMesh> meshes; // by MeshKey
  uint64_t nextRevision = 1;
};

const char* SubjectKindToString(SubjectKind kind) {
  return kind == SubjectKind::Assembly ? "assembly" : "part";
}

bool SubjectKindFromString(const std::string& s, SubjectKind& out) {
  if (s == "part") {
    out = SubjectKind::Part;
    return true;
  }
  if (s == "assembly") {
    out = SubjectKind::Assembly;
    return true;
  }
  return false;
}

uint64_t MeshKey(const TopoDS_Shape& shape, const MeshSettings& settings) {
  if (shape.IsNull())
    return 0;
  inputhash::Hasher h;
  h.Str("mesh/1");
  h.U64(inputhash::ShapeDigestOf(shape));
  h.F64(settings.maxSize);
  h.U64(settings.faceSizes.size());
  for (const auto& [face, size] : settings.faceSizes) {
    h.U64(static_cast<uint64_t>(face));
    h.F64(size);
  }
  const uint64_t d = h.Digest();
  return d == 0 ? 1 : d;
}

SimDocument::SimDocument() : m_store(std::make_shared<SimStore>()) {}

SimDocument SimDocument::Clone() const {
  SimDocument copy(*this);
  copy.m_store = std::make_shared<SimStore>(*m_store);
  return copy;
}

uint64_t SimDocument::NewRevision() const { return m_store->nextRevision++; }

void SimDocument::Touch(uint64_t studyId) {
  m_studyRevision[studyId] = NewRevision();
  m_version++;
}

void SimDocument::TouchSettings() {
  m_settingsRevision = NewRevision();
  m_version++;
}

void SimDocument::SetSubject(SimSubject subject) {
  m_subject = std::move(subject);
  TouchSettings();
}

void SimDocument::SetRoles(std::vector<Participant> roles) {
  m_roles = std::move(roles);
  TouchSettings();
}

const Participant* SimDocument::FindRole(uint64_t occurrenceId) const {
  for (const Participant& p : m_roles)
    if (p.occurrenceId == occurrenceId)
      return &p;
  return nullptr;
}

void SimDocument::SetMesh(SimMesh mesh) {
  m_mesh = std::move(mesh);
  TouchSettings();
}

const Study* SimDocument::FindStudy(uint64_t id) const {
  for (const Study& s : m_studies)
    if (s.id == id)
      return &s;
  return nullptr;
}

Study* SimDocument::StudyForRun(uint64_t id) {
  for (Study& s : m_studies)
    if (s.id == id)
      return &s;
  return nullptr;
}

Study& SimDocument::AddStudy(const std::string& name) {
  Study study;
  study.id = AllocateId();
  study.name =
      name.empty() ? ("Study " + std::to_string(m_studies.size() + 1)) : name;
  m_studies.push_back(std::move(study));
  Touch(m_studies.back().id);
  return m_studies.back();
}

bool SimDocument::RemoveStudy(uint64_t id) {
  const auto it = std::find_if(m_studies.begin(), m_studies.end(),
                               [id](const Study& s) { return s.id == id; });
  if (it == m_studies.end())
    return false;
  m_studies.erase(it);
  m_studyRevision.erase(id);
  m_version++;
  return true;
}

void SimDocument::RestoreStudy(Study study) {
  EnsureNextIdAtLeast(study.id + 1);
  for (const Load& l : study.loads)
    EnsureNextIdAtLeast(l.id + 1);
  m_studyRevision[study.id] = 0;
  m_studies.push_back(std::move(study));
}

bool SimDocument::ReplaceStudy(Study study) {
  for (Study& s : m_studies)
    if (s.id == study.id) {
      for (const Load& l : study.loads)
        EnsureNextIdAtLeast(l.id + 1);
      s = std::move(study);
      Touch(s.id);
      return true;
    }
  return false;
}

Study SimDocument::Effective(const Study& study) const {
  Study out = study;
  if (out.meshSize.value == 0.0 && out.meshSize.expr.empty())
    out.meshSize = m_mesh.size;
  // A study of a topology result meshes nothing (its mesh is cut from the
  // result), so the document's refinements are not its.
  if (!m_mesh.refinements.empty() && out.shapeFrom.study == 0) {
    std::vector<Refinement> all = m_mesh.refinements;
    all.insert(all.end(), out.refinements.begin(), out.refinements.end());
    out.refinements = std::move(all);
  }
  if (m_subject.kind == SubjectKind::Assembly)
    out.participants = m_roles;
  return out;
}

const StudyRuntime* SimDocument::StudyResults(uint64_t studyId) const {
  const auto it = m_store->results.find(studyId);
  return it == m_store->results.end() ? nullptr : &it->second;
}

void SimDocument::StoreStudyResults(uint64_t studyId, StudyRuntime rt) {
  KeepWhatIsRead(rt); // as it is read back and saved
  rt.solvedAtVersion = StudyStamp(studyId);
  m_store->results[studyId] = std::move(rt);
  ++m_store->epoch;
}

uint64_t SimDocument::StudyResultsEpoch() const { return m_store->epoch; }

uint64_t SimDocument::StudyStamp(uint64_t studyId) const {
  inputhash::Hasher h;
  h.U64(m_settingsRevision);
  const auto rev = [&](uint64_t id) {
    const auto it = m_studyRevision.find(id);
    return it == m_studyRevision.end() ? uint64_t{0} : it->second;
  };
  h.U64(rev(studyId));
  if (const Study* s = FindStudy(studyId)) {
    if (s->thermalLoad.kind == ThermalLoad::Kind::FromStudy)
      h.U64(rev(s->thermalLoad.fromStudy));
  }
  const uint64_t d = h.Digest();
  return d == 0 ? 1 : d;
}

bool SimDocument::StudyResultsStale(
    uint64_t studyId,
    const std::function<uint64_t(uint64_t docId)>& stampOf) const {
  const StudyRuntime* rt = StudyResults(studyId);
  if (!rt || rt->solvedAtVersion != StudyStamp(studyId))
    return true;
  for (const auto& [docId, stamp] : rt->solvedPartVersions)
    if (!stampOf || stampOf(docId) != stamp)
      return true;
  const Study* s = FindStudy(studyId);
  if (s == nullptr || s->shapeFrom.study == 0 || s->shapeFrom.study == studyId)
    return false;
  const StudyRuntime* src = StudyResults(s->shapeFrom.study);
  return src == nullptr || StudyResultsStale(s->shapeFrom.study, stampOf) ||
         src->densityDigest != rt->sourceDigest;
}

void SimDocument::RestoreStudyResults(uint64_t studyId, StudyRuntime rt) {
  rt.solvedAtVersion = 0; // no stamp is 0 (StudyStamp): stale until restamped
  m_store->results[studyId] = std::move(rt);
  ++m_store->epoch;
}

bool SimDocument::MarkStudyResultsCurrent(
    uint64_t studyId, std::vector<std::pair<uint64_t, uint64_t>> stamps) {
  const auto it = m_store->results.find(studyId);
  if (it == m_store->results.end())
    return false;
  it->second.solvedAtVersion = StudyStamp(studyId);
  it->second.solvedPartVersions = std::move(stamps);
  ++m_store->epoch;
  return true;
}

std::vector<uint64_t> SimDocument::ResultIds() const {
  std::vector<uint64_t> ids;
  for (const auto& [id, rt] : m_store->results)
    ids.push_back(id);
  return ids;
}

void SimDocument::DropStudyResults(uint64_t studyId) {
  if (m_store->results.erase(studyId) != 0)
    ++m_store->epoch;
}

const StoredMesh* SimDocument::FindMesh(uint64_t key) const {
  const auto it = m_store->meshes.find(key);
  return it == m_store->meshes.end() ? nullptr : &it->second;
}

void SimDocument::StoreMesh(uint64_t key, StoredMesh mesh) {
  if (key == 0 || !mesh.mesh)
    return;
  m_store->meshes[key] = std::move(mesh);
  ++m_store->epoch;
}

const std::map<uint64_t, StoredMesh>& SimDocument::Meshes() const {
  return m_store->meshes;
}

std::vector<uint64_t> SimDocument::KeptMeshKeys() const {
  std::set<uint64_t> keys(m_documentMeshKeys.begin(), m_documentMeshKeys.end());
  for (const auto& [id, rt] : m_store->results)
    keys.insert(rt.meshKeys.begin(), rt.meshKeys.end());
  std::vector<uint64_t> out;
  for (uint64_t k : keys)
    if (m_store->meshes.contains(k))
      out.push_back(k);
  return out;
}

std::pair<size_t, size_t> SimDocument::ClearStorage() {
  const std::pair<size_t, size_t> gone{m_store->meshes.size(),
                                       m_store->results.size()};
  m_store->meshes.clear();
  m_store->results.clear();
  m_documentMeshKeys.clear();
  ++m_store->epoch;
  m_version++; // the files go at the next save
  return gone;
}

void SimDocument::VisitExprs(
    const std::function<void(const std::string& owner, const ExprSlot&)>&
        visit) {
  const std::string meshOwner = "simulation mesh";
  visit(meshOwner, {m_mesh.size, ExprQuantity::Length, "mesh/size"});
  for (size_t i = 0; i < m_mesh.refinements.size(); ++i)
    visit(meshOwner, {m_mesh.refinements[i].size, ExprQuantity::Length,
                      "mesh/refinements/" + std::to_string(i) + "/size"});
  for (Study& study : m_studies) {
    const std::string owner = "study '" + study.name + "'";
    study.VisitExprs([&](const ExprSlot& slot) { visit(owner, slot); });
  }
  for (size_t i = 0; i < m_roles.size(); ++i)
    visit("simulation roles", {m_roles[i].meshSize, ExprQuantity::Length,
                               "roles/" + std::to_string(i) + "/mesh_size"});
}

bool SimDocument::RenameReferences(const std::string& from,
                                   const std::string& to) {
  bool changed = false;
  VisitExprs([&](const std::string&, const ExprSlot& slot) {
    changed |= RenameInSlot(slot, from, to);
  });
  if (changed)
    m_version++;
  return changed;
}

std::vector<std::string>
SimDocument::FindParameterReferences(const std::string& name) {
  std::vector<std::string> owners;
  VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    if (SlotReads(slot, name) &&
        std::find(owners.begin(), owners.end(), owner) == owners.end())
      owners.push_back(owner);
  });
  return owners;
}

} // namespace fem
