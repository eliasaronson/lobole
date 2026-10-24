// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/ReplaceFaceFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/features/DatumPlaneFeature.h" // ResolveParentPlane
#include "core/features/OffsetFaceFeature.h"
#include "core/features/SketchFeature.h"

#include <BOPAlgo_MakerVolume.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

ReplaceFaceFeature::ReplaceFaceFeature(const std::string& name, uint64_t bodyId,
                                       const ElementRef& face)
    : bodyId(bodyId), face(face) {
  m_name = name;
}

namespace {

TopoDS_Face FaceAt(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++)
    if (i == index)
      return TopoDS::Face(ex.Current());
  return {};
}

// The target, as a surface to build on: a plane, or a face's whole surface.
struct Target {
  bool planar = false;
  gp_Pln plane;
  TopoDS_Face face; // non-planar: the face whose surface it is
  std::string label;
};

TopoDS_Face BoundedPlane(const gp_Pln& pln, const glm::dvec3& centre,
                         double half) {
  // Centred on the body: the plane's own origin may be far from it.
  const gp_Pnt c(centre.x, centre.y, centre.z);
  gp_Pln at = pln;
  const gp_Pnt o = pln.Location();
  const gp_Dir n = pln.Axis().Direction();
  const gp_Vec oc(o, c);
  const double h = oc.Dot(gp_Vec(n));
  at.SetLocation(c.Translated(gp_Vec(n) * -h));
  return BRepBuilderAPI_MakeFace(at, -half, half, -half, half).Face();
}

// The target's surface, bounded to `half` around the body: a plane as a
// square, a surface of revolution all the way round, anything else extended
// past its own edges.
TopoDS_Face TargetFace(const Target& t, const glm::dvec3& centre, double half) {
  if (t.planar)
    return BoundedPlane(t.plane, centre, half);
  BRepAdaptor_Surface s(t.face);
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(t.face);
  double u0, u1, v0, v1;
  BRepTools::UVBounds(t.face, u0, u1, v0, v1);
  switch (s.GetType()) {
  case GeomAbs_Cylinder:
  case GeomAbs_Cone:
    return BRepBuilderAPI_MakeFace(surf, 0.0, 2.0 * M_PI, v0 - half, v1 + half,
                                   1e-7)
        .Face();
  case GeomAbs_Sphere:
    return BRepBuilderAPI_MakeFace(surf, 0.0, 2.0 * M_PI, -M_PI / 2, M_PI / 2,
                                   1e-7)
        .Face();
  default: {
    TopoDS_Face out;
    BRepLib::ExtendFace(t.face, half, Standard_True, Standard_True,
                        Standard_True, Standard_True, out);
    return out;
  }
  }
}

bool Touches(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  BRepAlgoAPI_Section sec(a, b, Standard_False);
  sec.Approximation(Standard_False);
  sec.Build();
  if (!sec.IsDone())
    return false;
  TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE);
  return ex.More();
}

} // namespace

namespace directedit {

std::string ReplaceOnto(
    const Body& body, int replaced, const std::string& faceName,
    const std::function<TopoDS_Face(const glm::dvec3&, double)>& makeOnto,
    const std::string& label, uint64_t tag, CADShape& out, ElementMap& map) {
  const std::string described = DescribeFace(body, replaced, faceName);
  // Otherwise: the cells of the untouched faces, the neighbours extended and
  // the target, and the one on the untouched faces' inner side.
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi, /*tight=*/true);
  const double diag = glm::length(hi - lo);
  const glm::dvec3 centre = 0.5 * (lo + hi);

  const Copied copy = CopyFor(body, {});
  TopTools_IndexedMapOfShape faces, edges;
  TopExp::MapShapes(copy.shape, TopAbs_FACE, faces);
  TopExp::MapShapes(copy.shape, TopAbs_EDGE, edges);
  const TopoDS_Face gone = FaceAt(copy.shape, replaced);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(copy.shape, TopAbs_EDGE, TopAbs_FACE, parents);
  std::set<int> neighbours;
  for (TopExp_Explorer ex(gone, TopAbs_EDGE); ex.More(); ex.Next()) {
    const int at = parents.FindIndex(ex.Current());
    if (at == 0)
      continue;
    for (const TopoDS_Shape& f : parents(at))
      if (!f.IsSame(gone))
        neighbours.insert(faces.FindIndex(f) - 1);
  }
  // Face indices of the copy are the body's (ElementMap's explorer order and
  // MapShapes agree on a solid's faces).
  const auto nameOf = [&](int i) {
    const std::string* n = body.elementMap.NameOf(i);
    return n ? *n : "face " + std::to_string(i);
  };

