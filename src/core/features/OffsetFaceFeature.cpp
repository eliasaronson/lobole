// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/OffsetFaceFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/ModelSpace.h"
#include "core/features/ReplaceFaceFeature.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepLib.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

OffsetFaceFeature::OffsetFaceFeature(const std::string& name, uint64_t bodyId,
                                     std::vector<ElementRef> faces,
                                     double distance)
    : bodyId(bodyId), faces(std::move(faces)), distance(distance) {
  m_name = name;
}

namespace directedit {

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

TopoDS_Face FaceAt(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++)
    if (i == index)
      return TopoDS::Face(ex.Current());
  return {};
}

// A picked face whose neighbour across an edge meets it tangentially and is
// not itself picked: offsetting one without the other tears them apart.
std::string TangentNeighbour(const Picked& picked) {
  const TopoDS_Shape& shape = picked.body->shape.GetShape();
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, parents);
  for (size_t k = 0; k < picked.indices.size(); k++) {
    const TopoDS_Face face = FaceAt(shape, picked.indices[k]);
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
      const int at = parents.FindIndex(edge);
      if (at == 0)
        continue;
      for (const TopoDS_Shape& other : parents(at)) {
        if (other.IsSame(face))
          continue;
        const int oi = faces.FindIndex(other) - 1;
        if (oi < 0 || std::find(picked.indices.begin(), picked.indices.end(),
                                oi) != picked.indices.end())
          continue;
        if (!Measure::EdgeIsSmooth(edge, face, TopoDS::Face(other)))
          continue;
        const std::string* on = picked.body->elementMap.NameOf(oi);
        const std::string otherName = on ? *on : "face " + std::to_string(oi);
        return "offsetting " +
               DescribeFace(*picked.body, picked.indices[k], picked.names[k]) +
               " would tear it from " +
               DescribeFace(*picked.body, oi, otherName) +
               ", which meets it tangentially (a fillet?): offset both "
               "together";
      }
    }
  }
  return {};
}

// BRepOffset_MakeOffset reports an offset face as GENERATED from the face it
// offset; to the edit it is that face, moved: Modified, so it is named
// "<name>;M<tag>" (the history the naming reads, through this adapter).
struct OffsetHistory {
  BRepOffset_MakeOffset& maker;
  TopTools_ListOfShape scratch;
  const TopTools_ListOfShape& Modified(const TopoDS_Shape& s) {
    if (s.ShapeType() != TopAbs_FACE)
      return maker.Modified(s);
    scratch.Clear();
    for (const TopoDS_Shape& m : maker.Modified(s))
      scratch.Append(m);
    for (const TopoDS_Shape& g : maker.Generated(s))
      if (g.ShapeType() == TopAbs_FACE)
        scratch.Append(g);
    return scratch;
  }
  const TopTools_ListOfShape& Generated(const TopoDS_Shape& s) {
    if (s.ShapeType() != TopAbs_FACE)
      return maker.Generated(s);
    scratch.Clear();
    for (const TopoDS_Shape& g : maker.Generated(s))
      if (g.ShapeType() != TopAbs_FACE)
        scratch.Append(g);
    return scratch;
  }
  Standard_Boolean IsDeleted(const TopoDS_Shape& s) {
    if (s.ShapeType() == TopAbs_FACE && !Modified(s).IsEmpty())
      return Standard_False;
    return maker.IsDeleted(s);
  }
};

} // namespace

std::optional<double> ThicknessUnder(const Body& body, int faceIndex) {
  const TopoDS_Shape& shape = body.shape.GetShape();
  const TopoDS_Face face = FaceAt(shape, faceIndex);
  if (face.IsNull())
    return std::nullopt;
  const auto inset = Measure::FaceInsetOf(face);
  if (!inset)
    return std::nullopt;
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
  if (surf.IsNull())
    return std::nullopt;
  const gp_Pnt p(inset->point.x, inset->point.y, inset->point.z);
  GeomAPI_ProjectPointOnSurf proj(p, surf);
  if (!proj.IsDone() || proj.NbPoints() == 0)
    return std::nullopt;
  double u = 0.0, v = 0.0;
  proj.LowerDistanceParameters(u, v);
  GeomLProp_SLProps props(surf, u, v, 1, Precision::Confusion());
  if (!props.IsNormalDefined())
    return std::nullopt;
  gp_Dir n = props.Normal();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  // Inward: against the outward normal.
  const gp_Lin line(p, n.Reversed());
  IntCurvesFace_ShapeIntersector hit;
  hit.Load(shape, Precision::Confusion());
  hit.Perform(line, 1e-6, Precision::Infinite());
  if (!hit.IsDone())
    return std::nullopt;
  std::optional<double> nearest;
  for (int i = 1; i <= hit.NbPnt(); i++) {
    const double w = hit.WParameter(i);
    if (w > 1e-5 && (!nearest || w < *nearest))
      nearest = w;
  }
  return nearest;
}

