// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// FEM study model: a document-level analysis definition (NOT a Feature). A
// Study consumes the finished bodies and produces no geometry, so it lives on
// Document as a sibling to the parameter table rather than in the regeneration
// history. Mesh and results are derived data (FemMesher.h, FemSolver.h): a
// run's result is saved beside its study with the hash of its inputs, cut to
// what is read back (fem::KeepWhatIsRead), and this configuration is what
// produces it.

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
  Torque,   // a torque (N*mm) about an axis, as the traction
            // t = (T/J) a x (x - o), J = integral of r^2 dA over the face
  Remote,   // a force (N) acting at a point off the face, carried by the
            // face as F/A plus the linear traction that carries its moment
            // (docs/simulation/README.md#torque-and-remote-loads)
  Pin,      // a support on a cylindrical face: a rigid pin on its axis that
            // holds the face radially and axially where the part presses on
            // it (compression only), free to turn unless fixRotation
            // (docs/simulation/README.md#pin-supports)
};
// A support holds the part rather than loading it.
inline bool IsSupport(LoadType t) {
  return t == LoadType::Fixed || t == LoadType::Pin;
}

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

// Part of a load's face (docs/simulation/README.md#loads-on-part-of-a-face):
// the face's mesh facets whose centroids lie in a band between two planes
// across `normal` (from <= n.p <= to, n the unit normal, mm), or within
// `radius` of a point. A seat on a shaft, a drum's bearing on an arbor: a
// load where the face is not its own face. Whole = the face, as ever.
struct LoadRegion {
  enum class Kind { Whole, Band, Sphere };
  Kind kind = Kind::Whole;
  glm::dvec3 normal{0.0}; // Band: the planes' normal (need not be unit)
  double fromMm = 0.0, toMm = 0.0;
  glm::dvec3 center{0.0}; // Sphere
  double radiusMm = 0.0;
  bool IsWhole() const { return kind == Kind::Whole; }
  // Is `p` (mm, the part's frame) in the region? Always, for Whole.
  bool Contains(const glm::dvec3& p) const;
  // Empty when the region can be applied; otherwise why not (a zero
  // normal, an empty band, a radius that is not positive).
  std::string Why() const;
  bool operator==(const LoadRegion&) const = default;
};

// One boundary condition. `magnitude` is N (Force, Remote), MPa (Pressure) or
// N*mm (Torque), ignored for Fixed. `direction` is the force direction (Force
// and Remote; need not be normalized — it is normalized at solve time).
// Magnitude follows the value+ expr convention so it can read "bolt_preload
// * 1.5".
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
  // Torque only: the axis it turns about, a point on it and its direction
  // (mm; the part's coordinates, or the world's on an assembly study). A
  // zero direction means the face's own axis, which only a cylindrical face
  // has: anywhere else the axis is given or the load is refused. A positive
  // torque turns right-handed about the direction.
  glm::dvec3 axisOrigin{0.0};
  glm::dvec3 axisDirection{0.0};
  bool HasAxis() const { return glm::dot(axisDirection, axisDirection) > 0.0; }
  // Remote only: the point the force acts at (mm, same frame as the axis).
  // Required: a remote load without one is refused.
  glm::dvec3 point{0.0};
  bool hasPoint = false;
  // The load case this force or pressure belongs to, by name
  // (docs/topopt/README.md#load-cases). Empty on a study with no cases, and
  // always on a support: supports are shared by every case.
  std::string loadCase;
  // Part of the face only (v183); Whole, the face, as ever. A part study's.
  LoadRegion region;
  // Pin only: hold the tangential direction too, so the part cannot turn on
  // the pin (a keyed or pressed pin). The pin's axis is its face's, kept in
  // axisOrigin/axisDirection once CompleteLoadGeometry has run.
  bool fixRotation = false;
};

// One load case of a study (docs/topopt/README.md#load-cases): the force and
// pressure loads that name it act together, with every support. A topology
// study minimises the weighted sum of its cases' compliances; a static study
// solves each case on one factorisation. value+expr convention.
struct LoadCase {
  std::string name;
  double weight = 1.0;
  std::string weightExpr;
};

