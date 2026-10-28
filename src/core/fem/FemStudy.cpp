// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemStudy.h"
#include "core/ElementHistory.h"
#include "core/PrintedMass.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemTopo.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace fem {

const char* LoadTypeToString(LoadType type) {
  switch (type) {
  case LoadType::Fixed:
    return "fixed";
  case LoadType::Force:
    return "force";
  case LoadType::Pressure:
    return "pressure";
  case LoadType::Torque:
    return "torque";
  case LoadType::Remote:
    return "remote";
  case LoadType::Pin:
    return "pin";
  }
  return "fixed";
}

bool LoadTypeFromString(const std::string& s, LoadType& out) {
  if (s == "fixed") {
    out = LoadType::Fixed;
    return true;
  }
  if (s == "force") {
    out = LoadType::Force;
    return true;
  }
  if (s == "pressure") {
    out = LoadType::Pressure;
    return true;
  }
  if (s == "torque") {
    out = LoadType::Torque;
    return true;
  }
  if (s == "remote") {
    out = LoadType::Remote;
    return true;
  }
  if (s == "pin") {
    out = LoadType::Pin;
    return true;
  }
  return false;
}

const char* AnalysisTypeToString(AnalysisType type) {
  switch (type) {
  case AnalysisType::Modal:
    return "modal";
  case AnalysisType::Topology:
    return "topology";
  case AnalysisType::Static:
    break;
  }
  return "static";
}

const char* CpuSolverName(AnalysisType type) {
  switch (type) {
  case AnalysisType::Modal:
    return "cpu (Eigen SimplicialLDLT, shift-invert)";
  case AnalysisType::Topology:
    return "cpu (Eigen CG-IC)";
  case AnalysisType::Static:
    break;
  }
  return "cpu (Eigen SimplicialLDLT)";
}

bool AnalysisTypeFromString(const std::string& s, AnalysisType& out) {
  if (s == "static") {
    out = AnalysisType::Static;
    return true;
  }
  if (s == "modal") {
    out = AnalysisType::Modal;
    return true;
  }
  if (s == "topology") {
    out = AnalysisType::Topology;
    return true;
  }
  return false;
}

const char* ParticipantRoleToString(ParticipantRole role) {
  switch (role) {
  case ParticipantRole::Meshed:
    return "meshed";
  case ParticipantRole::Rigid:
    return "rigid";
  case ParticipantRole::Ignored:
    return "ignored";
  case ParticipantRole::Bonded:
    return "bonded";
  }
  return "rigid";
}

bool ParticipantRoleFromString(const std::string& s, ParticipantRole& out) {
  if (s == "meshed") {
    out = ParticipantRole::Meshed;
    return true;
  }
  if (s == "rigid") {
    out = ParticipantRole::Rigid;
    return true;
  }
  if (s == "ignored") {
    out = ParticipantRole::Ignored;
    return true;
  }
  if (s == "bonded") {
    out = ParticipantRole::Bonded;
    return true;
  }
  return false;
}

bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols,
                        std::string* error) {
  bool ok = true;
  // Evaluate one value+expr pair; on failure records the first error but keeps
  // going so every bad field is left with its last good value.
  auto eval = [&](const std::string& text, double& value,
                  const std::string& what) {
    if (text.empty())
      return;
    const auto v = ExpressionEngine::Default().EvaluateText(text, symbols);
    if (v.has_value()) {
      value = *v;
    } else if (ok) {
      ok = false;
      if (error)
        *error = what + " expression '" + text + "': " + v.error().message;
    }
  };

  eval(study.meshSizeExpr, study.meshSize, "mesh size");
  for (auto& r : study.refinements)
    eval(r.sizeExpr, r.size, "refinement size");
  for (auto& l : study.loads)
    eval(l.magnitudeExpr, l.magnitude, "load magnitude");
  for (auto& pt : study.participants)
    eval(pt.meshSizeExpr, pt.meshSize, "participant mesh size");
  for (auto& c : study.loadCases)
    eval(c.weightExpr, c.weight, "load case '" + c.name + "' weight");
  if (study.type == AnalysisType::Topology) {
    eval(study.volumeFractionExpr, study.volumeFraction, "volume fraction");
    eval(study.filterRadiusExpr, study.filterRadius, "filter radius");
  }
  return ok;
}

bool LoadRegion::Contains(const glm::dvec3& p) const {
  switch (kind) {
  case Kind::Whole:
    return true;
  case Kind::Band: {
    const double t = glm::dot(p, glm::normalize(normal));
    return t >= fromMm && t <= toMm;
  }
  case Kind::Sphere:
    return glm::length(p - center) <= radiusMm;
  }
  return true;
}

std::string LoadRegion::Why() const {
  if (kind == Kind::Band) {
    if (!(glm::length(normal) > 1e-12) || !std::isfinite(glm::length(normal)))
      return "a band region's normal is a non-zero [x, y, z]";
    if (!std::isfinite(fromMm) || !std::isfinite(toMm) || !(toMm > fromMm))
      return "a band region runs from from_mm to a larger to_mm (mm along "
             "its normal)";
  }
  if (kind == Kind::Sphere && (!(radiusMm > 0.0) || !std::isfinite(radiusMm) ||
                               !std::isfinite(glm::length(center))))
    return "a radius region needs center_mm [x, y, z] and a positive "
           "radius_mm";
  return {};
}

bool ValidateLoadCases(const Study& study, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  // A load on part of a face
  // (docs/simulation/README.md#loads-on-part-of-a-face): a static or modal
  // study of a part. A topology study's frozen skin is whole faces.
  for (const Load& l : study.loads) {
    if (l.region.IsWhole())
      continue;
    if (const std::string why = l.region.Why(); !why.empty())
      return fail("load " + std::to_string(l.id) + ": " + why);
    if (study.type == AnalysisType::Topology)
      return fail("load " + std::to_string(l.id) +
                  " acts on part of its face (region), and a topology "
                  "study freezes whole faces: load a face of its own");
    if (study.IsAssemblyStudy())
      return fail("load " + std::to_string(l.id) +
                  " acts on part of its face (region), which is a part "
                  "study's; on an assembly study load a whole face");
  }
  const auto known = [&] {
    std::string k;
    for (const LoadCase& c : study.loadCases)
      k += (k.empty() ? "" : ", ") + ("'" + c.name + "'");
    return k;
  };
  if (study.loadCases.empty()) {
    for (const Load& l : study.loads)
      if (!l.loadCase.empty())
        return fail("load " + std::to_string(l.id) + " names load case '" +
                    l.loadCase +
                    "', and the study has none (set_study load_cases)");
    return true;
  }
  if (study.type == AnalysisType::Modal)
    return fail("a modal study has no loads to put in load cases; load cases "
                "belong to static and topology studies");
  for (size_t i = 0; i < study.loadCases.size(); ++i) {
    const LoadCase& c = study.loadCases[i];
    if (c.name.empty())
      return fail("load case " + std::to_string(i + 1) + " has no name");
    for (size_t j = 0; j < i; ++j)
      if (study.loadCases[j].name == c.name)
        return fail("load case '" + c.name + "' is named twice");
    if (!(c.weight > 0.0))
      return fail("load case '" + c.name + "': weight " +
                  std::to_string(c.weight) + " is not positive");
  }
  std::vector<int> count(study.loadCases.size(), 0);
  for (const Load& l : study.loads) {
    if (IsSupport(l.type)) {
      if (!l.loadCase.empty())
        return fail("load " + std::to_string(l.id) +
                    " is a support in load case '" + l.loadCase +
                    "': supports hold in every case, so a support names none");
      continue;
    }
    const int k = study.FindLoadCase(l.loadCase);
    if (k < 0)
      return fail(l.loadCase.empty()
                      ? "load " + std::to_string(l.id) +
                            " is in no load case; the study's cases are " +
                            known()
                      : "load " + std::to_string(l.id) + " names load case '" +
                            l.loadCase + "'; the study's cases are " + known());
    ++count[k];
  }
  for (size_t i = 0; i < count.size(); ++i)
    if (count[i] == 0)
      return fail("load case '" + study.loadCases[i].name +
                  "' has no force or pressure in it");
  return true;
}

