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
#include <BOPTools_AlgoTools3D.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <IntTools_Context.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
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

// A point inside `f`, as parameters (the kernel's own search: a face's
// parameter box can miss a thin band of a face altogether -- a shelled
// box's rim, a thin extrusion's end).
std::optional<gp_Pnt2d> InteriorUV(const TopoDS_Face& f) {
  gp_Pnt p;
  gp_Pnt2d uv;
  const Handle(IntTools_Context) context = new IntTools_Context();
  if (BOPTools_AlgoTools3D::PointInFace(f, p, uv, context) != 0)
    return std::nullopt;
  return uv;
}

// Whether `piece` (a piece of `face`, as a cell holds it) faces the way
// `face` does in its body: the cosine of the two outward normals at a point
// of the piece, nullopt where there is no such point.
std::optional<double> SameSide(const TopoDS_Face& piece,
                               const TopoDS_Face& face) {
  const auto uv = InteriorUV(piece);
  if (!uv)
    return std::nullopt;
  gp_Pnt p, q;
  gp_Vec n, m;
  BRepGProp_Face(piece).Normal(uv->X(), uv->Y(), p, n);
  GeomAPI_ProjectPointOnSurf proj(p, BRep_Tool::Surface(face));
  if (!proj.IsDone() || proj.NbPoints() == 0)
    return std::nullopt;
  double fu = 0.0, fv = 0.0;
  proj.LowerDistanceParameters(fu, fv);
  BRepGProp_Face(face).Normal(fu, fv, q, m);
  if (n.Magnitude() < 1e-12 || m.Magnitude() < 1e-12)
    return std::nullopt;
  return n.Dot(m) / (n.Magnitude() * m.Magnitude());
}