  TopoDS_Compound input;
  BRep_Builder builder;
  builder.MakeCompound(input);
  ElementMap inputMap; // names on `input`, filled once it is complete
  std::vector<std::pair<TopoDS_Shape, std::string>> named;
  std::vector<int> untouched;
  std::vector<std::pair<int, TopoDS_Face>> extended;
  for (int i = 0; i < faces.Extent(); i++) {
    if (i == replaced)
      continue;
    const TopoDS_Face f = TopoDS::Face(faces(i + 1));
    if (neighbours.count(i)) {
      // Extended across the edges it shares with the replaced face only:
      // grown every way, a neighbour runs on through the faces the edit
      // leaves alone (a cradle's end wall through the bottom) and cuts them.
      bool uMin = false, uMax = false, vMin = false, vMax = false;
      {
        double u0, u1, v0, v1;
        BRepTools::UVBounds(f, u0, u1, v0, v1);
        const double tu = 1e-6 * (u1 - u0) + 1e-9;
        const double tv = 1e-6 * (v1 - v0) + 1e-9;
        bool any = false;
        for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
          const int at = parents.FindIndex(ex.Current());
          if (at == 0)
            continue;
          bool shared = false;
          for (const TopoDS_Shape& g : parents(at))
            shared = shared || g.IsSame(gone);
          if (!shared)
            continue;
          double eu0, eu1, ev0, ev1;
          BRepTools::UVBounds(f, TopoDS::Edge(ex.Current()), eu0, eu1, ev0,
                              ev1);
          bool here = false;
          if (eu1 - eu0 <= tu && std::abs(eu0 - u0) <= tu)
            here = uMin = true;
          if (eu1 - eu0 <= tu && std::abs(eu1 - u1) <= tu)
            here = uMax = true;
          if (ev1 - ev0 <= tv && std::abs(ev0 - v0) <= tv)
            here = vMin = true;
          if (ev1 - ev0 <= tv && std::abs(ev1 - v1) <= tv)
            here = vMax = true;
          any = any || here;
          if (!here) // a curved boundary: no one side to grow from
            uMin = uMax = vMin = vMax = true;
        }
        if (!any)
          uMin = uMax = vMin = vMax = true;
      }
      TopoDS_Face ext;
      const std::string why = Guarded("Replace Face", [&] {
        BRepLib::ExtendFace(f, diag, uMin, uMax, vMin, vMax, ext);
      });
      if (!why.empty() || ext.IsNull()) {
        return "Replace Face could not extend " +
               DescribeFace(body, i, nameOf(i)) + " to meet " + label;
      }
      builder.Add(input, ext);
      named.emplace_back(ext, nameOf(i));
      extended.emplace_back(i, ext);
    } else {
      builder.Add(input, f);
      named.emplace_back(f, nameOf(i));
      untouched.push_back(i);
    }
  }
  const TopoDS_Face onto = makeOnto(centre, 2.0 * diag);
  if (onto.IsNull()) {
    return "Replace Face could not build the surface of " + label;
  }
  builder.Add(input, onto);
  named.emplace_back(onto, faceName);
  {
    TopTools_IndexedMapOfShape inFaces, inEdges;
    TopExp::MapShapes(input, TopAbs_FACE, inFaces);
    TopExp::MapShapes(input, TopAbs_EDGE, inEdges);
    for (const auto& [s, n] : named)
      if (const int at = inFaces.FindIndex(s); at > 0)
        inputMap.names[at - 1] = n;
    // The untouched faces' edges are the body's own: they keep their names.
    for (int e = 1; e <= inEdges.Extent(); e++)
      if (const int at = edges.FindIndex(inEdges(e)); at > 0)
        if (const std::string* n = body.elementMap.EdgeNameOf(at - 1))
          inputMap.edgeNames[e - 1] = *n;
  }

  BOPAlgo_MakerVolume maker;
  std::string why = Guarded("Replace Face", [&] {
    TopTools_ListOfShape args;
    for (TopExp_Explorer ex(input, TopAbs_FACE); ex.More(); ex.Next())
      args.Append(ex.Current());
    maker.SetArguments(args);
    maker.SetRunParallel(Standard_False);
    maker.SetIntersect(Standard_True);
    maker.Perform();
  });
  if (why.empty() && maker.HasErrors())
    why = "Replace Face of " + described + " onto " + label +
          " did not build: the faces do not close a volume";
  if (!why.empty()) {
    return why;
  }

  // The cells on the inner side of the faces the edit leaves alone (the
  // neighbours, when there is nothing else).
  std::vector<glm::dvec3> probes;
  {
    const std::vector<int>& from =
        untouched.empty()
            ? std::vector<int>(neighbours.begin(), neighbours.end())
            : untouched;
    for (const int i : from) {
      const auto in = Measure::FaceInsetAt(body, i);
      const auto fi = Measure::DescribeFaceAt(body, i);
      const auto thick = ThicknessUnder(body, i);
      if (!in || !fi || !thick)
        continue;
      const double step = std::min(0.5 * *thick, 1e-3 * diag + 1e-4);
      probes.push_back(in->point - glm::normalize(fi->normal) * step);
    }
  }
  std::vector<TopoDS_Shape> cells;
  for (TopExp_Explorer ex(maker.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
    for (const glm::dvec3& p : probes) {
      BRepClass3d_SolidClassifier cls(ex.Current(), gp_Pnt(p.x, p.y, p.z),
                                      1e-7);
      if (cls.State() == TopAbs_IN) {
        cells.push_back(ex.Current());
        break;
      }
    }
  }
  const auto unreached = [&]() {
    std::vector<std::string> miss;
    for (const auto& [i, ext] : extended)
      if (!Touches(ext, onto))
        miss.push_back(nameOf(i));
    return miss;
  };
  if (cells.empty()) {
    const auto miss = unreached();
    return "Replace Face of " + described + " onto " + label +
           (miss.empty()
                ? std::string(" closes no volume")
                : ": " + NameList(miss) + ", extended, do not reach it");
  }
  if (static_cast<int>(cells.size()) != body.shape.SolidCount()) {
    return "Replace Face of " + described + " onto " + label + " leaves " +
           std::to_string(cells.size()) + " separate pieces of the body";
  }
  TopoDS_Shape resultShape = cells.front();
  if (cells.size() > 1) {
    TopoDS_Compound c;
    builder.MakeCompound(c);
    for (const TopoDS_Shape& s : cells)
      builder.Add(c, s);
    resultShape = c;
  }
  const BRepTools_History history(topo::HistoryArguments(input), maker);
  map = NameThrough(input, inputMap, history, resultShape, tag);
  const CADShape result(resultShape);
  RestoreUntouched(body, result, map, tag, {faceName});

  // The untouched faces keep their names and geometry; the target must be
  // there.
  {
    std::set<std::string> have;
    for (const auto& [i, n] : map.names)
      have.insert(n);
    for (const int i : untouched)
      if (!have.count(nameOf(i))) {
        return "Replace Face of " + described + " onto " + label + " cuts " +
               DescribeFace(body, i, nameOf(i)) +
               ", which it should leave alone";
      }
    const std::string mine = faceName + ";M" + std::to_string(tag);
    const bool landed =
        std::any_of(map.names.begin(), map.names.end(), [&](const auto& kv) {
          return kv.second.compare(0, mine.size(), mine) == 0;
        });
    if (!landed) {
      const auto miss = unreached();
      return "Replace Face of " + described + " onto " + label +
             (miss.empty()
                  ? std::string(": the target does not bound the "
                                "body there")
                  : ": " + NameList(miss) + ", extended, do not reach it");
    }
  }
  out = result;
  return {};
}