// featureId 0 = "whichever body has this element". An element name looks
// like it carries its feature ("3/face7"), but that prefix is the feature
// that CREATED the element, not the body's `created_by` — after one boolean
// the two differ, so a caller who reads the id off the name picks a body
// that does not exist. Searching is the honest resolution, and ambiguity is
// refused rather than guessed.
bool ResolveFace(const Document& doc, const ElementRef& ref,
                 const std::string& what, ResolvedFace& out,
                 std::string* error) {
  const auto& bodies = doc.GetBodies();
  std::vector<int> hits;
  for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
    if (ref.featureId != 0 && bodies[i].createdBy != ref.featureId)
      continue;
    if (const auto idx = bodies[i].elementMap.Resolve(ref.name)) {
      if (ref.featureId != 0) {
        out.bodyIndex = i;
        out.faceIndex = *idx;
        return true;
      }
      hits.push_back(i);
      out.bodyIndex = i;
      out.faceIndex = *idx;
    }
  }
  if (hits.size() == 1)
    return true;
  if (error) {
    if (hits.size() > 1) {
      *error = what + ": '" + ref.name + "' is on " +
               std::to_string(hits.size()) +
               " bodies — say which with feature_id (its created_by):";
      for (int i : hits)
        *error += " " + std::to_string(bodies[i].createdBy) + " (" +
                  bodies[i].name + ")";
    } else {
      *error = topo::UnresolvedReason(bodies, ref, topo::ElementKind::Face,
                                      what + ": face");
      if (ref.featureId != 0) {
        bool onAnother = false;
        for (const auto& b : bodies)
          if (b.elementMap.Resolve(ref.name))
            onAnother = true;
        if (onAnother)
          *error += " on the body created by feature " +
                    std::to_string(ref.featureId) +
                    " — it is on another body (drop feature_id and it will be "
                    "found, or read created_by from get_state)";
      }
    }
  }
  return false;
}

std::optional<glm::dvec3> DefaultForceDirection(const Document& doc,
                                                const ElementRef& face) {
  ResolvedFace resolved;
  if (!ResolveFace(doc, face, "force", resolved, nullptr))
    return std::nullopt;
  const auto info = Measure::DescribeFaceAt(doc.GetBodies()[resolved.bodyIndex],
                                            resolved.faceIndex);
  if (!info)
    return std::nullopt;
  const glm::dvec3 n = info->normal;
  if (glm::dot(n, n) < 1e-12)
    return std::nullopt;
  // Into the face: pushing on a face is what placing a force on it means.
  return -glm::normalize(n);
}

namespace {
// Face `index` of `shape` in TopExp_Explorer order, which is what a
// ResolvedFace counts (Measure's FaceAt).
TopoDS_Shape FaceByIndex(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), ++i)
    if (i == index)
      return ex.Current();
  return {};
}
} // namespace