// The fuse leaves each neighbour and the strip the cell added to it as two
// faces on one surface, meeting along the replaced face's old edge, and a
// target that crosses the face (a tilt) leaves the piece the fuse added and
// the piece the cut uncovered as two faces on the target, meeting where it
// crossed: each merged back into one, across those edges only. Every edge
// and vertex of a face with no such edge is kept, so no other face is
// rebuilt or renamed (two coplanar faces an imported body carries side by
// side stay two).
TopoDS_Shape MergeAcross(const TopoDS_Shape& result, const TopoDS_Face& gone,
                         const TopoDS_Face* onto, double tolOnto,
                         BRepTools_History& history) {
  std::vector<TopoDS_Edge> old;
  double tol = Precision::Confusion();
  for (TopExp_Explorer ex(gone, TopAbs_EDGE); ex.More(); ex.Next()) {
    old.push_back(TopoDS::Edge(ex.Current()));
    tol = std::max(tol, BRep_Tool::Tolerance(old.back()));
  }
  tol = 2.0 * tol + 1e-7;
  Bnd_Box around;
  BRepBndLib::Add(gone, around);
  around.Enlarge(tol);
  const auto onOld = [&](const TopoDS_Edge& e) {
    if (BRep_Tool::Degenerated(e))
      return false;
    Bnd_Box box;
    BRepBndLib::Add(e, box);
    if (around.IsOut(box))
      return false;
    BRepAdaptor_Curve c(e);
    const gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(mid);
    for (const TopoDS_Edge& o : old) {
      BRepExtrema_DistShapeShape dist(v, o);
      if (dist.IsDone() && dist.Value() <= tol)
        return true;
    }
    return false;
  };
  // Whether a face lies on the target's surface: its type first, then a
  // point of it.
  const Handle(Geom_Surface) target =
      onto != nullptr ? BRep_Tool::Surface(*onto) : nullptr;
  const GeomAbs_SurfaceType targetType =
      onto != nullptr ? BRepAdaptor_Surface(*onto, Standard_False).GetType()
                      : GeomAbs_OtherSurface;
  TopTools_DataMapOfShapeInteger onTarget;
  const auto isOnTarget = [&](const TopoDS_Shape& s) {
    if (const int* known = onTarget.Seek(s))
      return *known != 0;
    const TopoDS_Face f = TopoDS::Face(s);
    bool on = false;
    if (BRepAdaptor_Surface(f, Standard_False).GetType() == targetType)
      if (const auto uv = InteriorUV(f)) {
        const gp_Pnt p = BRep_Tool::Surface(f)->Value(uv->X(), uv->Y());
        GeomAPI_ProjectPointOnSurf proj(p, target);
        on = proj.IsDone() && proj.NbPoints() > 0 &&
             proj.LowerDistance() <= tolOnto;
      }
    onTarget.Bind(s, on ? 1 : 0);
    return on;
  };
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndUniqueAncestors(result, TopAbs_EDGE, TopAbs_FACE,
                                      edgeFaces);
  // The faces either side of a seam, and the seams' ends: where a tilted
  // target crossed a side, that side's edge is two pieces end to end, one
  // again once the vertex between them is free to go.
  TopTools_MapOfShape touched, ends;
  for (int i = 1; i <= edgeFaces.Extent(); i++) {
    const TopTools_ListOfShape& two = edgeFaces(i);
    bool seam = onOld(TopoDS::Edge(edgeFaces.FindKey(i)));
    if (!seam && onto != nullptr && two.Extent() == 2) {
      seam = true;
      for (const TopoDS_Shape& f : two)
        seam = seam && isOnTarget(f);
    }
    if (seam) {
      for (const TopoDS_Shape& f : two)
        touched.Add(f);
      for (TopExp_Explorer vx(edgeFaces.FindKey(i), TopAbs_VERTEX); vx.More();
           vx.Next())
        ends.Add(vx.Current());
    }
  }
  if (touched.IsEmpty())
    return TopoDS_Shape(); // nothing to merge: as it is
  TopTools_MapOfShape keep;
  for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next()) {
    if (touched.Contains(ex.Current()))
      continue;
    for (TopExp_Explorer sx(ex.Current(), TopAbs_EDGE); sx.More(); sx.Next())
      keep.Add(sx.Current());
    for (TopExp_Explorer sx(ex.Current(), TopAbs_VERTEX); sx.More(); sx.Next())
      if (!ends.Contains(sx.Current()))
        keep.Add(sx.Current());
  }
  try {
    ShapeUpgrade_UnifySameDomain unify(result, /*UnifyEdges=*/Standard_True,
                                       /*UnifyFaces=*/Standard_True,
                                       /*ConcatBSplines=*/Standard_False);
    unify.KeepShapes(keep);
    unify.Build();
    // A merge the kernel gets wrong (a bore's strips merged into the bore of
    // a ball knob came back invalid) leaves the strips as they are: two
    // faces on one surface are untidy, not wrong.
    if (unify.Shape().IsNull() || !BRepCheck_Analyzer(unify.Shape()).IsValid())
      return TopoDS_Shape();
    history.Merge(unify.History());
    return unify.Shape();
  } catch (const Standard_Failure& e) {
    LOG_WARN("Replace Face: the strips were not merged into their faces ({})",
             e.GetMessageString());
    return TopoDS_Shape();
  }
}

} // namespace

namespace directedit {

std::string ReplaceOnto(
    const Body& body, int replaced, const std::string& faceName,
    const std::function<TopoDS_Face(const glm::dvec3&, double)>& makeOnto,
    const std::string& label, uint64_t tag, CADShape& out, ElementMap& map,
    const std::string& op) {
  const std::string described = DescribeFace(body, replaced, faceName);
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi, /*tight=*/true);
  const double diag = glm::length(hi - lo);
  const glm::dvec3 centre = 0.5 * (lo + hi);

  const Copied copy = CopyFor(body, {});
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(copy.shape, TopAbs_FACE, faces);
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