std::string ReplaceOntoPlane(const Body& body, int index,
                             const std::string& name, const glm::dvec3& origin,
                             const glm::dvec3& normal, const std::string& label,
                             uint64_t tag, CADShape& out, ElementMap& map) {
  const gp_Pln pln(gp_Pnt(origin.x, origin.y, origin.z),
                   gp_Dir(normal.x, normal.y, normal.z));
  return ReplaceOnto(
      body, index, name,
      [&](const glm::dvec3& centre, double half) {
        return BoundedPlane(pln, centre, half);
      },
      label, tag, out, map);
}

} // namespace directedit

bool ReplaceFaceFeature::Execute(RegenContext& ctx) {
  if (face.name.empty())
    return true; // previewing, nothing picked (WhyIncomplete)
  const uint64_t tag = GetId();

  // The target first (it may be a face of this same body).
  Target target;
  std::vector<std::string> keep{face.name};
  if (targetKind == ReplaceTargetKind::Face && !targetFace.name.empty())
    keep.push_back(targetFace.name);
  switch (targetKind) {
  case ReplaceTargetKind::Plane: {
    const auto plane = ResolveParentPlane(ctx, targetId, basePlane);
    if (!plane) {
      SetError("Replace Face: the target plane (feature " +
               std::to_string(targetId) + ") no longer exists");
      return false;
    }
    const glm::dvec3 o = plane->GetOrigin(), n = plane->GetNormal();
    target.planar = true;
    target.plane = gp_Pln(gp_Pnt(o.x, o.y, o.z), gp_Dir(n.x, n.y, n.z));
    target.label = targetId == 0 ? std::string("the ") +
                                       (basePlane == PlaneType::XY   ? "XY"
                                        : basePlane == PlaneType::XZ ? "XZ"
                                                                     : "YZ") +
                                       " plane"
                                 : "datum plane " + std::to_string(targetId);
    break;
  }
  case ReplaceTargetKind::Sketch: {
    const auto* sk = dynamic_cast<const SketchFeature*>(ctx.FindById(targetId));
    if (sk == nullptr) {
      SetError("Replace Face: the target sketch (feature " +
               std::to_string(targetId) + ") no longer exists");
      return false;
    }
    const glm::dvec3 o = sk->plane.GetOrigin(), n = sk->plane.GetNormal();
    target.planar = true;
    target.plane = gp_Pln(gp_Pnt(o.x, o.y, o.z), gp_Dir(n.x, n.y, n.z));
    target.label = "the plane of sketch " + sk->GetName();
    break;
  }
  case ReplaceTargetKind::Face: {
    directedit::Picked on;
    if (std::string why =
            directedit::ResolvePicked(ctx, targetFace.featureId, {targetFace},
                                      fingerprints, "replace onto", on, &keep);
        !why.empty()) {
      SetError("Replace Face target: " + why);
      return false;
    }
    const TopoDS_Face tf = FaceAt(on.body->shape.GetShape(), on.indices[0]);
    BRepAdaptor_Surface s(tf);
    target.label = "`" + targetFace.name + "`";
    if (s.GetType() == GeomAbs_Plane) {
      target.planar = true;
      target.plane = s.Plane();
    } else {
      target.face = tf; // read only: its surface is built on afresh
    }
    break;
  }
  }

  directedit::Picked picked;
  if (std::string why = directedit::ResolvePicked(
          ctx, bodyId, {face}, fingerprints, "replace", picked, &keep);
      !why.empty()) {
    SetError(why);
    return false;
  }
  if (targetKind == ReplaceTargetKind::Face && targetFace.featureId == bodyId &&
      targetFace.name == face.name) {
    SetError("Replace Face: `" + face.name + "` is its own target");
    return false;
  }
  const Body& body = *picked.body;
  const int replaced = picked.indices[0];
  const std::string described =
      directedit::DescribeFace(body, replaced, face.name);
  const auto info = Measure::DescribeFaceAt(body, replaced);

  // A parallel plane onto a plane is an offset by the signed distance.
  if (target.planar && info && info->kind == "plane") {
    const gp_Dir tn = target.plane.Axis().Direction();
    const glm::dvec3 n = glm::normalize(info->normal);
    const double dot = n.x * tn.X() + n.y * tn.Y() + n.z * tn.Z();
    if (std::abs(std::abs(dot) - 1.0) < 1e-9) {
      const gp_Pnt o = target.plane.Location();
      const double d =
          glm::dot(glm::dvec3(o.X(), o.Y(), o.Z()) - info->centroid, n);
      if (std::abs(d) < 1e-7) {
        SetError("Replace Face: " + described + " already lies on " +
                 target.label);
        return false;
      }
      CADShape result;
      ElementMap map;
      if (std::string why = directedit::OffsetFaces(picked, d, tag, result, map,
                                                    "Replace Face");
          !why.empty()) {
        SetError(why);
        return false;
      }
      picked.body->shape = result;
      picked.body->elementMap = map;
      return true;
    }
  }

  CADShape result;
  ElementMap map;
  if (std::string why = directedit::ReplaceOnto(
          body, replaced, face.name,
          [&](const glm::dvec3& centre, double half) {
            return TargetFace(target, centre, half);
          },
          target.label, tag, result, map);
      !why.empty()) {
    SetError(why);
    return false;
  }
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi, /*tight=*/true);
  const double diag = glm::length(hi - lo);
  directedit::Expect expect;
  // A tilt about the face's middle moves no volume at all.
  expect.volume = directedit::Expect::Volume::Any;
  expect.faces = directedit::Expect::Faces::Modified;
  expect.bboxGrowth = diag;
  if (std::string bad = directedit::CheckResult(body, picked, result, map, tag,
                                                expect, "Replace Face");
      !bad.empty()) {
    SetError(bad);
    LOG_ERROR("ReplaceFaceFeature '{}': {}", m_name, bad);
    return false;
  }
  picked.body->shape = result;
  picked.body->elementMap = map;
  return true;
}
