#pragma once

// FEM study model: a document-level analysis definition (NOT a Feature). A
// Study consumes the finished bodies and produces no geometry, so it lives on
// Document as a sibling to the parameter table rather than in the regeneration
// history. Mesh and results are derived data, never serialized (see
// FemMesher.h / the FEM2 solver); only this configuration persists.

#include "core/Conventions.h"
#include "core/ElementMap.h"
#include "core/Expression.h"
#include "core/fem/FemMesher.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Document;

namespace fem {

// A boundary condition applied to one face (resolved through an ElementRef, the
// toponaming contract — a dangling ref is a study error, not a silent skip).
enum class LoadType {
  Fixed,    // zero all three DOF on the face's nodes
  Force,    // a total force vector (N) spread as uniform traction
  Pressure, // a scalar pressure (MPa) along the INWARD face normal: a
            // positive value pushes into the face
};

// A per-face mesh size override (mm). size follows the value+expr convention:
// `sizeExpr` empty means `size` is a literal, else it is the evaluated result.
struct Refinement {
  ElementRef face;
  double size = 0.0;
  std::string sizeExpr;
  uint64_t occurrenceId = 0; // assembly studies: whose face (0 on a part)
};

// Assembly studies (docs/simulation/README.md#assembly-studies): a study on
// an AssemblyDocument names OCCURRENCES, each in one role. Absent and rigid
// bracket the truth — a part left out is a lower bound on stiffness, the same
// part rigid an upper bound, and meshed the number between them.
enum class ParticipantRole {
  Meshed,  // a participant: meshed and solved, its own flexibility counts
  Rigid,   // a support: its contact patches are clamped on the parts it touches
  Ignored, // not in the analysis at all
  // Meshed AND glued (S2): one mesh spans it and the meshed occurrence where
  // they touch, so load crosses the joint and each keeps its own material.
  // Bonding is declared, not inferred — T-nuts, drivers and press fits are
  // not glue.
  Bonded,
};
struct Participant {
  uint64_t occurrenceId = 0; // a TOP-LEVEL occurrence of the assembly
  ParticipantRole role = ParticipantRole::Rigid;
  // Per-participant element size (mm; 0 = the study's global size): a 16 mm
  // panel and a 60 mm brace do not want one number. value+expr convention.
  double meshSize = 0.0;
  std::string meshSizeExpr;
};
const char* ParticipantRoleToString(ParticipantRole role);
[[nodiscard]] bool ParticipantRoleFromString(const std::string& s,
                                             ParticipantRole& out);

// One boundary condition. `magnitude` is N (Force) or MPa (Pressure), ignored
// for Fixed. `direction` is the force direction (Force only; need not be
// normalized — it is normalized at solve time). Magnitude follows the value+
// expr convention so it can read "bolt_preload * 1.5".
//
// On an assembly study every anchor is occurrence-qualified (`occurrenceId`,
// the job document's `{jobPartId, ElementRef}` shape), and a Fixed load may
// anchor on a CONTACT instead of a face: `contactWith` names the rigid
// occurrence, and the load clamps the patch the two share, re-derived every
// time the study meshes. The patch does not exist in either part, so there is
// no face to name; and an anchor that is re-derived survives a rebuild the
// way a mate on a frame does. `face` is empty for a contact anchor.
struct Load {
  uint64_t id = 0;
  LoadType type = LoadType::Fixed;
  ElementRef face;
  uint64_t occurrenceId = 0; // 0 on a part study
  uint64_t contactWith = 0; // Fixed only: clamped where this occurrence touches
  double magnitude = 0.0;
  std::string magnitudeExpr;
  // Down, for a load whose face could not be resolved. Every path that HAS a
  // face derives this from it (DefaultForceDirection): a force pushes into
  // the face you picked. "Down" comes from the world frame — a hand-written
  // axis here was once "down" in no coordinate system this app uses.
  glm::dvec3 direction = -conv::kWorldUp;
};

// What a study solves for.
enum class AnalysisType {
  Static,   // linear-static: stress + displacement under loads
  Modal,    // natural frequencies + mode shapes (no external loads)
  Topology, // generative: SIMP compliance minimization -> a density field
};

// A single-part analysis definition (linear static or modal).
struct Study {
  uint64_t id = 0;
  std::string name;
  AnalysisType type = AnalysisType::Static;

  // Global maximum element size (mm); 0 = auto from the body bounding box.
  double meshSize = 0.0;
  std::string meshSizeExpr;

  std::vector<Refinement> refinements;
  std::vector<Load> loads;

  // Assembly studies only (empty on a part study): who takes part, and how.
  // An occurrence not listed is ignored.
  std::vector<Participant> participants;
  const Participant* FindParticipant(uint64_t occurrenceId) const {
    for (const Participant& p : participants)
      if (p.occurrenceId == occurrenceId)
        return &p;
    return nullptr;
  }
  bool IsAssemblyStudy() const { return !participants.empty(); }

