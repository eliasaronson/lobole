#pragma once

// FEM study model: a document-level analysis definition (NOT a Feature). A
// Study consumes the finished bodies and produces no geometry, so it lives on
// Document as a sibling to the parameter table rather than in the regeneration
// history. Mesh and results are derived data, never serialized (see
// FemMesher.h / the FEM2 solver); only this configuration persists.

#include "core/ElementMap.h"
#include "core/Expression.h"
#include "core/FemMesher.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Document;

namespace fem {

// A boundary condition applied to one face (resolved through an ElementRef, the
// toponaming contract — a dangling ref is a study error, not a silent skip).
enum class LoadType {
  Fixed,     // zero all three DOF on the face's nodes
  Force,     // a total force vector (N) spread as uniform traction
  Pressure,  // a scalar pressure (MPa) along the outward face normal
};

// A per-face mesh size override (mm). size follows the value+expr convention:
// `sizeExpr` empty means `size` is a literal, else it is the evaluated result.
struct Refinement {
  ElementRef face;
  double size = 0.0;
  std::string sizeExpr;
};

// One boundary condition. `magnitude` is N (Force) or MPa (Pressure), ignored
// for Fixed. `direction` is the force direction (Force only; need not be
// normalized — it is normalized at solve time). Magnitude follows the value+
// expr convention so it can read "bolt_preload * 1.5".
struct Load {
  uint64_t id = 0;
  LoadType type = LoadType::Fixed;
  ElementRef face;
  double magnitude = 0.0;
  std::string magnitudeExpr;
  glm::dvec3 direction{0.0, 0.0, -1.0};
};

// What a study solves for.
enum class AnalysisType {
  Static,    // linear-static: stress + displacement under loads
  Modal,     // natural frequencies + mode shapes (no external loads)
  Topology,  // generative: SIMP compliance minimization -> a density field
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
bool LoadTypeFromString(const std::string& s, LoadType& out);
const char* AnalysisTypeToString(AnalysisType type);
bool AnalysisTypeFromString(const std::string& s, AnalysisType& out);

// Evaluates every expression-driven field (mesh size, refinement sizes, load
// magnitudes) against `symbols`, writing results into the value fields. On a
// parse/unknown-name error, sets *error (naming the offending text) and returns
// false; the value fields keep their last good values. Lazy: called at mesh/
// solve time, not during regeneration (a Study is not visited by the feature
// loop, and results are never auto-solved).
bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols, std::string* error);

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
bool ResolveFace(const Document& doc, const ElementRef& ref, const std::string& what,
                 ResolvedFace& out, std::string* error);

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
bool MeshStudy(const Document& doc, Study& study, std::vector<StudyBodyMesh>& out,
               std::string* error);

}  // namespace fem