std::string OffsetFaces(const Picked& picked, double distance, uint64_t tag,
                        CADShape& out, ElementMap& map, const std::string& op) {
  const Body& body = *picked.body;
  if (!InModelSpace(distance) || std::abs(distance) < 1e-6)
    return op + " distance " + Mm(distance) +
           " mm is not a distance to move a face by";
  // A face whose tangent neighbour (a fillet) is not offset with it: the
  // two surfaces never meet again, and BRepOffset quietly offsets the whole
  // tangent chain instead -- faces nobody picked.
  if (std::string tear = TangentNeighbour(picked); !tear.empty())
    return tear;
  if (std::string why = FreeformNeighbour(picked, op, "moves along");
      !why.empty())
    return why;
  if (distance < 0.0) {
    for (size_t k = 0; k < picked.indices.size(); k++) {
      const auto t = ThicknessUnder(body, picked.indices[k]);
      if (t && -distance >= *t - 1e-6)
        return "offsetting `" + picked.names[k] + "` by " + Mm(distance) +
               " mm consumes the body: it is " + Mm(*t) + " mm thick there";
    }
  }
  Expect expect;
  expect.volume =
      distance > 0.0 ? Expect::Volume::Grows : Expect::Volume::Shrinks;
  expect.faces = Expect::Faces::Modified;
  expect.bboxGrowth = std::max(0.0, distance);

  // The offset builder quilts its faces in an order that follows addresses
  // (MakeShells; patched in our OCC, nix/patches/occt-shell-cap-order.patch,
  // not in apt's): a result that fails the checks is built again on a fresh
  // copy, as Shell's is, and refused only when every draw failed.
  constexpr int kAttempts = 5;
  std::string error;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    const Copied copy = CopyFor(body, picked.indices);
    BRepOffset_MakeOffset maker;
    const Handle(Message_ProgressIndicator) deadline =
        MakeDeadline(kBuilderBudgetS);
    error = Guarded(op + " of " + NameList(picked.names), [&] {
      maker.Initialize(copy.shape, 0.0, Precision::Confusion(), BRepOffset_Skin,
                       /*Intersection=*/Standard_True,
                       /*SelfInter=*/Standard_False, GeomAbs_Intersection,
                       /*Thickening=*/Standard_False,
                       /*RemoveIntEdges=*/Standard_False);
      for (const TopoDS_Shape& f : copy.faces)
        maker.SetOffsetOnFace(TopoDS::Face(f), distance);
      maker.MakeOffsetShape(deadline->Start());
    });
    if (error.empty() && Expired(deadline)) {
      error = op + " of " + NameList(picked.names) + " gave up after " +
              std::to_string(int(kBuilderBudgetS)) +
              " s: the faces around them are too costly to offset";
      return error; // a draw again would cost as much
    }
    // A fault is not drawn again: the builder long-jumped out of its own
    // frames, and running it once more on that heap is how a caught fault
    // becomes an uncaught one (tests/test_direct_edit.cpp's battery holder).
    if (!error.empty()) {
      if (error.find("inside OpenCASCADE") != std::string::npos)
        break;
      continue;
    }
    if (!maker.IsDone() || maker.Shape().IsNull()) {
      error = op + " of " + NameList(picked.names) + " by " + Mm(distance) +
              " mm did not build (OpenCASCADE offset error " +
              std::to_string(static_cast<int>(maker.Error())) + ")";
      continue;
    }
    TopoDS_Shape shape = maker.Shape();
    // Skin mode hands back the offset SHELL where it could not close it into
    // a solid itself (a bore offset into its block): a closed shell is the
    // solid's boundary, made one again, its faces the same.
    if (!TopExp_Explorer(shape, TopAbs_SOLID).More()) {
      TopExp_Explorer shell(shape, TopAbs_SHELL);
      if (shell.More()) {
        BRepBuilderAPI_MakeSolid solid(TopoDS::Shell(shell.Current()));
        if (solid.IsDone()) {
          TopoDS_Solid made = solid.Solid();
          BRepLib::OrientClosedSolid(made);
          shape = made;
        }
      }
    }
    OffsetHistory adapter{maker, {}};
    BRepTools_History history(topo::HistoryArguments(copy.shape), adapter);
    ElementMap names =
        NameThrough(copy.shape, body.elementMap, history, shape, tag);
    const CADShape result(shape);
    RestoreUntouched(body, result, names, tag, picked.names);
    error = CheckResult(body, picked, result, names, tag, expect, op);
    if (error.empty()) {
      if (attempt > 0)
        LOG_INFO("{}: built on attempt {} of {}", op, attempt + 1, kAttempts);
      out = result;
      map = names;
      return {};
    }
  }
  // The offset builder refuses some faces it should move (a wall of an
  // imported holder with cradles cut across it: no error, no shape). A
  // planar face offset is that face put onto its own plane moved by the
  // distance, which Replace Face's cells build without it: one face at a
  // time, each onto its moved plane.
  const bool allPlanar =
      std::all_of(picked.indices.begin(), picked.indices.end(), [&](int i) {
        const auto info = Measure::DescribeFaceAt(body, i);
        return info && info->kind == "plane";
      });
  if (!allPlanar)
    return error;
  Body step = body;
  for (size_t k = 0; k < picked.names.size(); k++) {
    // An earlier step may have trimmed it: then it reads ";M<tag>".
    std::string name = picked.names[k];
    auto at = step.elementMap.Resolve(name);
    if (!at) {
      name += ";M" + std::to_string(tag);
      at = step.elementMap.Resolve(name);
    }
    const auto info = at ? Measure::DescribeFaceAt(step, *at) : std::nullopt;
    if (!info)
      return error;
    const glm::dvec3 n = glm::normalize(info->normal);
    CADShape next;
    ElementMap names;
    const std::string why = ReplaceOntoPlane(
        step, *at, name, info->centroid + n * distance, n,
        "its plane moved " + Mm(distance) + " mm", tag, next, names);
    if (!why.empty())
      return error + "; built face by face instead: " + why;
    step.shape = next;
    step.elementMap = names;
  }
  // Each face moved once, under this edit's tag; the faces a later step
  // trimmed again read one ";M<tag>", and an earlier step's moved face is
  // still its own name.
  for (auto& [i, n] : step.elementMap.names)
    n = CollapseSteps(n, tag);
  for (auto& [i, n] : step.elementMap.edgeNames)
    n = CollapseSteps(n, tag);
  RestoreUntouched(body, step.shape, step.elementMap, tag, picked.names);
  if (std::string bad = CheckResult(body, picked, step.shape, step.elementMap,
                                    tag, expect, op);
      !bad.empty())
    return error + "; built face by face instead: " + bad;
  out = step.shape;
  map = step.elementMap;
  return {};
}

} // namespace directedit

bool OffsetFaceFeature::Execute(RegenContext& ctx) {
  if (faces.empty())
    return true; // previewing, nothing picked (WhyIncomplete)
  directedit::Picked picked;
  if (std::string why = directedit::ResolvePicked(
          ctx, bodyId, faces, fingerprints, "offset", picked);
      !why.empty()) {
    SetError(why);
    return false;
  }
  CADShape result;
  ElementMap map;
  if (std::string why =
          directedit::OffsetFaces(picked, distance, GetId(), result, map);
      !why.empty()) {
    SetError(why);
    LOG_ERROR("OffsetFaceFeature '{}': {}", m_name, why);
    return false;
  }
  picked.body->shape = result;
  picked.body->elementMap = map;
  return true;
}
