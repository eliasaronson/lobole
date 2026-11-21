// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The geometry step of a simulation (docs/simulation/README.md
// #the-geometry-step): the shape the analysis sees, which is the subject
// regenerated without the features suppressed for the analysis and cut on a
// symmetry plane, and the check of what in it is smaller than the elements
// it will be meshed with. The subject itself is never changed: the
// analysis part is a copy (fem::SimGeometry on the simulation names what
// is suppressed and where it is cut).

#include "core/ElementMap.h"
#include "core/fem/FemStudy.h"
#include "core/fem/SimDocument.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Document;
struct Body;

namespace fem {

struct StudyPlan;

// `part` regenerated with `features` suppressed (each by its stable id), as
// a copy: the part is not touched. It starts from the part's own bodies and
// regeneration cache (Document::CopyWithGeometry, AdoptEvaluations), so the
// features before the first suppressed one are not built again. Null with
// `error` when a feature is not the part's, or when suppressing them leaves
// a feature that built before failing, or no body at all.
std::shared_ptr<const Document> AnalysisPart(const Document& part,
                                             std::vector<uint64_t> features,
                                             std::string* error);

// The analysis parts made so far, by the part (its instance and version)
// and what is suppressed in it: what a simulation's context hands its
// planner, so a frame that plans twice regenerates once. A part edited moves
// its version and its entry is not found again. Bounded; thread-safe.
class AnalysisPartCache {
public:
  std::shared_ptr<const Document>
  Get(const Document& part, std::vector<uint64_t> features, std::string* error);

private:
  struct Entry {
    uint64_t instance = 0, version = 0, configuration = 0;
    std::vector<uint64_t> features;
    std::shared_ptr<const Document> doc;
    std::string error;
  };
  std::mutex m_mutex;
  std::vector<Entry> m_entries; // the most recent last
};

// The study as it reads the analysis part: a load's, a refinement's or a
// kept face's name with the steps the suppressed features added taken out
// (a face a suppressed cut went through is that face again without it).
// Refused, naming the feature, when a face is one a suppressed feature MADE
// (a hole's bore, a fillet's face), or a piece of a face one split; the same
// for the symmetry plane's face when `symmetry` is given. `part` names the
// features (the subject as it is, before the copy).
bool StudyForAnalysis(const Document& part, Study& study,
                      const std::vector<uint64_t>& suppressed,
                      std::string* error, SymmetryPlane* symmetry = nullptr);

// The symmetry plane in the part's own coordinates: a point on it and the
// normal pointing to the side kept.
struct CutPlane {
  glm::dvec3 origin{0.0};
  glm::dvec3 normal{0.0, 0.0, 1.0};
  std::string name; // "datum 'Mid'", "face '4/cap0'"
};
// Refused for a datum that is not a datum plane of `part`, or a face that
// does not resolve or is not flat.
bool ResolveSymmetryPlane(const Document& part, const SymmetryPlane& plane,
                          CutPlane& out, std::string* error);

// Cuts every planned body on `plane`, keeping the side its normal points
// to, and holds the cut with a roller (Load::symmetryCut): the half model of
// a part and load symmetric about the plane. Each load's face is mapped onto
// what is left of it -- a force is a total, so a face the plane cuts keeps
// the share of it its kept area carries; a face wholly on the other side
// takes none -- and a refinement follows its face. Refused, naming why: a
// study other than a static one (a half model finds only the modes, the
// buckling and the heat symmetric about the plane), a torque, a remote load,
// a pin, a contact or a load on part of a face (none is mirror-symmetric
// once cut), a static study of a topology shape, a bonded group, a body
// the plane does not halve (the part is not symmetric about it), or one the
// cut leaves nothing of.
bool CutPlanOnSymmetry(StudyPlan& plan, AnalysisType type,
                       const CutPlane& plane, std::string* error);

// What is smaller than the element size it will be meshed at
// (docs/simulation/README.md#the-geometry-check): an edge shorter than it, a
// face narrower than it (four times its area over its perimeter), and two
// faces that do not touch closer than it -- a wall (`throughMaterial`) or a
// gap -- each with the feature that made it, so a person or an agent knows
// what to suppress or refine before meshing.
struct GeometryFinding {
  enum class Kind { Edge, Face, Gap };
  Kind kind = Kind::Edge;
  std::string body;
  int index = -1;      // the edge's or face's index in the body
  int otherIndex = -1; // a gap's other face
  std::string element; // its stable name ("" when it has none)
  std::string otherElement;
  double sizeMm = 0.0;    // its length, width or distance
  double elementMm = 0.0; // the element size there
  uint64_t feature = 0;   // the feature that made it (0 when unknown)
  std::string featureName;
  bool suppressible = false;    // a feature that is not the part's first solid
  bool throughMaterial = false; // a gap: across material (a wall), not air
  glm::dvec3 at{0.0};           // where, in the part's coordinates
};
const char* GeometryFindingKindName(GeometryFinding::Kind kind);
struct GeometryCheck {
  struct Body {
    std::string name;
    double elementMm = 0.0;
    int faces = 0, edges = 0;
  };
  std::vector<Body> bodies;
  std::vector<GeometryFinding> findings;
  // Faces past this many pairs are not compared for gaps (named in `note`).
  bool gapsSkipped = false;
  std::string note;
};
// The check of one planned body: `shape` meshed at `settings`, its faces
// and edges named by `body`'s element map and their features by `part`.
// `keep`, when given, leaves out what lies wholly on the far side of a
// symmetry plane.
void CheckBodyGeometry(const Document& part, const ::Body& body,
                       const TopoDS_Shape& shape, const MeshSettings& settings,
                       const CutPlane* keep, GeometryCheck& out);

// What the geometry step is, hashed: mixed into a study's input hash so a
// change reads the result stale. 0 for an empty step, so a simulation with
// none hashes as before.
uint64_t GeometryHash(const SimGeometry& geometry);

} // namespace fem