std::optional<LoadAxis> CylindricalFaceAxis(const TopoDS_Shape& face) {
  if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
    return std::nullopt;
  BRepAdaptor_Surface surf(TopoDS::Face(face));
  if (surf.GetType() != GeomAbs_Cylinder)
    return std::nullopt;
  const gp_Ax1 ax = surf.Cylinder().Axis();
  glm::dvec3 d(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
  // The surface's own sign is arbitrary; the largest component positive
  // makes a shaft along Z turn about +Z whichever way it was modelled.
  int big = 0;
  for (int k = 1; k < 3; ++k)
    if (std::abs(d[k]) > std::abs(d[big]) + 1e-9)
      big = k;
  if (d[big] < 0)
    d = -d;
  const glm::dvec3 o(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  const gp_Pnt c = props.CentreOfMass();
  const glm::dvec3 cc(c.X(), c.Y(), c.Z());
  LoadAxis out;
  out.direction = glm::normalize(d);
  out.origin = o + glm::dot(cc - o, out.direction) * out.direction;
  return out;
}

std::optional<LoadAxis> CylindricalFaceAxis(const Document& doc,
                                            const ElementRef& face) {
  ResolvedFace rf;
  if (!ResolveFace(doc, face, "torque", rf, nullptr))
    return std::nullopt;
  return CylindricalFaceAxis(FaceByIndex(
      doc.GetBodies()[rf.bodyIndex].shape.GetShape(), rf.faceIndex));
}

std::optional<LoadAxis> TorqueAxis(const Document& doc, const Load& load) {
  if (load.HasAxis())
    return LoadAxis{load.axisOrigin, glm::normalize(load.axisDirection)};
  return CylindricalFaceAxis(doc, load.face);
}

bool CompleteLoadGeometry(const TopoDS_Shape& face, Load& load,
                          std::string* error) {
  if (load.type == LoadType::Torque && !load.HasAxis()) {
    const auto axis = CylindricalFaceAxis(face);
    if (!axis) {
      if (error)
        *error = "load " + std::to_string(load.id) +
                 ": a torque on a face that is not cylindrical needs an axis "
                 "(axis_origin_mm and axis_dir); a cylindrical face lends its "
                 "own";
      return false;
    }
    load.axisOrigin = axis->origin;
    load.axisDirection = axis->direction;
  }
  if (load.type == LoadType::Pin) {
    const auto axis = CylindricalFaceAxis(face);
    if (!axis) {
      if (error)
        *error = "load " + std::to_string(load.id) +
                 ": a pin support goes on a cylindrical face (a bore or a "
                 "shaft); this face is not one";
      return false;
    }
    load.axisOrigin = axis->origin;
    load.axisDirection = axis->direction;
  }
  if (load.type == LoadType::Remote && !load.hasPoint) {
    if (error)
      *error = "load " + std::to_string(load.id) +
               ": a remote load needs the point it acts at (point_mm)";
    return false;
  }
  return true;
}

FaceLoad MakeFaceLoad(const Load& load) {
  FaceLoad fl;
  fl.type = load.type;
  fl.magnitude = load.magnitude;
  fl.direction = load.direction;
  if (load.type == LoadType::Torque) {
    fl.origin = load.axisOrigin;
    fl.axis = load.axisDirection;
  } else if (load.type == LoadType::Remote) {
    fl.origin = load.point;
  }
  return fl;
}

bool PlanStudyMesh(const Document& doc, Study& study, MeshPlan& out,
                   std::string* error) {
  out = MeshPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);

  // Resolve every ref, grouping per body. Per-face refinement sizes are keyed
  // by resolved face index (what fem::Mesh's MeshSettings.faceSizes expects).
  std::map<int, MeshSettings> perBody; // bodyIndex -> settings
  auto touch = [&](int bodyIndex) -> MeshSettings& {
    MeshSettings& s = perBody[bodyIndex];
    s.maxSize = study.meshSize; // 0 => auto, per body
    return s;
  };

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    touch(rf.bodyIndex); // ensure the loaded body is meshed
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    touch(rf.bodyIndex)
        .faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  if (perBody.empty())
    return fail("study has no loads or refinements to mesh");

  const auto& bodies = doc.GetBodies();
  for (const auto& [bodyIndex, settings] : perBody) {
    if (bodies[bodyIndex].reference)
      return fail("body '" + bodies[bodyIndex].name +
                  "' is a derived reference (another part's geometry); set "
                  "reference: false on its Derived feature to analyse it");
    MeshBodyPlan b;
    b.bodyIndex = bodyIndex;
    b.bodyName = bodies[bodyIndex].name;
    b.shape = BRepBuilderAPI_Copy(bodies[bodyIndex].shape.GetShape()).Shape();
    b.settings = settings;
    out.bodies.push_back(std::move(b));
  }
  return true;
}

bool MeshPlanned(const MeshPlan& plan, std::vector<StudyBodyMesh>& out,
                 std::string* error) {
  out.clear();
  std::string err;
  for (const MeshBodyPlan& b : plan.bodies) {
    auto mesh = Mesh(b.shape, b.settings, &err);
    if (!mesh) {
      if (error)
        *error = "body '" + b.bodyName + "': " + err;
      return false;
    }
    StudyBodyMesh r;
    r.bodyIndex = b.bodyIndex;
    r.bodyName = b.bodyName;
    r.mesh = std::move(*mesh);
    out.push_back(std::move(r));
  }
  return true;
}

bool MeshStudy(const Document& doc, Study& study,
               std::vector<StudyBodyMesh>& out, std::string* error) {
  out.clear();
  MeshPlan plan;
  if (!PlanStudyMesh(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlanned(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

namespace {

// The mesh of a static study of a topology result (ShapeSource): the result's
// tets at or above the threshold, or all of them when `whole`, less what is
// not face-connected to a support. Fills bp.premeshed and the plan's report.
bool PlanShapeMesh(const Document& doc, const Study& study, int bodyIndex,
                   StudyBodyPlan& bp, StudyPlan& out, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const ShapeSource& src = study.shapeFrom;
  if (study.type != AnalysisType::Static)
    return fail("only a static study takes its shape from a topology result");
  const Study* topo = doc.FindStudy(src.study);
  if (topo == nullptr || topo->type != AnalysisType::Topology)
    return fail("shape_from names study " + std::to_string(src.study) +
                ", which is not a topology study of this part");
  const StudyRuntime* rt = doc.StudyResults(src.study);
  if (rt == nullptr || rt->bodies.empty())
    return fail("topology study '" + topo->name +
                "' has no result to take a shape from: run it first");
  if (doc.StudyResultsStale(src.study))
    return fail("topology study '" + topo->name +
                "' is stale: the part changed after it ran, so its shape is "
                "not this part's -- run it again first");
  const StudyBodyResult& tb = rt->bodies.front();
  if (tb.bodyIndex != bodyIndex)
    return fail("the loads are on body '" + doc.GetBodies()[bodyIndex].name +
                "', and topology study '" + topo->name + "' optimised '" +
                tb.bodyName + "'");
  if (!study.refinements.empty())
    return fail("a study of a topology result meshes nothing, so it takes no "
                "refinements: the mesh is the result's");
  const FemMesh& m = tb.mesh;
  const size_t n = m.tets.size();
  if (tb.density.size() != n)
    return fail("topology study '" + topo->name + "' has no density field");
  if (!src.whole && (src.threshold < 0.0 || src.threshold >= 1.0))
    return fail("shape threshold " + std::to_string(src.threshold) +
                " is not in [0, 1) (0 = the volume-matched level)");
  // The field thresholded is the density averaged onto the corners and back
  // onto each tet -- the field the iso-surface (the Shape view, the STL) is
  // drawn from. Per tet, a member one or two tets wide at its threshold is a
  // chain that meets itself at edges: a hinge to a static solve, which then
  // reports the swing of a loose sliver as the part's deflection.
  std::vector<double> nodeD(m.nodes.size(), 0.0);
  std::vector<int> nodeN(m.nodes.size(), 0);
  for (size_t e = 0; e < n; ++e)
    for (int i = 0; i < 4; ++i) {
      nodeD[m.tets[e][i]] += tb.density[e];
      ++nodeN[m.tets[e][i]];
    }
  std::vector<double> smooth(n, 0.0);
  for (size_t e = 0; e < n; ++e) {
    for (int i = 0; i < 4; ++i) {
      const int v = m.tets[e][i];
      smooth[e] += 0.25 * nodeD[v] / std::max(1, nodeN[v]);
    }
    // A frozen tet (a support, a load, a keep face) stays, whatever its
    // neighbours.
    if (tb.density[e] >= 1.0 - 1e-9)
      smooth[e] = 1.0;
  }
  const double thr =
      src.whole ? 0.0
      : src.threshold > 0.0
          ? src.threshold
          : AutoIsoThreshold(m, smooth, rt->volumeFractionAchieved);

  // The supports' tets, which the solid must reach, and the loaded faces,
  // which must reach a support through it.
  std::set<int> fixedFaces;
  std::map<int, uint64_t> loadedFaces; // face index -> a load on it
  for (const auto& [faceIndex, load] : bp.loads)
    if (IsSupport(load.type))
      fixedFaces.insert(faceIndex);
    else
      loadedFaces.emplace(faceIndex, load.id);
  std::vector<char> fixedNode(m.nodes.size(), 0);
  for (const FemMesh::BoundaryTri& t : m.boundary)
    if (fixedFaces.count(t.faceIndex))
      for (int v : t.nodes)
        fixedNode[v] = 1;
  std::vector<char> anchored(n, 0);
  for (size_t e = 0; e < n; ++e)
    for (int v : m.tets[e])
      anchored[e] |= fixedNode[v];
  // A loaded face's triangles, by the tet that owns each (the one that has
  // all three of its corners).
  std::vector<std::vector<int>> tetsOfNode(m.nodes.size());
  for (size_t e = 0; e < n; ++e)
    for (int i = 0; i < 4; ++i)
      tetsOfNode[m.tets[e][i]].push_back(static_cast<int>(e));
  std::map<int, std::vector<int>> loadedTets; // face index -> owner tets
  for (const FemMesh::BoundaryTri& t : m.boundary) {
    if (!loadedFaces.count(t.faceIndex))
      continue;
    for (int e : tetsOfNode[t.nodes[0]]) {
      int shared = 0;
      for (int i = 0; i < 4; ++i)
        shared += m.tets[e][i] == t.nodes[0] || m.tets[e][i] == t.nodes[1] ||
                  m.tets[e][i] == t.nodes[2];
      if (shared == 3)
        loadedTets[t.faceIndex].push_back(e);
    }
  }

  // The solid at `level`: what is at or above it and face-connected to a
  // support (a tet that meets the rest at an edge or a corner is a hinge,
  // and a solve hands back its free swing as the answer). Returns the first
  // loaded face it leaves out, or -1.
  std::vector<char> solid(n, 0);
  int dropped = 0;
  const auto cut = [&](double level) {
    for (size_t e = 0; e < n; ++e)
      solid[e] = src.whole || smooth[e] >= level;
    int count = 0;
    const std::vector<int> comp = TetFaceComponents(m, solid, &count);
    std::vector<char> reaches(count, 0);
    for (size_t e = 0; e < n; ++e)
      if (comp[e] >= 0 && anchored[e])
        reaches[comp[e]] = 1;
    dropped = 0;
    for (size_t e = 0; e < n; ++e)
      if (solid[e] && !reaches[comp[e]]) {
        solid[e] = 0;
        ++dropped;
      }
    for (const auto& [fi, tets] : loadedTets) {
      bool any = false;
      for (int e : tets)
        any = any || solid[e];
      if (!any)
        return fi;
    }
    return -1;
  };
  double level = thr;
  int lost = cut(level);
  if (lost >= 0 && !src.whole && src.threshold > 0.0)
    return fail("at threshold " + std::to_string(thr) + " the face of load " +
                std::to_string(loadedFaces[lost]) +
                " is not connected to a support through the shape (only "
                "through tets meeting at edges or corners); a lower "
                "threshold keeps more of it");
  if (lost >= 0 && !src.whole) {
    // The volume-matched shape does not carry every load to a support: the
    // highest level that does, found by bisection, and said
    // (StudyRuntime::shapeThresholdLowered).
    double lo = 0.0, hi = thr;
    if (cut(lo) >= 0)
      return fail("the face of load " + std::to_string(loadedFaces[lost]) +
                  " is not connected to a support through any of topology "
                  "study '" +
                  topo->name + "'s result");
    for (int it = 0; it < 24; ++it) {
      const double mid = 0.5 * (lo + hi);
      (cut(mid) < 0 ? lo : hi) = mid;
    }
    level = lo;
    cut(level);
    out.shapeThresholdLoweredFrom = thr;
  }
  auto mesh = std::make_shared<FemMesh>(ExtractSubMesh(m, solid));
  if (mesh->tets.empty())
    return fail("no tet of topology study '" + topo->name + "' at threshold " +
                std::to_string(thr) + " is connected to a support");
  out.sourceDigest = rt->densityDigest;
  out.shapeThreshold = level;
  out.shapeIslandTets = dropped;
  out.shapeVolume = TetSetVolume(m, solid);
  bp.premeshed = std::move(mesh);
  return true;
}

} // namespace

bool PlanStudy(const Document& doc, Study& study, StudyPlan& out,
               std::string* error) {
  out = StudyPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  if (!ValidateLoadCases(study, &err))
    return fail(err);
  out.type = study.type;
  out.numModes = study.numModes;
  out.gravity = study.gravity;
  for (const LoadCase& c : study.loadCases)
    out.caseNames.push_back(c.name);

  const auto& bodies = doc.GetBodies();

  // Group every load by the body it resolves to, and collect per-face
  // refinement sizes. A refinement alone does not make a body "loaded".
  struct BodyPlan {
    std::vector<std::pair<int, size_t>> loads; // (faceIndex, load index)
    MeshSettings settings;
  };
  std::map<int, BodyPlan> perBody;

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    BodyPlan& bp = perBody[rf.bodyIndex];
    bp.settings.maxSize = study.meshSize;
    bp.loads.push_back({rf.faceIndex, i});
  }
  if (perBody.empty())
    return fail("study has no loads to solve");

  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    // Only refine bodies that are actually solved.
    auto it = perBody.find(rf.bodyIndex);
    if (it != perBody.end())
      it->second.settings.faceSizes.push_back(
          {rf.faceIndex, study.refinements[i].size});
  }

  for (auto& [bodyIndex, plan] : perBody) {
    const Body& body = bodies[bodyIndex];

    // Material must have stiffness data (0 = unknown, like density).
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    if (mp.youngGPa <= 0.0f)
      return fail("body '" + body.name + "': material '" +
                  (body.material.empty() ? "default" : body.material) +
                  "' has no stiffness data");

    // Each loaded body needs at least one support.
    bool hasFixed = false;
    for (const auto& [faceIndex, loadIdx] : plan.loads) {
      const Load& l = study.loads[loadIdx];
      if (IsSupport(l.type))
        hasFixed = true;
      // A pin holds only where it is pressed, and a mode has no load to
      // press it (docs/simulation/README.md#pin-supports).
      if (l.type == LoadType::Pin && study.type == AnalysisType::Modal)
        return fail("load " + std::to_string(l.id) +
                    ": a modal study's supports are fixed; a pin holds where "
                    "a load presses the part onto it, and a mode has none");
    }
    if (!hasFixed)
      return fail("body '" + body.name + "' has loads but no fixed support");

    // Density-dependent analyses must fail HERE (prepare has the material
    // library and study), not on the worker.
    if (study.type == AnalysisType::Modal && mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for modal analysis)");
    if (study.type != AnalysisType::Modal &&
        (study.gravity.x != 0 || study.gravity.y != 0 ||
         study.gravity.z != 0) &&
        mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for gravity)");

    // GPa -> MPa; density kg/m^3 -> tonne/mm^3 for the consistent mm-N-t-s
    // system (so gravity loads and modal frequencies come out right).
    StudyBodyPlan bp;
    bp.bodyIndex = bodyIndex;
    bp.bodyName = body.name;
    bp.mat = Material{mp.youngGPa * 1000.0, mp.poisson, mp.density * 1e-12};
    // A PRINTED part weighs what its walls, shells and infill weigh
    // (docs/tolerances/README.md#printed-mass): its self-weight and modal
    // mass take the printed estimate's share of the solid, spread evenly.
    // The stiffness stays the solid material's.
    if (doc.PrintProcess()) {
      std::string why;
      if (const auto pm = printmass::EstimatePrinted(
              &*doc.PrintProcess(), doc.PrintSettings().For(body.createdBy),
              body, &why);
          pm && pm->solidMm3 > 0.0)
        bp.mat.densityTPerMM3 *= pm->effectiveMm3 / pm->solidMm3;
    }
    // A deep copy: the mesher writes triangulation into the faces it
    // meshes, and this shape may be meshed on a worker while the document's
    // own is being drawn.
    bp.settings = plan.settings;
    for (const auto& [faceIndex, loadIdx] : plan.loads) {
      Load l = study.loads[loadIdx];
      if (!CompleteLoadGeometry(FaceByIndex(body.shape.GetShape(), faceIndex),
                                l, &err))
        return fail(err);
      bp.loads.push_back({faceIndex, std::move(l)});
    }
    if (study.shapeFrom.study != 0) {
      // A static study of a topology result: its mesh is cut from the
      // result, nothing is meshed
      // (docs/topopt/README.md#a-static-study-of-the-optimised-shape).
      if (!PlanShapeMesh(doc, study, bodyIndex, bp, out, &err))
        return fail(err);
    } else {
      bp.shape = BRepBuilderAPI_Copy(body.shape.GetShape()).Shape();
    }
    out.bodies.push_back(std::move(bp));
  }
  return true;
}

bool MeshPlannedStudy(const StudyPlan& plan, StudyJobInput& out,
                      std::string* error) {
  out = StudyJobInput{};
  out.type = plan.type;
  out.numModes = plan.numModes;
  out.gravity = plan.gravity;
  out.caseNames = plan.caseNames;
  out.sourceDigest = plan.sourceDigest;
  out.shapeThreshold = plan.shapeThreshold;
  out.shapeIslandTets = plan.shapeIslandTets;
  out.shapeVolume = plan.shapeVolume;
  out.shapeThresholdLoweredFrom = plan.shapeThresholdLoweredFrom;
  std::string err;
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  for (const StudyBodyPlan& bp : plan.bodies) {
    auto mesh = bp.premeshed ? std::optional<FemMesh>(*bp.premeshed)
                             : Mesh(bp.shape, bp.settings, &err);
    if (!mesh)
      return fail("body '" + bp.bodyName + "': " + err);

    // A bonded group (S2): the mesh spans several solids, each its own
    // domain, each its own material. Which domain is which member is
    // answered by geometry — a tet's centroid is inside exactly one — not
    // by trusting the mesher's numbering.
    std::vector<Material> materials;
    if (!bp.members.empty()) {
      int domains = 0;
      for (int d : mesh->tetDomain)
        domains = std::max(domains, d + 1);
      materials.assign(std::max(domains, 1), bp.mat);
      std::vector<bool> known(materials.size(), false);
      for (size_t t = 0; t < mesh->tets.size(); ++t) {
        const int d = mesh->tetDomain.empty() ? 0 : mesh->tetDomain[t];
        if (known[d])
          continue;
        const auto& tet = mesh->tets[t];
        const glm::dvec3 c = 0.25 * (mesh->nodes[tet[0]] + mesh->nodes[tet[1]] +
                                     mesh->nodes[tet[2]] + mesh->nodes[tet[3]]);
        bool found = false;
        for (const StudyBodyPlan::Member& m : bp.members) {
          BRepClass3d_SolidClassifier cls(m.solid, gp_Pnt(c.x, c.y, c.z), 1e-6);
          if (cls.State() == TopAbs_IN) {
            materials[d] = m.mat;
            found = true;
            break;
          }
        }
        if (!found)
          return fail("body '" + bp.bodyName +
                      "': a mesh domain lies in none of the bonded members");
        known[d] = true;
      }
    }

    // Build fixed-node set and face tractions from the resolved faces. Fixed
    // faces contribute all their boundary-tri nodes; force/pressure faces
    // contribute their tri indices.
    std::set<int> fixedSet;
    std::vector<SupportFacets> supportFacets;
    std::vector<StudyJobBody::PinSupport> pins;
    std::vector<FaceLoad> faceLoads;
    std::vector<std::vector<FaceLoad>> caseLoads(plan.caseNames.size());
    // A torque or remote load on a face the imprint split arrives once per
    // piece: its pieces are one FaceLoad, so its field is the whole face's.
    std::map<uint64_t, std::pair<size_t, size_t>> fieldLoad; // id -> (case, i)
    // A facet is in a load's region when its centroid is (LoadRegion).
    const auto inRegion = [&](const FemMesh::BoundaryTri& tri,
                              const LoadRegion& region) {
      if (region.IsWhole())
        return true;
      glm::dvec3 c(0.0);
      for (int k = 0; k < 3; ++k)
        c += mesh->nodes[tri.nodes[k]];
      return region.Contains(c / 3.0);
    };
    for (const auto& [faceIndex, load] : bp.loads) {
      if (load.type == LoadType::Fixed) {
        size_t before = fixedSet.size();
        SupportFacets sf{load.id, load.type, {}};
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t) {
          const auto& tri = mesh->boundary[t];
          if (tri.faceIndex == faceIndex && inRegion(tri, load.region)) {
            sf.tris.push_back(t);
            for (int n : tri.nodes)
              fixedSet.insert(n);
          }
        }
        if (!load.region.IsWhole() && fixedSet.size() == before)
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s region holds none of its face's mesh facets");
        // A fixed support split by an imprint is one support.
        bool merged = false;
        for (SupportFacets& have : supportFacets)
          if (load.id != 0 && have.id == load.id) {
            have.tris.insert(have.tris.end(), sf.tris.begin(), sf.tris.end());
            merged = true;
          }
        if (!merged)
          supportFacets.push_back(std::move(sf));
      } else if (load.type == LoadType::Pin) {
        StudyJobBody::PinSupport pin;
        pin.id = load.id;
        pin.axisOrigin = load.axisOrigin;
        pin.axisDirection = load.axisDirection;
        pin.fixRotation = load.fixRotation;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex &&
              inRegion(mesh->boundary[t], load.region))
            pin.tris.push_back(t);
        if (pin.tris.empty())
          return fail("body '" + bp.bodyName + "': pin " +
                      std::to_string(load.id) +
                      (load.region.IsWhole()
                           ? "'s face meshed to no facets"
                           : "'s region holds none of its face's mesh facets"));
        supportFacets.push_back({load.id, load.type, pin.tris});
        pins.push_back(std::move(pin));
      } else {
        std::vector<int> tris;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex &&
              inRegion(mesh->boundary[t], load.region))
            tris.push_back(t);
        if (tris.empty() && !load.region.IsWhole())
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s region holds none of its face's mesh facets");
        if (tris.empty() && bp.premeshed)
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s face has no material left in the optimised shape");
        size_t k = 0;
        if (!plan.caseNames.empty()) {
          while (k < plan.caseNames.size() &&
                 plan.caseNames[k] != load.loadCase)
            ++k;
          if (k == plan.caseNames.size())
            return fail("load " + std::to_string(load.id) +
                        " names load case '" + load.loadCase +
                        "', which the study does not have");
        }
        std::vector<FaceLoad>& into =
            plan.caseNames.empty() ? faceLoads : caseLoads[k];
        const bool field =
            load.type == LoadType::Torque || load.type == LoadType::Remote;
        if (field && load.id != 0)
          if (auto it = fieldLoad.find(load.id); it != fieldLoad.end()) {
            std::vector<int>& more = into[it->second.second].tris;
            more.insert(more.end(), tris.begin(), tris.end());
            continue;
          }
        FaceLoad fl = MakeFaceLoad(load);
        fl.tris = std::move(tris);
        if (field && load.id != 0)
          fieldLoad[load.id] = {k, into.size()};
        into.push_back(std::move(fl));
      }
    }
    if (fixedSet.empty() && pins.empty())
      return fail("body '" + bp.bodyName + "': fixed face meshed to no nodes");

    StudyJobBody jb;
    jb.supports = std::move(supportFacets);
    jb.pins = std::move(pins);
    jb.bodyIndex = bp.bodyIndex;
    jb.bodyName = bp.bodyName;
    jb.mat = bp.mat;
    jb.materials = std::move(materials);
    jb.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
    jb.faceLoads = std::move(faceLoads);
    jb.caseLoads = std::move(caseLoads);
    jb.mesh = std::move(*mesh);
    out.bodies.push_back(std::move(jb));
  }
  return true;
}