  // The cells are built from the replaced face, its neighbours extended and
  // the target -- nothing else of the body. Built from every face, the cost
  // grew with the body (25 s on a 291-face bracket), and the target, as
  // large as the body, cut every other wall that crosses it into cells of
  // their own.
  const TopoDS_Face onto = makeOnto(centre, 2.0 * diag);
  if (onto.IsNull()) {
    return op + " could not build the surface of " + label;
  }
  // How far a planar neighbour is grown past the face: what it takes to
  // reach the target from the face's boundary (four times the farthest
  // point, for a neighbour that meets it at a slant), and no more.
  double reach = 0.0;
  {
    const Handle(Geom_Surface) to = BRep_Tool::Surface(onto);
    for (TopExp_Explorer ex(gone, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge e = TopoDS::Edge(ex.Current());
      if (BRep_Tool::Degenerated(e))
        continue;
      BRepAdaptor_Curve c(e);
      for (int k = 0; k <= 8; k++) {
        const gp_Pnt p =
            c.Value(c.FirstParameter() +
                    (c.LastParameter() - c.FirstParameter()) * k / 8.0);
        GeomAPI_ProjectPointOnSurf proj(p, to);
        reach = proj.IsDone() && proj.NbPoints() > 0
                    ? std::max(reach, proj.LowerDistance())
                    : diag;
      }
    }
  }
  const double grow = std::min(diag, 4.0 * reach + 1e-3 * diag + 1e-3);

  TopoDS_Compound input;
  BRep_Builder builder;
  builder.MakeCompound(input);
  std::vector<int> untouched;
  std::vector<std::pair<int, TopoDS_Face>> extended;
  for (int i = 0; i < faces.Extent(); i++) {
    if (i == replaced)
      continue;
    if (!neighbours.count(i)) {
      untouched.push_back(i);
      continue;
    }
    const TopoDS_Face f = TopoDS::Face(faces(i + 1));
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
        BRepTools::UVBounds(f, TopoDS::Edge(ex.Current()), eu0, eu1, ev0, ev1);
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
    // A plane is grown only as far as the target is (see `grow`): grown by
    // the body's diagonal, a chamfer beside the face ran on through the
    // whole volume between the face and its target and cut it into pieces,
    // of which only the one touching the face was kept. A curved one goes
    // all the way: a sphere's dent grown by the target's distance closes
    // nothing near its poles, and the cell runs down into the dent.
    const bool planar =
        BRepAdaptor_Surface(f, Standard_False).GetType() == GeomAbs_Plane;
    TopoDS_Face ext;
    const std::string why = Guarded(op, [&] {
      BRepLib::ExtendFace(f, planar ? grow : diag, uMin, uMax, vMin, vMax, ext);
    });
    if (!why.empty() || ext.IsNull()) {
      return op + " could not extend " + DescribeFace(body, i, nameOf(i)) +
             " to meet " + label;
    }
    builder.Add(input, ext);
    extended.emplace_back(i, ext);
  }
  builder.Add(input, onto);
  builder.Add(input, gone);

  // One deadline for the cells and the booleans that splice them in.
  const Handle(Message_ProgressIndicator) deadline =
      MakeDeadline(BuilderBudgetS(1));
  const auto gaveUp = [&] {
    return op + " of " + described + " onto " + label + " gave up after " +
           std::to_string(int(BuilderBudgetS(1))) +
           " s: the faces around it are too costly to intersect";
  };
  BOPAlgo_MakerVolume maker;
  std::string why = Guarded(op + " of " + described, [&] {
    TopTools_ListOfShape args;
    for (TopExp_Explorer ex(input, TopAbs_FACE); ex.More(); ex.Next())
      args.Append(ex.Current());
    maker.SetArguments(args);
    maker.SetRunParallel(Standard_False);
    maker.SetIntersect(Standard_True);
    maker.Perform(deadline->Start());
  });
  if (why.empty() && Expired(deadline))
    why = gaveUp();
  if (why.empty() && maker.HasErrors())
    why = op + " of " + described + " onto " + label +
          " did not build: the faces do not close a volume";
  if (!why.empty()) {
    return why;
  }

  // The cells between the face and its target. Which side of the face a
  // cell lies on is told by the outward normals of its pieces against the
  // face's own in the body, never by classifying a point, which an imported
  // face's tolerance would blur.
  TopTools_MapOfShape fromGone, fromOnto;
  const auto images = [&](const TopoDS_Shape& s, TopTools_MapOfShape& into) {
    const TopTools_ListOfShape& mods = maker.Modified(s);
    if (mods.IsEmpty())
      into.Add(s);
    for (const TopoDS_Shape& m : mods)
      into.Add(m);
  };
  images(gone, fromGone);
  images(onto, fromOnto);
  TopoDS_Compound adds, takes;
  builder.MakeCompound(adds);
  builder.MakeCompound(takes);
  int nAdd = 0, nTake = 0;
  for (TopExp_Explorer ex(maker.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
    // Every piece of the face in the cell, and every piece of the target,
    // the cell's outward normal against the face's own: +1 the same way,
    // -1 the other, 0 when the pieces disagree or none can be read.
    const auto sideOf = [&](const TopTools_MapOfShape& from) {
      int side = 0;
      for (TopExp_Explorer fx(ex.Current(), TopAbs_FACE); fx.More();
           fx.Next()) {
        if (!from.Contains(fx.Current()))
          continue;
        const std::optional<double> c =
            SameSide(TopoDS::Face(fx.Current()), gone);
        const int s = !c || std::abs(*c) < 1e-6 ? 0 : *c > 0.0 ? 1 : -1;
        if (s == 0 || (side != 0 && s != side))
          return 0;
        side = s;
      }
      return side;
    };
    // Between the two: a cell on the face's inner side (its piece faces the
    // way the face does) that the target closes on the far side (its piece
    // facing back) is material taken away, and the mirror of that material
    // added. A cell that holds both and is not between them -- the rest of
    // a revolved body's inside, closed by a target that runs all the way
    // round -- is neither.
    const int atFace = sideOf(fromGone);
    const int atTarget = sideOf(fromOnto);
    if (atFace == 1 && atTarget == -1) {
      builder.Add(takes, ex.Current());
      nTake++;
    } else if (atFace == -1 && atTarget == 1) {
      builder.Add(adds, ex.Current());
      nAdd++;
    }
  }
  const auto unreached = [&]() {
    std::vector<std::string> miss;
    for (const auto& [i, ext] : extended)
      if (!Touches(ext, onto))
        miss.push_back(nameOf(i));
    return miss;
  };
  if (nAdd + nTake == 0) {
    const auto miss = unreached();
    return op + " of " + described + " onto " + label +
           (miss.empty()
                ? std::string(" closes no volume")
                : ": " + NameList(miss) + ", extended, do not reach it");
  }

  // The target's pieces carry the face's name, moved; a neighbour's strip
  // is not named, merging into the neighbour, which names it.
  const std::string mine = faceName + ";M" + std::to_string(tag);
  // Where a neighbour meets the target the edge is "<neighbour>;G<id>", as
  // a builder's history names an edge a face generates.
  TopTools_DataMapOfShapeInteger fromExtended;
  for (const auto& [i, ext] : extended) {
    const TopTools_ListOfShape& mods = maker.Modified(ext);
    if (mods.IsEmpty())
      fromExtended.Bind(ext, i);
    for (const TopoDS_Shape& m : mods)
      fromExtended.Bind(m, i);
  }
  const std::string generated = ";G" + std::to_string(tag);
  const auto cellNames = [&](const TopoDS_Shape& cells) {
    ElementMap m;
    TopTools_IndexedMapOfShape cf, ce;
    TopExp::MapShapes(cells, TopAbs_FACE, cf);
    TopExp::MapShapes(cells, TopAbs_EDGE, ce);
    for (int k = 1; k <= cf.Extent(); k++)
      if (fromOnto.Contains(cf(k)))
        m.names[k - 1] = mine;
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndUniqueAncestors(cells, TopAbs_EDGE, TopAbs_FACE,
                                        edgeFaces);
    for (int k = 1; k <= edgeFaces.Extent(); k++) {
      bool onTarget = false;
      const int* by = nullptr;
      for (const TopoDS_Shape& f : edgeFaces(k)) {
        onTarget = onTarget || fromOnto.Contains(f);
        if (const int* i = fromExtended.Seek(f))
          by = i;
      }
      if (onTarget && by != nullptr)
        if (const int e = ce.FindIndex(edgeFaces.FindKey(k)); e > 0)
          m.edgeNames[e - 1] = nameOf(*by) + generated;
    }
    return m;
  };
  CADShape result = body.shape;
  map = body.elementMap;
  const auto splice = [&](BooleanOp bop, const TopoDS_Shape& cells,
                          const topo::RefineResult& refine) -> std::string {
    ElementMap next;
    std::string err;
    CADShape made;
    std::string fault = Guarded(op + " of " + described, [&] {
      made = topo::BooleanNamed(bop, result, map, CADShape(cells),
                                cellNames(cells), tag, next, &err, 0.0, refine,
                                deadline->Start());
    });
    if (fault.empty() && Expired(deadline))
      return gaveUp();
    if (!fault.empty())
      return fault;
    if (!made.IsValid())
      return op + " of " + described + " onto " + label +
             " did not join the body" + (err.empty() ? "" : " (" + err + ")");
    result = made;
    map = next;
    return {};
  };
  const double tolOnto = 1e-7 * diag + Precision::Confusion();
  // The target's two halves are merged only where it crossed the face: a
  // face moved flush with a plateau beside it stays two faces, the
  // plateau keeping its name.
  const bool crossed = nAdd > 0 && nTake > 0;
  const topo::RefineResult merge = [&](const TopoDS_Shape& r,
                                       BRepTools_History& h) {
    return MergeAcross(r, gone, nullptr, tolOnto, h);
  };
  const topo::RefineResult mergeHalves = [&](const TopoDS_Shape& r,
                                             BRepTools_History& h) {
    return MergeAcross(r, gone, &onto, tolOnto, h);
  };
  if (nAdd > 0)
    if (std::string w = splice(BooleanOp::Union, adds, merge); !w.empty())
      return w;
  if (nTake > 0)
    if (std::string w =
            splice(BooleanOp::Subtract, takes, crossed ? mergeHalves : merge);
        !w.empty())
      return w;
  for (auto& [i, n] : map.names)
    n = CollapseSteps(n, tag);
  // An edge this edit generated and its own merge rebuilt is still the
  // edge it generated: "<neighbour>;G<id>", not "…;G<id>;M<id>".
  const std::string rebuilt = generated + ";M" + std::to_string(tag);
  for (auto& [i, n] : map.edgeNames) {
    n = CollapseSteps(n, tag);
    for (size_t at = n.find(rebuilt); at != std::string::npos;
         at = n.find(rebuilt, at)) {
      const size_t after = at + rebuilt.size();
      if (after == n.size() || n[after] == ';' || n[after] == '[' ||
          n[after] == '.' || n[after] == ',' || n[after] == '#')
        n.erase(at + generated.size(), rebuilt.size() - generated.size());
      else
        at = after;
    }
  }
  RestoreUntouched(body, result, map, tag, {faceName});

  // The untouched faces keep their names and geometry; the target must be
  // there.
  {
    std::set<std::string> have;
    for (const auto& [i, n] : map.names)
      have.insert(n);
    for (const int i : untouched)
      if (!have.count(nameOf(i))) {
        return op + " of " + described + " onto " + label + " cuts " +
               DescribeFace(body, i, nameOf(i)) +
               ", which it should leave alone";
      }
    const bool landed =
        std::any_of(map.names.begin(), map.names.end(), [&](const auto& kv) {
          return kv.second.compare(0, mine.size(), mine) == 0;
        });
    if (!landed) {
      const auto miss = unreached();
      return op + " of " + described + " onto " + label +
             (miss.empty()
                  ? std::string(": the target does not bound the "
                                "body there")
                  : ": " + NameList(miss) + ", extended, do not reach it");
    }
  }
  out = result;
  return {};
}

std::string ReplaceOntoSurfaceOf(const Body& body, int index,
                                 const std::string& name,
                                 const TopoDS_Face& surface,
                                 const std::string& label, uint64_t tag,
                                 CADShape& out, ElementMap& map,
                                 const std::string& op) {
  Target target;
  target.face = surface;
  return ReplaceOnto(
      body, index, name,
      [&](const glm::dvec3& centre, double half) {
        return TargetFace(target, centre, half);
      },
      label, tag, out, map, op);
}

std::string ReplaceOntoPlane(const Body& body, int index,
                             const std::string& name, const glm::dvec3& origin,
                             const glm::dvec3& normal, const std::string& label,
                             uint64_t tag, CADShape& out, ElementMap& map,
                             const std::string& op) {
  const gp_Pln pln(gp_Pnt(origin.x, origin.y, origin.z),
                   gp_Dir(normal.x, normal.y, normal.z));
  return ReplaceOnto(
      body, index, name,
      [&](const glm::dvec3& centre, double half) {
        return BoundedPlane(pln, centre, half);
      },
      label, tag, out, map, op);
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
  if (std::string why = directedit::FreeformNeighbour(picked, "Replace Face",
                                                      "extends or trims");
      !why.empty()) {
    SetError(why);
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