  // Static only: uniform gravity/self-weight acceleration (m/s^2 in world
  // axes; 0 = off). Needs the body's material density.
  glm::dvec3 gravity{0.0, 0.0, 0.0};

  // Modal only: how many lowest natural frequencies to compute.
  int numModes = 6;

  // Topology only. The optimizer removes material from the loaded body down to
  // `volumeFraction` of its volume while keeping it as stiff as possible under
  // the study's Force/Pressure loads (gravity is intentionally ignored — a
  // design-dependent load). `filterRadius` (mm; 0 = auto) sets the minimum
  // member size. `keepFaces` are frozen solid in addition to the auto-frozen
  // support/load skin (bolt bosses, bearing seats). volumeFraction/filterRadius
  // follow the value+expr convention.
  double volumeFraction = 0.3;
  std::string volumeFractionExpr;
  double filterRadius = 0.0;
  std::string filterRadiusExpr;
  int maxIterations = 60;
  std::vector<ElementRef> keepFaces;

  // Transient: the most recent mesh/solve error (dangling ref, no fixed
  // support, singular matrix, ...). Never serialized. Empty when healthy.
  std::string error;
};

const char* LoadTypeToString(LoadType type);
[[nodiscard]] bool LoadTypeFromString(const std::string& s, LoadType& out);
const char* AnalysisTypeToString(AnalysisType type);

// The CPU solver a study of this type runs, as study_status, study_results and
// the receipt name it: static and modal factor K directly (SimplicialLDLT;
// modal inverts it for the shift-invert eigen solve), topology iterates
// (conjugate gradients with an incomplete-Cholesky preconditioner).
const char* CpuSolverName(AnalysisType type);
[[nodiscard]] bool AnalysisTypeFromString(const std::string& s,
                                          AnalysisType& out);

// Evaluates every expression-driven field (mesh size, refinement sizes, load
// magnitudes) against `symbols`, writing results into the value fields. On a
// parse/unknown-name error, sets *error (naming the offending text) and returns
// false; the value fields keep their last good values. Lazy: called at mesh/
// solve time, not during regeneration (a Study is not visited by the feature
// loop, and results are never auto-solved).
bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols,
                        std::string* error);

// The face a load/refinement ref resolves to: a body (index into
// doc.GetBodies()) and a face index in that body (TopExp_Explorer order).
struct ResolvedFace {
  int bodyIndex = -1;
  int faceIndex = -1;
};

// Resolves `ref` against the document bodies exactly as ShellFeature/Measure
// do (body.createdBy == ref.featureId, then elementMap.Resolve). Returns false
// and sets *error (naming `what`) if the body or the element name is gone —
// the toponaming contract: a dangling ref is a first-class study error.
bool ResolveFace(const Document& doc, const ElementRef& ref,
                 const std::string& what, ResolvedFace& out,
                 std::string* error);

// The direction a force placed on `face` should push: INTO the part, i.e.
// the negated outward normal of that face. A force default is a direction
// with a unit attached, so it has to come from what it acts on rather than
// from a world literal (docs/ui/tool-sessions.md#defaults-and-refusals) — the
// GUI placement and add_load without an explicit `direction` both call this, so
// clicking and scripting cannot drift apart. nullopt when the ref does not
// resolve or the surface has no usable normal.
std::optional<glm::dvec3> DefaultForceDirection(const Document& doc,
                                                const ElementRef& face);

// One meshed body of a study.
struct StudyBodyMesh {
  int bodyIndex = -1;
  std::string bodyName;
  FemMesh mesh;
};

// Evaluates the study's expressions, resolves every ref, and meshes each body
// the study touches (any body referenced by a load or refinement), applying
// the global size and per-face refinement sizes. On any error (bad expression,
// dangling ref, mesher failure) sets study.error and *error and returns false.
// Pure meshing — no solve. Used by mesh_study (and, in FEM2, run_study).
bool MeshStudy(const Document& doc, Study& study,
               std::vector<StudyBodyMesh>& out, std::string* error);

// MeshStudy split at the thread boundary
// (docs/ui/long-operations.md#fem-meshing-and-solving): PlanStudyMesh does
// everything that needs the Document — expressions, refs, per-body settings —
// and DEEP-COPIES each shape, so MeshPlanned can run on a worker with nothing
// shared: Netgen attaches triangulation to the faces it meshes, which on the
// document's own shape would be a write under the renderer's reads. MeshStudy
// == PlanStudyMesh + MeshPlanned.
struct MeshBodyPlan {
  int bodyIndex = -1;
  std::string bodyName;
  TopoDS_Shape shape; // a copy, owned here
  MeshSettings settings;
};
struct MeshPlan {
  std::vector<MeshBodyPlan> bodies;
};
bool PlanStudyMesh(const Document& doc, Study& study, MeshPlan& out,
                   std::string* error);
bool MeshPlanned(const MeshPlan& plan, std::vector<StudyBodyMesh>& out,
                 std::string* error);

} // namespace fem
