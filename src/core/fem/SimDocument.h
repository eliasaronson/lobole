// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A SIMULATION: what is analysed, how it is meshed, and the studies asked of
// it (docs/simulation/README.md#the-simulation-document-core).
//
// Shaped like cam::JobDocument: it names its subject -- a part or an
// assembly -- by stable project-document id and owns no geometry. Studies
// have lived on the thing they analysed (Document::m_studies,
// AssemblyDocument::m_studies), built by two planners with different
// abilities. Here a part subject is an assembly of one meshed participant
// (one planner, SimStudy.h), every study of the document solves on one mesh
// unless it sets a size of its own, and the mesh is kept with the document.
//
// A project saves and opens these (simulations/<folder>/,
// core/io/SerializeSimulation.h) as tabs of their own (ui::DocKind::
// Simulation), SimMigration.h moves an older file's studies of a part or an
// assembly into them, and the GUI and the RPCs act on them
// (docs/scripting/rpc.md#simulations).

#include "core/ExprValue.h"
#include "core/Expression.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemStudy.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fem {

struct StudyRuntime;

// What a simulation analyses.
enum class SubjectKind { Part, Assembly };
const char* SubjectKindToString(SubjectKind kind);
inline constexpr const char* kSubjectKindNames = "part, assembly";
[[nodiscard]] bool SubjectKindFromString(const std::string& s,
                                         SubjectKind& out);

// A reference, never a copy (the JobPart contract): a model edit reaches the
// simulation instead of orphaning it.
struct SimSubject {
  SubjectKind kind = SubjectKind::Part;
  uint64_t docId = 0;
  // Which configuration of the subject is analysed: 0 = the active one. A
  // simulation has one; two configurations are two simulations.
  uint64_t configuration = 0;
  // Display only; the subject owns its name. Kept so a simulation whose
  // subject has been deleted can say what is missing.
  std::string name;
};

// The document's mesh (docs/simulation/README.md#one-mesh-shared): what every
// study solves on unless it sets a mesh size of its own. value+expr.
struct SimMesh {
  ExprValue size{0.0}; // mm; 0 = auto (each body's bounding diagonal / 15)
  std::vector<Refinement> refinements;
};

// One meshed body as the mesher made it, kept by the key of what it was made
// from (MeshKey: the shape and its settings). Shared, never copied: a mesh of
// a cabinet is tens of megabytes.
struct StoredMesh {
  std::shared_ptr<const FemMesh> mesh;
  std::string body; // the body's name, for the tree and the report
};
// The key a mesh is stored by: the shape as geometry (inputhash::ShapeDigest)
// and every setting the mesher reads.
uint64_t MeshKey(const TopoDS_Shape& shape, const MeshSettings& settings);

// What a simulation's documents share with its undo snapshots: results and
// meshes are derived, not document state, and a snapshot that copied a
// mesh would be the reason nobody could undo. The revision counter is here
// too, so an undo followed by a different edit never re-issues a stamp a
// result was solved at.
// (SimDocument.cpp: it holds StudyRuntime, which this header does not see.)
struct SimStore;

class SimDocument {
public:
  SimDocument();
  // A copy is an undo snapshot: it SHARES the results and meshes (SimStore).
  SimDocument(const SimDocument&) = default;
  SimDocument& operator=(const SimDocument&) = default;
  // A document of its own (a duplicated tab): the same settings, studies,
  // ids and revisions, with a store of its own holding copies of the
  // results and the same (immutable, shared) meshes. A copy would write its
  // results into the original's store.
  SimDocument Clone() const;

  const SimSubject& Subject() const { return m_subject; }
  // A new subject, configuration or name: every result reads stale.
  void SetSubject(SimSubject subject);

  // On an assembly subject, who takes part and how (the participants
  // table); empty on a part, whose bodies are the one meshed participant.
  const std::vector<Participant>& Roles() const { return m_roles; }
  void SetRoles(std::vector<Participant> roles);
  const Participant* FindRole(uint64_t occurrenceId) const;

  const SimMesh& Mesh() const { return m_mesh; }
  void SetMesh(SimMesh mesh);

  // ---- Studies ------------------------------------------------------------
  const std::vector<Study>& GetStudies() const { return m_studies; }
  const Study* FindStudy(uint64_t id) const;
  // A run's write access to a study's transient fields (its error, its
  // evaluated expressions); an edit is ReplaceStudy.
  Study* StudyForRun(uint64_t id);
  // Appends a study of `type` with a fresh id, named `name` or, with it
  // empty, by its kind numbered past the names it would repeat ("Static 1",
  // "Modal 2"): what the toolbar, the Simulation button and add_study all
  // name a new study. A buckling study takes its own default number of
  // factors.
  Study& AddStudy(const std::string& name = "",
                  AnalysisType type = AnalysisType::Static);
  bool RemoveStudy(uint64_t id);
  // Appends a fully formed study, keeping its ids (load, migration).
  void RestoreStudy(Study study);
  // Replaces the study with `study.id` in place; its results read stale.
  bool ReplaceStudy(Study study);