// A region a topology study must leave empty
// (docs/topopt/README.md#keep-outs): a body of the part, by name (another
// part's arrives as a Derived reference body), or, when `body` is empty, the
// box [boxMin, boxMax] in part coordinates (mm).
struct KeepOut {
  std::string body;
  glm::dvec3 boxMin{0.0};
  glm::dvec3 boxMax{0.0};
};

// Where a static study's mesh comes from
// (docs/topopt/README.md#a-static-study-of-the-optimised-shape): 0 = the body
// is meshed, as ever; otherwise the topology study `study`'s result, its tets
// at or above `threshold` (0 = the volume-matched level) that are connected
// to the supports, or every tet of its design domain when `whole`.
struct ShapeSource {
  uint64_t study = 0;
  double threshold = 0.0;
  bool whole = false;
};

// Whether a static run checks that its peak has settled
// (docs/simulation/README.md#a-peak-that-has-settled): Auto runs the
// refinement loop when the first solve took under the converge_auto_s
// preference and keeps refining while each step does; Always runs it
// whatever it costs; Never skips it, and the result says "not checked".
enum class ConvergeMode { Auto, Always, Never };
inline constexpr const char* kConvergeModeNames = "auto, always, never";
const char* ConvergeModeToString(ConvergeMode mode);
[[nodiscard]] bool ConvergeModeFromString(const std::string& s,
                                          ConvergeMode& out);

inline constexpr int kBucklingFactorsDefault = 3;

// What a study solves for.
enum class AnalysisType {
  Static,   // linear-static: stress + displacement under loads
  Modal,    // natural frequencies + mode shapes (no external loads)
  Topology, // generative: SIMP compliance minimization -> a density field
  // Linear buckling (docs/simulation/README.md#buckling): by what factor the
  // loads can grow before the part loses stability, and in what shape.
  Buckling,
};

// Which program solves a study (docs/simulation/README.md#calculix). Auto
// is the in-house solver for every analysis there is today; CalculiX solves
// a static study as a second, independent answer.
enum class Solver {
  Auto,
  InHouse,
  Calculix,
};
const char* SolverToString(Solver solver);
inline constexpr const char* kSolverNames = "auto, in_house, calculix";
[[nodiscard]] bool SolverFromString(const std::string& s, Solver& out);

// A static study solved past linear (docs/simulation/README.md
// #nonlinear-static), on CalculiX: large deformation (NLGEOM, the load
// stepped up in increments, a pressure following its face), and on request
// a snap-through run that seeds the shape with the first buckling mode and
// steps the load up until the part stops carrying more. Off = linear.
struct Nonlinear {
  bool largeDeformation = false;
  // The step's largest increment is its load over this many (0 = the
  // default: kNonlinearIncrements, kSnapIncrements on a snap-through run).
  int increments = 0;
  bool snapThrough = false;
  // Snap-through: the first mode's peak motion seeded into the shape (mm),
  // and the motion past which the part is taken to have failed (mm). Not
  // stated (`has*` false): derived from the part (a tenth of the wall the
  // mode bends, a tenth of the part's size). A stated 0 imperfection is the
  // perfect shape.
  double imperfectionMm = 0.0;
  bool hasImperfection = false;
  double deflectionLimitMm = 0.0;
  bool hasDeflectionLimit = false;
  bool On() const { return largeDeformation; }
  bool operator==(const Nonlinear&) const = default;
};
inline constexpr int kNonlinearIncrements = 10;
inline constexpr int kSnapIncrements = 20;

// A single-part analysis definition (linear static or modal).
struct Study {
  uint64_t id = 0;
  std::string name;
  AnalysisType type = AnalysisType::Static;
  // Absent from the file when Auto, so every study saved before it hashes
  // and re-saves as it did.
  Solver solver = Solver::Auto;

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

  // Modal: how many lowest natural frequencies to compute. Buckling: how
  // many factors (kBucklingFactorsDefault when not given: a symmetric
  // section has two equal first modes).
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
  std::vector<KeepOut> keepOuts;