bool PrepareStudy(const Document& doc, Study& study, StudyJobInput& out,
                  std::string* error) {
  out = StudyJobInput{};
  StudyPlan plan;
  if (!PlanStudy(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlannedStudy(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

namespace {

// The facet's corner normal, outward as Netgen orients a boundary triangle.
glm::dvec3 FacetCross(const FemMesh& mesh, const FemMesh::BoundaryTri& t) {
  const glm::dvec3& a = mesh.nodes[t.nodes[0]];
  return glm::cross(mesh.nodes[t.nodes[1]] - a, mesh.nodes[t.nodes[2]] - a);
}

// One pin as the contact iteration sees it: its facets, its nodes and each
// node's frame (into the material, round the axis, along it).
struct PinState {
  const StudyJobBody::PinSupport* pin = nullptr;
  std::vector<int> nodes;                       // unique, ascending
  std::map<int, int> slot;                      // node -> index into nodes
  std::vector<std::array<glm::dvec3, 3>> frame; // per node
  std::vector<char> active;                     // per facet (pin->tris)
  std::vector<double> nodeWeight;               // area share x radius, per node
  double faceArea = 0.0;
};

// Solves one body with pins (docs/simulation/README.md#pin-supports): per
// case, the facets each pin presses on are found by iterating linear solves
// -- a facet whose nodes the pin pulls leaves the contact, one whose nodes
// move into the pin returns -- and each iteration's constraints are tested
// for a rigid motion they leave free before anything is factored.
std::optional<std::vector<FemResults>>
SolveWithPins(const StudyJobBody& jb, const std::vector<Material>& mats,
              const std::vector<std::vector<FaceLoad>>& rhs,
              const glm::dvec3& g, const SolveOptions& options,
              SolveReport& report, std::string* error) {
  const FemMesh& mesh = jb.mesh;
  auto fail =
      [&](const std::string& m) -> std::optional<std::vector<FemResults>> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  std::vector<PinState> pins(jb.pins.size());
  for (size_t p = 0; p < jb.pins.size(); ++p) {
    PinState& ps = pins[p];
    ps.pin = &jb.pins[p];
    const glm::dvec3 a = glm::normalize(ps.pin->axisDirection);
    const glm::dvec3 o = ps.pin->axisOrigin;
    std::set<int> nodes;
    double side = 0.0; // > 0: the face looks away from the axis (a shaft)
    for (int t : ps.pin->tris) {
      const auto& tri = mesh.boundary[t];
      for (int v : tri.nodes)
        nodes.insert(v);
      const glm::dvec3 cr = FacetCross(mesh, tri);
      ps.faceArea += 0.5 * glm::length(cr);
      glm::dvec3 c = (mesh.nodes[tri.nodes[0]] + mesh.nodes[tri.nodes[1]] +
                      mesh.nodes[tri.nodes[2]]) /
                     3.0;
      glm::dvec3 r = (c - o) - glm::dot(c - o, a) * a;
      side += glm::dot(cr, r);
    }
    ps.nodes.assign(nodes.begin(), nodes.end());
    for (size_t i = 0; i < ps.nodes.size(); ++i) {
      ps.slot[ps.nodes[i]] = static_cast<int>(i);
      const glm::dvec3 x = mesh.nodes[ps.nodes[i]];
      glm::dvec3 r = (x - o) - glm::dot(x - o, a) * a;
      if (glm::length(r) <= 0.0)
        return fail("pin " + std::to_string(ps.pin->id) +
                    ": a node of its face lies on its axis");
      r = glm::normalize(r);
      // Into the material: away from the axis in a bore, towards it on a
      // shaft.
      const glm::dvec3 m = side > 0.0 ? -r : r;
      ps.frame.push_back({m, glm::cross(a, m), a});
    }
    ps.active.assign(ps.pin->tris.size(), 1);
    // Each node's share of its facets' area (a third of each corner's, a
    // sixth of each node's; any positive share ties the turn), times its
    // distance from the axis: the tie is then the face's moment of slide.
    ps.nodeWeight.assign(ps.nodes.size(), 0.0);
    for (int t : ps.pin->tris) {
      const auto& tri = mesh.boundary[t];
      const double area = 0.5 * glm::length(FacetCross(mesh, tri));
      for (int v : tri.nodes)
        ps.nodeWeight[ps.slot.at(v)] += area / 6.0;
    }
    for (size_t i = 0; i < ps.nodes.size(); ++i) {
      const glm::dvec3 x = mesh.nodes[ps.nodes[i]];
      ps.nodeWeight[i] *= glm::length((x - o) - glm::dot(x - o, a) * a);
    }
  }
  const auto pinFreeToTurn = [&](const glm::dvec3& dir, const glm::dvec3& pt,
                                 double scale) -> const PinState* {
    for (const PinState& ps : pins) {
      if (ps.pin->fixRotation)
        continue;
      const glm::dvec3 a = glm::normalize(ps.pin->axisDirection);
      const glm::dvec3 d = pt - ps.pin->axisOrigin;
      if (std::abs(glm::dot(a, dir)) > 1.0 - 1e-6 &&
          glm::length(d - glm::dot(d, a) * a) < 1e-3 * scale)
        return &ps;
    }
    return nullptr;
  };
  glm::dvec3 lo(1e300), hi(-1e300);
  for (const glm::dvec3& p : mesh.nodes) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  const double scale = glm::length(hi - lo);

  constexpr int kMaxContactIterations = 30;
  std::vector<FemResults> out;
  for (size_t k = 0; k < rhs.size(); ++k) {
    for (PinState& ps : pins)
      ps.active.assign(ps.pin->tris.size(), 1);
    const std::string inCase =
        rhs.size() > 1 ? " in load case " + std::to_string(k + 1) : "";
    bool settled = false, finalPass = false;
    std::vector<std::vector<char>> seen;
    {
      std::vector<char> state;
      for (const PinState& ps : pins)
        state.insert(state.end(), ps.active.begin(), ps.active.end());
      seen.push_back(std::move(state));
    }
    for (int it = 0; it < kMaxContactIterations && !settled; ++it) {
      Supports sup;
      sup.fixedNodes = jb.fixedNodes;
      sup.reactions = true;
      for (const PinState& ps : pins) {
        std::vector<char> held(ps.nodes.size(), 0);
        for (size_t f = 0; f < ps.pin->tris.size(); ++f)
          if (ps.active[f])
            for (int v : mesh.boundary[ps.pin->tris[f]].nodes)
              held[ps.slot.at(v)] = 1;
        // Radially and axially where the facets press on the pin. A pin
        // held from turning ties its face's slides round the axis, weighted
        // by the area each node stands for and its radius: the face as a
        // whole does not turn, while each node may still slide.
        Tie tie;
        for (size_t i = 0; i < ps.nodes.size(); ++i)
          if (held[i] || ps.pin->fixRotation) {
            HeldNode h;
            h.node = ps.nodes[i];
            h.frame = ps.frame[i];
            h.held = {bool(held[i]), false, bool(held[i])};
            sup.held.push_back(h);
            if (ps.pin->fixRotation)
              tie.entries.push_back({ps.nodes[i], 1, ps.nodeWeight[i]});
          }
        if (!tie.entries.empty())
          sup.ties.push_back(std::move(tie));
      }
      glm::dvec3 dir(0.0), pt(0.0);
      bool turns = false;
      const std::string free = FreeRigidMotion(mesh, sup, &dir, &pt, &turns);
      if (!free.empty()) {
        const PinState* about = turns ? pinFreeToTurn(dir, pt, scale) : nullptr;
        if (it == 0)
          return fail("its supports leave it free to " + free +
                      (about ? ": pin " + std::to_string(about->pin->id) +
                                   " turns freely, so the part needs a second "
                                   "support, or fix_rotation on the pin"
                             : ""));
        return fail("the loads" + inCase +
                    " pull it off its pins: held only where it presses on "
                    "them, it is free to " +
                    free);
      }
      SolveReport r;
      auto one = SolveCases(mesh, mats, sup, {rhs[k]}, error, g, options, &r);
      if (!one)
        return std::nullopt;
      report.solver = r.solver;
      report.iterations += r.iterations;
      report.worstResidual = std::max(report.worstResidual, r.worstResidual);
      if (!r.externalDeclined.empty())
        report.externalDeclined = r.externalDeclined;
      FemResults& res = one->front();
      // Thresholds: a pull smaller than a millionth of the largest reaction
      // on the pins, a move smaller than a billionth of the largest one.
      double maxR = 0.0;
      for (const PinState& ps : pins)
        for (int v : ps.nodes)
          maxR = std::max(maxR, glm::length(res.nodeReaction[v]));
      const double tolR = 1e-6 * maxR;
      const double tolU = 1e-9 * std::max(res.maxDisplacement, 1e-300);
      bool changed = false;
      double pushed = 0.0, pulled = 0.0;
      for (PinState& ps : pins)
        for (size_t f = 0; f < ps.pin->tris.size(); ++f) {
          const auto& tri = mesh.boundary[ps.pin->tris[f]];
          double push = 0.0, into = 0.0;
          for (int v : tri.nodes) {
            const glm::dvec3& m = ps.frame[ps.slot.at(v)][0];
            push += glm::dot(res.nodeReaction[v], m);
            into += glm::dot(res.displacement[v], m);
          }
          if (ps.active[f]) {
            pushed += std::max(push, 0.0);
            pulled += std::max(-push, 0.0);
          }
          if (finalPass)
            continue;
          if (ps.active[f] && push < -tolR) {
            ps.active[f] = 0;
            changed = true;
          } else if (!ps.active[f] && into / 6.0 < -tolU) {
            ps.active[f] = 1;
            changed = true;
          }
        }
      // A facet straddling the contact's edge can flip each time: once the
      // set repeats, only the facets every state of the cycle had in
      // contact stay in it, and that is solved once more as the answer --
      // the edge facets let go rather than kept pulling.
      if (changed && !finalPass) {
        std::vector<char> state;
        for (const PinState& ps : pins)
          state.insert(state.end(), ps.active.begin(), ps.active.end());
        for (size_t h = 0; h < seen.size(); ++h)
          if (seen[h] == state) {
            for (size_t q = h; q < seen.size(); ++q)
              for (size_t i = 0; i < state.size(); ++i)
                state[i] &= seen[q][i];
            size_t at = 0;
            for (PinState& ps : pins)
              for (char& c : ps.active)
                c = state[at++];
            finalPass = true;
            break;
          }
        seen.push_back(std::move(state));
      } else if (finalPass) {
        changed = false;
      }
      // The answer holds no pull worth the name: a pin that pulls is not one.
      if (!changed && pulled > 1e-3 * pushed)
        return fail("the pins' contact" + inCase +
                    " did not settle: it still pulls on the part");
      if (!changed) {
        settled = true;
        // What each pin pressed on.
        for (const PinState& ps : pins) {
          FemResults::SupportReaction sr;
          sr.support = ps.pin->id;
          sr.type = LoadType::Pin;
          sr.faceAreaMm2 = ps.faceArea;
          for (size_t f = 0; f < ps.pin->tris.size(); ++f)
            if (ps.active[f])
              sr.contactAreaMm2 +=
                  0.5 *
                  glm::length(FacetCross(mesh, mesh.boundary[ps.pin->tris[f]]));
          res.reactions.push_back(sr);
        }
        out.push_back(std::move(res));
      }
    }
    if (!settled)
      return fail("the pins' contact" + inCase + " did not settle in " +
                  std::to_string(kMaxContactIterations) + " iterations");
  }
  return out;
}

// Each support's total reaction, from the nodes' (every node counted once,
// for the first support that holds it), into the result's reaction list;
// then the support-edge flag. Clears the per-node reactions.
void FinishSupports(const StudyJobBody& jb, FemResults& res) {
  std::vector<FemResults::SupportReaction> pinRows = std::move(res.reactions);
  res.reactions.clear();
  std::vector<char> counted(jb.mesh.nodes.size(), 0);
  for (const SupportFacets& sf : jb.supports) {
    FemResults::SupportReaction sr;
    for (const auto& pr : pinRows)
      if (pr.support == sf.id && sf.type == LoadType::Pin)
        sr = pr;
    sr.support = sf.id;
    sr.type = sf.type;
    if (res.nodeReaction.size() == jb.mesh.nodes.size()) {
      sr.hasMoment = true;
      for (int t : sf.tris)
        for (int v : jb.mesh.boundary[t].nodes)
          if (!counted[v]) {
            counted[v] = 1;
            sr.forceN += res.nodeReaction[v];
            sr.momentNmm += glm::cross(jb.mesh.nodes[v], res.nodeReaction[v]);
          }
    }
    res.reactions.push_back(sr);
  }
  res.nodeReaction.clear();
  res.nodeReaction.shrink_to_fit();
  res.supportEdgePeak = FindSupportEdgePeak(jb.mesh, res, jb.supports);
}

} // namespace

bool SolvePreparedStudy(StudyJobInput& in, StudyRuntime& rt, std::string* error,
                        const SolveOptions& options) {
  rt = StudyRuntime{};
  rt.caseNames = in.caseNames;
  rt.sourceDigest = in.sourceDigest;
  rt.shapeThreshold = in.shapeThreshold;
  rt.shapeIslandTets = in.shapeIslandTets;
  rt.shapeVolume = in.shapeVolume;
  rt.shapeThresholdLoweredFrom = in.shapeThresholdLoweredFrom;
  std::string err;
  // A failed (or cancelled) solve leaves no half result: a caller reads a
  // runtime with bodies as solved whole.
  auto fail = [&](const std::string& m) {
    rt = StudyRuntime{};
    if (error)
      *error = m;
    return false;
  };
  for (StudyJobBody& jb : in.bodies) {
    StudyBodyResult sbr;
    sbr.bodyIndex = jb.bodyIndex;
    sbr.bodyName = jb.bodyName;

    if (in.type == AnalysisType::Modal) {
      auto modes =
          jb.materials.empty()
              ? SolveModal(jb.mesh, jb.mat, jb.fixedNodes, in.numModes, &err)
              : SolveModal(jb.mesh, jb.materials, jb.fixedNodes, in.numModes,
                           &err);
      if (!modes)
        return fail("body '" + jb.bodyName + "': " + err);
      sbr.results.modes = std::move(*modes);
    } else {
      const glm::dvec3 g = in.gravity * 1000.0; // m/s^2 -> mm/s^2
      // Every case on one factorisation; without cases, the one set of loads.
      const std::vector<std::vector<FaceLoad>> rhs =
          jb.caseLoads.empty()
              ? std::vector<std::vector<FaceLoad>>{jb.faceLoads}
              : jb.caseLoads;
      const std::vector<Material> mats =
          jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
      SolveReport report;
      std::optional<std::vector<FemResults>> all;
      if (jb.pins.empty()) {
        Supports sup;
        sup.fixedNodes = jb.fixedNodes;
        sup.reactions = true;
        all = SolveCases(jb.mesh, mats, sup, rhs, &err, g, options, &report);
      } else {
        report.solver = kDirectSolverName;
        all = SolveWithPins(jb, mats, rhs, g, options, report, &err);
      }
      if (!all)
        return fail("body '" + jb.bodyName + "': " + err);
      for (FemResults& c : *all)
        FinishSupports(jb, c);
      // Several bodies may solve differently; the study names the
      // heaviest solver any body needed, which is the one a reader wants to
      // know about, and why an offered GPU was not it.
      std::string name = report.solver;
      if (!report.externalDeclined.empty())
        name += "; GPU declined: " + report.externalDeclined;
      if (rt.solver.empty() || rt.solver == kDirectSolverName ||
          report.solver == kGpuSolverName)
        rt.solver = name;
      // The maxima a summary reads are the worst over the cases.
      for (const FemResults& c : *all) {
        if (c.maxVonMises > rt.maxVonMises) {
          rt.maxVonMises = c.maxVonMises;
          rt.maxVmAt = c.maxVmAt;
          rt.maxVmBody = jb.bodyName;
        }
        if (c.maxDisplacement > rt.maxDisplacement) {
          rt.maxDisplacement = c.maxDisplacement;
          rt.maxDispAt = c.maxDispAt;
          rt.maxDispBody = jb.bodyName;
        }
      }
      sbr.results = all->front();
      if (!jb.caseLoads.empty())
        sbr.cases = std::move(*all);
    }
    sbr.mesh = std::move(jb.mesh);
    rt.bodies.push_back(std::move(sbr));
  }
  return true;
}

bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              std::string* error) {
  StudyJobInput input;
  if (!PrepareStudy(doc, study, input, error))
    return false;
  if (!SolvePreparedStudy(input, rt, error)) {
    // Solve failures set study.error too, matching the pre-split behavior.
    study.error = error ? *error : "solve failed";
    return false;
  }
  return true;
}

namespace {

// What a keep-out names, for its report and its refusals.
std::string KeepOutLabel(const KeepOut& k) {
  if (!k.body.empty())
    return "body '" + k.body + "'";
  char buf[160];
  std::snprintf(buf, sizeof(buf), "box [%g, %g, %g]-[%g, %g, %g]", k.boxMin.x,
                k.boxMin.y, k.boxMin.z, k.boxMax.x, k.boxMax.y, k.boxMax.z);
  return buf;
}

} // namespace

bool PrepareTopology(const Document& doc, Study& study, TopoInput& out,
                     std::string* error) {
  out = TopoInput{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  if (study.volumeFraction <= 0.0 || study.volumeFraction >= 1.0)
    return fail("volume fraction must be in (0, 1)");
  for (const Load& l : study.loads)
    if (l.type == LoadType::Pin)
      return fail("load " + std::to_string(l.id) +
                  ": a topology study's supports are fixed (it freezes the "
                  "faces they hold); a pin is a static study's");
  if (!ValidateLoadCases(study, &err))
    return fail(err);

  const auto& bodies = doc.GetBodies();

  // Group loads by body (v1 optimizes exactly one loaded body).
  std::map<int, std::vector<std::pair<int, size_t>>>
      bodyLoads; // (faceIndex, loadIdx)
  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    bodyLoads[rf.bodyIndex].push_back({rf.faceIndex, i});
  }
  if (bodyLoads.empty())
    return fail("study has no loads");
  if (bodyLoads.size() > 1)
    return fail("topology optimization supports a single loaded body (v1)");
  const int bodyIndex = bodyLoads.begin()->first;
  const Body& body = bodies[bodyIndex];

  MeshSettings settings;
  settings.maxSize = study.meshSize;
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      settings.faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  const auto matType =
      CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
  if (mp.youngGPa <= 0.0f)
    return fail("body '" + body.name + "': material '" +
                (body.material.empty() ? "default" : body.material) +
                "' has no stiffness data");

  // Keep faces (frozen solid beyond the auto-frozen support/load skin), by
  // keep-face index so an island can say which it took.
  std::vector<int> keepFaceIdx(study.keepFaces.size(), -1);
  for (size_t i = 0; i < study.keepFaces.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.keepFaces[i],
                     "keep face " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      keepFaceIdx[i] = rf.faceIndex;
  }

  // Keep-outs, resolved before anything is meshed: a body the part does not
  // have, the design body itself and an empty box are refused by name.
  struct KeepOutShape {
    const TopoDS_Shape* shape = nullptr; // a body's; null = the box
    glm::dvec3 lo{0.0}, hi{0.0};         // the box, or the body's bounds
  };
  std::vector<KeepOutShape> keepOuts;
  for (size_t i = 0; i < study.keepOuts.size(); ++i) {
    const KeepOut& k = study.keepOuts[i];
    KeepOutShape ks;
    if (!k.body.empty()) {
      const Body* hit = nullptr;
      std::string names;
      for (const Body& b : bodies) {
        names += (names.empty() ? "" : ", ") + ("'" + b.name + "'");
        if (b.name == k.body)
          hit = &b;
      }
      if (hit == nullptr)
        return fail("keep-out " + std::to_string(i + 1) +
                    ": the part has no "
                    "body '" +
                    k.body + "' (it has " + names +
                    "); a neighbour's geometry arrives as a Derived "
                    "reference body");
      if (hit == &body)
        return fail("keep-out " + std::to_string(i + 1) + ": '" + k.body +
                    "' is the body being optimised");
      ks.shape = &hit->shape.GetShape();
      if (!Measure::WorldBounds({*hit}, glm::dmat4(1.0), ks.lo, ks.hi))
        return fail("keep-out " + std::to_string(i + 1) + ": body '" + k.body +
                    "' has no shape");
    } else {
      if (!(k.boxMax.x > k.boxMin.x && k.boxMax.y > k.boxMin.y &&
            k.boxMax.z > k.boxMin.z))
        return fail("keep-out " + std::to_string(i + 1) + ": " +
                    KeepOutLabel(k) +
                    " is empty: its max must exceed its min on every axis");
      ks.lo = k.boxMin;
      ks.hi = k.boxMax;
    }
    keepOuts.push_back(ks);
  }

  // A deep copy, as a static plan meshes: Netgen writes triangulation into
  // the faces it meshes and clears their flags, and the document's shape is
  // what the renderer draws and the project saves.
  auto meshed =
      Mesh(BRepBuilderAPI_Copy(body.shape.GetShape()).Shape(), settings, &err);
  if (!meshed)
    return fail("body '" + body.name + "': " + err);

  // The faces that carry a support, a load or a keep, which no keep-out may
  // take a tet of.
  std::map<int, std::string> anchorFace; // face index -> what holds it
  for (const auto& [faceIndex, loadIdx] : bodyLoads[bodyIndex])
    anchorFace.emplace(
        faceIndex,
        (IsSupport(study.loads[loadIdx].type) ? "support " : "load ") +
            std::to_string(study.loads[loadIdx].id));
  for (size_t i = 0; i < keepFaceIdx.size(); ++i)
    if (keepFaceIdx[i] >= 0)
      anchorFace.emplace(keepFaceIdx[i], "keep face " + std::to_string(i + 1));

  // A tet whose centroid lies inside a keep-out leaves the mesh: void, and
  // not design space (docs/topopt/README.md#keep-outs).
  if (!keepOuts.empty()) {
    const FemMesh& full = *meshed;
    // The triangles of those faces, by their sorted corners: a keep-out may
    // not take a tet one of them is a face of. (A tet that only touches such
    // a face at an edge or a corner carries none of it, and goes.)
    const auto triKey = [](int a, int b, int c) {
      std::array<int, 3> k{a, b, c};
      std::sort(k.begin(), k.end());
      return k;
    };
    std::map<std::array<int, 3>, std::string> anchorTri;
    for (const FemMesh::BoundaryTri& t : full.boundary)
      if (auto it = anchorFace.find(t.faceIndex); it != anchorFace.end())
        anchorTri.emplace(triKey(t.nodes[0], t.nodes[1], t.nodes[2]),
                          it->second);
    std::vector<char> keep(full.tets.size(), 1);
    for (size_t i = 0; i < keepOuts.size(); ++i) {
      const KeepOutShape& ks = keepOuts[i];
      std::vector<char> inside(full.tets.size(), 0);
      for (size_t e = 0; e < full.tets.size(); ++e) {
        const auto& t = full.tets[e];
        const glm::dvec3 c = 0.25 * (full.nodes[t[0]] + full.nodes[t[1]] +
                                     full.nodes[t[2]] + full.nodes[t[3]]);
        if (c.x < ks.lo.x || c.y < ks.lo.y || c.z < ks.lo.z || c.x > ks.hi.x ||
            c.y > ks.hi.y || c.z > ks.hi.z)
          continue;
        if (ks.shape != nullptr) {
          BRepClass3d_SolidClassifier cls(*ks.shape, gp_Pnt(c.x, c.y, c.z),
                                          1e-6);
          if (cls.State() != TopAbs_IN)
            continue;
        }
        static constexpr int kFace[4][3] = {
            {0, 1, 2}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3}};
        for (const auto& fc : kFace)
          if (auto it = anchorTri.find(triKey(t[fc[0]], t[fc[1]], t[fc[2]]));
              it != anchorTri.end())
            return fail("keep-out " + std::to_string(i + 1) + " (" +
                        KeepOutLabel(study.keepOuts[i]) +
                        ") takes material that " + it->second +
                        " is on: a keep-out must clear the supports, loads "
                        "and keep faces");
        inside[e] = 1;
      }
      KeepOutReport rep;
      rep.what = KeepOutLabel(study.keepOuts[i]);
      for (size_t e = 0; e < inside.size(); ++e)
        if (inside[e] && keep[e]) {
          keep[e] = 0;
          ++rep.tets;
        }
      rep.volume = TetSetVolume(full, inside);
      out.keepOuts.push_back(rep);
    }
    meshed = ExtractSubMesh(full, keep);
    if (meshed->tets.empty())
      return fail("the keep-outs take the whole of body '" + body.name + "'");
  }
  FemMesh& mesh = *meshed;

  // Fixed nodes + face tractions per case + keep nodes, exactly as the static
  // solve resolves them (fixed faces contribute their nodes; force/pressure
  // faces their tri indices; keep faces their nodes).
  std::set<int> fixedSet, keepSet;
  const size_t nCases = std::max<size_t>(1, study.loadCases.size());
  std::vector<TopoCase> cases(nCases);
  for (size_t k = 0; k < study.loadCases.size(); ++k) {
    cases[k].name = study.loadCases[k].name;
    cases[k].weight = study.loadCases[k].weight;
    out.caseNames.push_back(study.loadCases[k].name);
  }
  bool hasFixed = false, hasApplied = false;
  for (const auto& [faceIndex, loadIdx] : bodyLoads[bodyIndex]) {
    const Load& load = study.loads[loadIdx];
    if (load.type == LoadType::Fixed) {
      hasFixed = true;
      for (const auto& tri : mesh.boundary)
        if (tri.faceIndex == faceIndex)
          for (int n : tri.nodes)
            fixedSet.insert(n);
    } else {
      hasApplied = true;
      Load complete = load;
      if (!CompleteLoadGeometry(FaceByIndex(body.shape.GetShape(), faceIndex),
                                complete, &err))
        return fail(err);
      FaceLoad fl = MakeFaceLoad(complete);
      for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
        if (mesh.boundary[t].faceIndex == faceIndex)
          fl.tris.push_back(t);
      const int k =
          study.loadCases.empty() ? 0 : study.FindLoadCase(load.loadCase);
      cases[k].loads.push_back(std::move(fl));
    }
  }
  if (!hasFixed)
    return fail("body '" + body.name + "' has no fixed support");
  if (!hasApplied)
    return fail("topology optimization needs a force or pressure load");
  if (fixedSet.empty())
    return fail("body '" + body.name + "': fixed face meshed to no nodes");
  out.keepFaceTris.assign(study.keepFaces.size(), {});
  for (size_t i = 0; i < keepFaceIdx.size(); ++i) {
    if (keepFaceIdx[i] < 0)
      continue;
    for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
      if (mesh.boundary[t].faceIndex == keepFaceIdx[i]) {
        out.keepFaceTris[i].push_back(t);
        for (int n : mesh.boundary[t].nodes)
          keepSet.insert(n);
      }
  }

  out.bodyIndex = bodyIndex;
  out.bodyName = body.name;
  out.mesh = std::move(mesh);
  // Density is unused (gravity is intentionally excluded from the TO
  // objective).
  out.material = Material{mp.youngGPa * 1000.0, mp.poisson, 0.0};
  out.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
  out.cases = std::move(cases);
  out.keepNodes.assign(keepSet.begin(), keepSet.end());
  out.params.volumeFraction = study.volumeFraction;
  out.params.filterRadius = study.filterRadius;
  out.params.maxIterations = study.maxIterations > 0 ? study.maxIterations : 60;
  return true;
}

StudyRuntime MakeTopoRuntime(TopoInput& input, const TopoResult& result) {
  StudyRuntime rt;
  rt.topology = true;
  StudyBodyResult sbr;
  sbr.bodyIndex = input.bodyIndex;
  sbr.bodyName = input.bodyName;
  sbr.density = result.density;
  rt.iterations = result.iterations;
  rt.converged = result.converged;
  rt.cancelled = result.cancelled;
  rt.volumeFractionAchieved = result.volumeFractionAchieved;
  rt.complianceHistory = result.complianceHistory;
  rt.caseNames = input.caseNames;
  for (const TopoCase& c : input.cases)
    rt.caseWeights.push_back(c.weight);
  rt.caseCompliance = result.caseCompliance;
  rt.keepOuts = input.keepOuts;
  rt.islands = result.islands;
  // Which keep faces an island took: a face any of whose triangles' tets
  // were removed.
  if (!result.islandTets.empty()) {
    std::set<int> gone(result.islandTets.begin(), result.islandTets.end());
    std::set<int> goneNodes;
    for (int e : gone)
      for (int v : input.mesh.tets[e])
        goneNodes.insert(v);
    for (size_t i = 0; i < input.keepFaceTris.size(); ++i) {
      bool hit = false;
      for (int t : input.keepFaceTris[i]) {
        // A boundary triangle belongs to the removed tet whose corners
        // include all three of its own.
        const auto& tri = input.mesh.boundary[t].nodes;
        if (goneNodes.count(tri[0]) && goneNodes.count(tri[1]) &&
            goneNodes.count(tri[2])) {
          for (int e : gone) {
            int shared = 0;
            for (int c = 0; c < 4; ++c)
              shared += input.mesh.tets[e][c] == tri[0] ||
                        input.mesh.tets[e][c] == tri[1] ||
                        input.mesh.tets[e][c] == tri[2];
            if (shared == 3) {
              hit = true;
              break;
            }
          }
        }
        if (hit)
          break;
      }
      if (hit)
        rt.islands.keepFaces.push_back(static_cast<int>(i));
    }
  }
  sbr.mesh = std::move(input.mesh);
  rt.densityDigest = DensityDigest(sbr.mesh, sbr.density);
  rt.bodies.push_back(std::move(sbr));
  return rt;
}

bool RunTopology(const Document& doc, Study& study, StudyRuntime& rt,
                 std::string* error) {
  TopoInput input;
  if (!PrepareTopology(doc, study, input, error))
    return false;
  std::string err;
  auto res = TopologyOptimize(input.mesh, input.material, input.fixedNodes,
                              input.cases, input.params, &err, {},
                              input.keepNodes, nullptr);
  if (!res) {
    study.error = "body '" + input.bodyName + "': " + err;
    if (error)
      *error = study.error;
    return false;
  }
  rt = MakeTopoRuntime(input, *res);
  return true;
}

// ---- What a result keeps -------------------------------------------------
// docs/simulation/README.md#what-a-saved-result-keeps

namespace {

double ToFloat(double v) { return static_cast<double>(static_cast<float>(v)); }

glm::dvec3 ToFloat(const glm::dvec3& p) {
  return {ToFloat(p.x), ToFloat(p.y), ToFloat(p.z)};
}

void RoundAll(std::vector<double>& v) {
  for (double& x : v)
    x = ToFloat(x);
}

void RoundAll(std::vector<glm::dvec3>& v) {
  for (glm::dvec3& p : v)
    p = ToFloat(p);
}

void RoundAll(std::vector<Stress>& v) {
  for (Stress& s : v)
    for (double& x : s)
      x = ToFloat(x);
}

void RoundFields(FemResults& r) {
  RoundAll(r.displacement);
  RoundAll(r.vonMises);
  RoundAll(r.stress);
  for (FemResults::ModeShape& m : r.modes)
    RoundAll(m.displacement);
}

// Every per-node array of `r` has one entry per node (or none).
bool PerNode(const FemResults& r, size_t n) {
  if (!r.displacement.empty() && r.displacement.size() != n)
    return false;
  if (!r.vonMises.empty() && r.vonMises.size() != n)
    return false;
  if (!r.stress.empty() && r.stress.size() != n)
    return false;
  for (const FemResults::ModeShape& m : r.modes)
    if (m.displacement.size() != n)
      return false;
  return true;
}

template <typename T>
void Gather(std::vector<T>& v, const std::vector<int>& kept) {
  if (v.empty())
    return;
  std::vector<T> out;
  out.reserve(kept.size());
  for (int old : kept)
    out.push_back(v[old]);
  v = std::move(out);
}

void Renumber(FemResults& r, const std::vector<int>& kept,
              const std::vector<int>& newIndex) {
  Gather(r.displacement, kept);
  Gather(r.vonMises, kept);
  Gather(r.stress, kept);
  for (FemResults::ModeShape& m : r.modes)
    Gather(m.displacement, kept);
  if (r.maxVmNode >= 0)
    r.maxVmNode = newIndex[r.maxVmNode];
  if (r.maxDispNode >= 0)
    r.maxDispNode = newIndex[r.maxDispNode];
}

// A static or modal body down to its boundary surface.
void KeepSurface(StudyBodyResult& b) {
  FemMesh& m = b.mesh;
  const size_t n = m.nodes.size();
  if (!PerNode(b.results, n))
    return;
  for (const FemResults& c : b.cases)
    if (!PerNode(c, n))
      return;
  std::vector<char> used(n, 0);
  for (const FemMesh::BoundaryTri& t : m.boundary)
    for (int v : t.nodes)
      if (v < 0 || static_cast<size_t>(v) >= n)
        return; // not a mesh this can renumber; left as it is
      else
        used[v] = 1;
  const auto peak = [&](const FemResults& r) {
    for (int v : {r.maxVmNode, r.maxDispNode})
      if (v >= 0 && static_cast<size_t>(v) < n)
        used[v] = 1;
  };
  peak(b.results);
  for (const FemResults& c : b.cases)
    peak(c);
  const auto valid = [&](const FemResults& r) {
    for (int v : {r.maxVmNode, r.maxDispNode})
      if (v >= 0 && static_cast<size_t>(v) >= n)
        return false;
    return true;
  };
  if (!valid(b.results))
    return;
  for (const FemResults& c : b.cases)
    if (!valid(c))
      return;

  std::vector<int> kept, newIndex(n, -1);
  for (size_t i = 0; i < n; ++i)
    if (used[i]) {
      newIndex[i] = static_cast<int>(kept.size());
      kept.push_back(static_cast<int>(i));
    }
  if (kept.size() != n) {
    Gather(m.nodes, kept);
    for (FemMesh::BoundaryTri& t : m.boundary)
      for (int& v : t.nodes)
        v = newIndex[v];
    Renumber(b.results, kept, newIndex);
    for (FemResults& c : b.cases)
      Renumber(c, kept, newIndex);
  }
  m.tets.clear();
  m.tets.shrink_to_fit();
  m.tetDomain.clear();
  m.tetDomain.shrink_to_fit();
}

} // namespace

void KeepWhatIsRead(StudyRuntime& rt) {
  for (StudyBodyResult& b : rt.bodies) {
    FemMesh& m = b.mesh;
    // The solved mesh's counts survive the cut (a loaded result carries
    // them; a fresh run fills them from the mesh).
    if (m.stats.nodeCount == 0)
      m.stats.nodeCount = m.nodes.size();
    if (m.stats.tetCount == 0)
      m.stats.tetCount = m.tets.size();
    if (!rt.topology)
      KeepSurface(b);
    if (std::all_of(m.tetDomain.begin(), m.tetDomain.end(),
                    [](int d) { return d == 0; }))
      m.tetDomain.clear();
    RoundAll(m.nodes);
    RoundAll(b.density);
    RoundFields(b.results);
    for (FemResults& c : b.cases)
      RoundFields(c);
  }
}

} // namespace fem