  // The study as it is planned: with the document's mesh size where it sets
  // none, the document's refinements before its own (none on a study of a
  // topology result, which meshes nothing), and the document's roles. What
  // the planner, the input hash and the mesh read.
  Study Effective(const Study& study) const;
  // Whether a study meshes on the document's mesh (no size or refinement of
  // its own).
  static bool SharesMesh(const Study& study) {
    return study.meshSize.value == 0.0 && study.meshSize.expr.empty() &&
           study.refinements.empty();
  }

  // ---- Results ------------------------------------------------------------
  const StudyRuntime* StudyResults(uint64_t studyId) const;
  // Stores a result stamped with the study's stamp now (StudyStamp) and the
  // subject stamps it was solved against (rt.solvedPartVersions).
  void StoreStudyResults(uint64_t studyId, StudyRuntime rt);
  uint64_t StudyResultsEpoch() const;
  // What a result is stamped with: the study's own revision, the document's
  // settings revision, and for a study that reads another (a static study
  // of a topology shape, a thermal load from a thermal study) that one's.
  uint64_t StudyStamp(uint64_t studyId) const;
  // Stale when solved at another stamp, when a subject document it read has
  // moved (`stampOf` answers a document id with its stamp now), or when the
  // topology result a shape study was cut from is stale or another.
  bool StudyResultsStale(
      uint64_t studyId,
      const std::function<uint64_t(uint64_t docId)>& stampOf) const;
  // A result read back from a file or moved from a part or an assembly
  // (SimMigration.h): stored as it is, and stale until RestampSimByInputs
  // (SimStudy.h) finds its inputs hash as they did when it was solved.
  void RestoreStudyResults(uint64_t studyId, StudyRuntime rt);
  // Stamps a stored result current against these subject stamps: a reopened
  // project's whose inputs hash as they did. False when absent.
  bool
  MarkStudyResultsCurrent(uint64_t studyId,
                          std::vector<std::pair<uint64_t, uint64_t>> stamps);
  // Every stored result id.
  std::vector<uint64_t> ResultIds() const;
  void DropStudyResults(uint64_t studyId);

  // ---- Meshes -------------------------------------------------------------
  const StoredMesh* FindMesh(uint64_t key) const;
  void StoreMesh(uint64_t key, StoredMesh mesh);
  const std::map<uint64_t, StoredMesh>& Meshes() const;
  // The keys of the document's own mesh as it was last built (Mesh): what
  // the tree's Mesh row shows and what a save keeps beside every result's.
  const std::vector<uint64_t>& DocumentMeshKeys() const {
    return m_documentMeshKeys;
  }
  void SetDocumentMeshKeys(std::vector<uint64_t> keys) {
    m_documentMeshKeys = std::move(keys);
  }
  // Every mesh a save keeps: the document's, and each stored result's.
  std::vector<uint64_t> KeptMeshKeys() const;
  // Clear stored meshes and results (clear_sim_storage): everything is
  // rebuilt on the next run. Returns how many meshes and results went.
  std::pair<size_t, size_t> ClearStorage();

  // ---- Ids, versions, the save stamp ---------------------------------------
  // One never-reused counter for studies, loads and refinements.
  uint64_t AllocateId() { return m_nextId++; }
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId)
      m_nextId = next;
  }
  uint64_t NextId() const { return m_nextId; }
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }
  // Whether the simulation changed since it was saved: every edit bumps the
  // version, so the stamp is the version.
  uint64_t SaveStamp() const { return m_version; }

  // Every expression slot: the document's mesh and each study's, with who
  // holds it ("simulation mesh", "study 'Bracket'").
  void VisitExprs(const std::function<void(const std::string& owner,
                                           const ExprSlot&)>& visit);
  // A variable renamed: every slot that reads it. True when one changed.
  bool RenameReferences(const std::string& from, const std::string& to);
  // Who reads variable `name`, once each ("study 'Bracket'"): a variable is
  // not deleted while anything reads it (DeleteVariableCommand).
  std::vector<std::string> FindParameterReferences(const std::string& name);

private:
  uint64_t NewRevision() const;
  void Touch(uint64_t studyId);
  void TouchSettings();

  SimSubject m_subject;
  std::vector<Participant> m_roles;
  SimMesh m_mesh;
  std::vector<Study> m_studies;
  std::map<uint64_t, uint64_t> m_studyRevision;
  uint64_t m_settingsRevision = 0;
  std::vector<uint64_t> m_documentMeshKeys;
  std::shared_ptr<SimStore> m_store;
  uint64_t m_nextId = 1;
  uint64_t m_version = 0;
};

} // namespace fem