  // Static and topology: the load cases (empty = every load is one case).
  std::vector<LoadCase> loadCases;
  // The index of the case named `name`, or -1.
  int FindLoadCase(const std::string& name) const {
    for (size_t i = 0; i < loadCases.size(); ++i)
      if (loadCases[i].name == name)
        return static_cast<int>(i);
    return -1;
  }

  // Static only: mesh a topology result instead of the body.
  ShapeSource shapeFrom;

  // Static only: whether a run checks its peak has settled (v206).
  ConvergeMode converge = ConvergeMode::Auto;

  // Static only: solved past linear, on CalculiX (v208).
  Nonlinear nonlinear;

  // Transient: the most recent mesh/solve error (dangling ref, no fixed
  // support, singular matrix, ...). Never serialized. Empty when healthy.
  std::string error;
};

// Empty when `solver` can solve a study of `type`; otherwise why not, naming
// what can: CalculiX solves static and buckling studies (modal and topology
// are in-house), and auto and in_house solve every type there is.
std::string SolverRefusal(Solver solver, AnalysisType type);
// The program that solves a study: its `solver`, except that a nonlinear
// study set to auto solves on CalculiX (the in-house solver is linear).
Solver EffectiveSolver(Solver solver, const Nonlinear& nonlinear);
// Empty when a study of `type` on `solver` with `loadCases` cases can take
// `nonlinear` with `converge`; otherwise why not, naming what can: a static
// study only, never in_house, a snap-through run needs large_deformation
// and one load set, increments are at least 1, a stated imperfection and
// deflection limit are not negative (the limit positive), and converge
// always is refused on a snap-through run, whose answer is a factor that
// refining round a stress peak does not settle.
std::string NonlinearRefusal(Solver solver, AnalysisType type,
                             const Nonlinear& nonlinear, size_t loadCases,
                             ConvergeMode converge);

const char* LoadTypeToString(LoadType type);
// Every load type's name, for a refusal that names what is accepted.
inline constexpr const char* kLoadTypeNames =
    "fixed, force, pressure, torque, remote, pin";
[[nodiscard]] bool LoadTypeFromString(const std::string& s, LoadType& out);
const char* AnalysisTypeToString(AnalysisType type);
// Every analysis type's name, for a refusal that names what is accepted.
inline constexpr const char* kAnalysisTypeNames =
    "static, modal, topology, buckling";

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

// The load cases' rules (docs/topopt/README.md#load-cases), checked before
// any meshing: names unique and not empty, weights positive, every force and
// pressure naming a case the study has, no support naming one, every case
// carrying a load; and none on a modal study, which has no loads to group.
// A study with no cases passes when no load names one. Expressions must have
// been evaluated.
bool ValidateLoadCases(const Study& study, std::string* error);

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

// An axis: a point on it and a unit direction (mm).
struct LoadAxis {
  glm::dvec3 origin{0.0};
  glm::dvec3 direction{0.0, 0.0, 1.0};
};
// The axis of a cylindrical face, its origin the face's centroid projected
// onto it and its direction signed so its largest component is positive (the
// surface's own sign is arbitrary). nullopt for any other face.
std::optional<LoadAxis> CylindricalFaceAxis(const TopoDS_Shape& face);
// The same for a face of a document, by ref; nullopt when the ref does not
// resolve or the face is not cylindrical.
std::optional<LoadAxis> CylindricalFaceAxis(const Document& doc,
                                            const ElementRef& face);
// The axis a torque load turns about: its own, else its face's. nullopt when
// it has none and its face is not cylindrical.
std::optional<LoadAxis> TorqueAxis(const Document& doc, const Load& load);
// Completes `load` against the face it acts on, before it is meshed: a
// torque without an axis takes the cylindrical face's, and is refused on any
// other face; a remote load without a point is refused; a pin takes its
// cylindrical face's axis and is refused on any other face. Every path that
// turns a Load into a traction calls this (part, assembly and topology
// studies), so a file and an RPC are refused the same way. Other load types
// pass unchanged.
bool CompleteLoadGeometry(const TopoDS_Shape& face, Load& load,
                          std::string* error);

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
