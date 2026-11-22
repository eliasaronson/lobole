// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/FilletFeature.h"
#include "core/Measure.h" // InvalidReasons

#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/HeavyBuild.h" // Cancelled

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ChFi3d.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <map>

FilletFeature::FilletFeature(const std::string& name,
                             std::vector<ElementRef> edges, double radius,
                             EdgeBlendOp op)
    : edges(std::move(edges)), op(op), radius(radius) {
  m_name = name;
}

namespace {

// What the blend builder would make of the picked edges, asked the way it
// asks (ChFi3d_Builder::PerformElement): an edge with a face on one side
// only, a degenerated one, or one whose two faces meet tangentially
// (ChFi3d::IsTangentFaces, OpenCASCADE's own test, so the two cannot
// disagree) starts no blend. When none of them can, the builder threw
// "There are no suitable edges for chamfer or fillet"; this says which edge
// and why. One among others may still be blended as part of another's
// tangent chain, so that is the builder's to answer (topo::EdgeBlendNamed
// refuses an edge it then left out).
std::string EdgeObjection(const Body& body, const std::vector<int>& indices,
                          const std::vector<std::string>& names,
                          const char* what) {
  std::string first;
  const TopoDS_Shape& shape = body.shape.GetShape();
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(shape, TopAbs_EDGE, all);
  TopTools_IndexedDataMapOfShapeListOfShape faces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, faces);
  for (size_t k = 0; k < indices.size(); k++) {
    if (indices[k] < 0 || indices[k] + 1 > all.Extent())
      continue;
    const TopoDS_Edge& e = TopoDS::Edge(all(indices[k] + 1));
    const std::string named = std::string(what) + " edge '" + names[k] + "'";
    std::string no;
    if (BRep_Tool::Degenerated(e)) {
      no = named + " is degenerated (a point): there is nothing to round";
      if (first.empty())
        first = no;
      continue;
    }
    // The two faces as ChFi3d_conexfaces takes them.
    TopoDS_Face f1, f2;
    if (const int at = faces.FindIndex(e); at != 0) {
      for (const TopoDS_Shape& f : faces(at)) {
        if (f1.IsNull()) {
          f1 = TopoDS::Face(f);
        } else {
          f2 = TopoDS::Face(f);
          if (!f2.IsSame(f1) || BRep_Tool::IsClosed(e, f1))
            break;
          f2.Nullify();
        }
      }
    }
    if (f1.IsNull() || f2.IsNull())
      no = named + " has a face on one side only: there is nothing to " +
           "round; pick an edge where two faces meet";
    else if (ChFi3d::IsTangentFaces(e, f1, f2))
      no = named + " is where two faces meet tangentially: there is " +
           "nothing to round; pick an edge where they meet at an angle";
    if (no.empty())
      return {}; // this one starts a blend
    if (first.empty())
      first = no;
  }
  return first;
}

} // namespace

namespace {

// The chamfered edges in the order they are built. A corner where three or
// more of them meet is closed by BRepFilletAPI_MakeChamfer with a plane
// through their ends (a triangle cut off flat), where the bevels' own planes
// meet in a point further out: three distance-3 chamfers at a box corner
// came out 2.25 mm3 short with a face more. Built in stages, every bevel is
// its own plane cut and the corner is mitred, whatever the order: the first
// takes two edges at a vertex, each later one one more. One stage, the
// usual case, when no such corner is picked.
std::vector<std::vector<int>> ChamferStages(const Body& body,
                                            const std::vector<int>& indices) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_VERTEX, vertices);
  const auto endsOf = [&](int idx) {
    TopoDS_Vertex a, b;
    TopExp::Vertices(TopoDS::Edge(edges(idx + 1)), a, b);
    return std::pair{a.IsNull() ? 0 : vertices.FindIndex(a),
                     b.IsNull() ? 0 : vertices.FindIndex(b)};
  };
  std::vector<std::vector<int>> stages;
  std::vector<int> left = indices;
  while (!left.empty()) {
    // Two at a vertex in the first stage, which the builder mitres; after
    // it one, since a corner already bevelled takes only one more at a time.
    const int most = stages.empty() ? 2 : 1;
    std::map<int, int> used; // vertex -> edges of this stage on it
    std::vector<int> stage, later;
    for (int idx : left) {
      const auto [a, b] = endsOf(idx);
      if ((a != 0 && used[a] >= most) ||
          (b != 0 && b != a && used[b] >= most)) {
        later.push_back(idx);
        continue;
      }
      if (a != 0)
        used[a]++;
      if (b != 0 && b != a)
        used[b]++;
      stage.push_back(idx);
    }
    stages.push_back(std::move(stage));
    left = std::move(later);
  }
  return stages;
}

// The edges of `shape` that lie on `original`: what a chamfer at one of its
// ends left of it. Every edge whose ends and middle are on it, to the
// kernel's tolerance, by index in `shape`'s edge map.
std::vector<int> PiecesOf(const TopoDS_Edge& original,
                          const TopoDS_Shape& shape) {
  std::vector<int> out;
  Bnd_Box reach;
  BRepBndLib::Add(original, reach);
  const double tol =
      std::max(10.0 * Precision::Confusion(), BRep_Tool::Tolerance(original));
  reach.Enlarge(tol);
  const auto onOriginal = [&](const gp_Pnt& p) {
    BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(),
                                 original);
    return d.IsDone() && d.Value() <= tol;
  };
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); i++) {
    const TopoDS_Edge e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e))
      continue;
    Bnd_Box box;
    BRepBndLib::Add(e, box);
    if (box.IsOut(reach))
      continue;
    BRepAdaptor_Curve c(e);
    const double a = c.FirstParameter(), b = c.LastParameter();
    if (onOriginal(c.Value(a)) && onOriginal(c.Value(0.5 * (a + b))) &&
        onOriginal(c.Value(b)))
      out.push_back(i - 1);
  }
  return out;
}

} // namespace

std::string FilletFeature::WhyRefusedOn(const std::vector<Body>& input,
                                        const Document& doc) const {
  (void)doc;
  if (edges.empty())
    return {};
  const char* what = op == EdgeBlendOp::Chamfer ? "chamfer" : "fillet";
  for (const ElementRef& ref : edges)
    if (ref.featureId != edges[0].featureId)
      return "all edges of one blend must belong to the same body";
  std::string why;
  const Body* body = topo::FindOwningBodyOfAll(
      input, edges, topo::ElementKind::Edge, "edge", &why);
  if (body == nullptr)
    return why;
  std::vector<int> indices;
  std::vector<std::string> names;
  for (const ElementRef& ref : edges) {
    indices.push_back(*body->elementMap.ResolveEdge(ref.name));
    names.push_back(ref.name);
  }
  if (std::string no = EdgeObjection(*body, indices, names, what); !no.empty())
    return no;
  if (!(radius > 1e-9))
    return std::string(what) + " radius must be positive";
  // The edges added to the blend builder, not built: its tangent-chain walk
  // and the contours it takes them into, in milliseconds where the build
  // can take seconds (the fuzz/30 chain it could not follow round a seam).
  return topo::EdgeBlendRefusal(body->shape, body->elementMap, indices, op,
                                radius);
}

bool FilletFeature::Execute(RegenContext& ctx) {
  if (edges.empty()) {
    return true; // previewing: no edges picked yet, nothing to do
  }
  const char* what = op == EdgeBlendOp::Chamfer ? "chamfer" : "fillet";

  const uint64_t bodyFeature = edges[0].featureId;
  for (const ElementRef& ref : edges) {
    if (ref.featureId != bodyFeature) {
      SetError("all edges of one blend must belong to the same body");
      return false;
    }
  }
  // The one body whose map names the edges: a pattern's copies and a split's
  // pieces share the createdBy, and the first of them is not always it.
  // Every name that is gone (deleted entity, consumed edge) is a first-class
  // failure with reattach, never a silent renumber.
  std::string why;
  Body* body = topo::FindOwningBodyOfAll(ctx, edges, topo::ElementKind::Edge,
                                         "edge", &why);
  if (body == nullptr) {
    SetError(why);
    return false;
  }
  std::vector<int> indices;
  std::vector<std::string> names;
  for (const ElementRef& ref : edges) {
    const int idx = *body->elementMap.ResolveEdge(ref.name);
    if (std::find(indices.begin(), indices.end(), idx) == indices.end()) {
      indices.push_back(idx); // two refs can land on one edge; blend it once
      names.push_back(ref.name);
    }
  }

  if (radius <= 1e-9) {
    SetError(std::string(what) + " radius must be positive");
    return false;
  }
  if (std::string no = EdgeObjection(*body, indices, names, what);
      !no.empty()) {
    SetError(no);
    return false;
  }

  // A size past a bound that refuses (fit::Refuses) is not built: the
  // builder's own failure far past the limit was most of what a refusal
  // cost (docs/modeling/README.md#bounds-that-refuse).
  const double asked = radius.value;
  const std::optional<fit::Bound> past = fit::RefusingBound(
      fit::RefusingBounds(*body, indices, op,
                          asked / (1.0 + fit::kRefuseMargin)),
      asked);
  ElementMap map;
  std::string error;
  topo::EdgeBlendFailure failure;
  CADShape result;
  if (!past && op == EdgeBlendOp::Chamfer) {
    // In stages (ChamferStages). A later stage's edges are what the stages
    // before it left of the picked ones: shortened at the corner, and named
    // afresh, so they are found by place, on the edge they were cut from.
    const std::vector<std::vector<int>> stages = ChamferStages(*body, indices);
    TopTools_IndexedMapOfShape picked;
    TopExp::MapShapes(body->shape.GetShape(), TopAbs_EDGE, picked);
    result = body->shape;
    map = body->elementMap;
    for (size_t k = 0; k < stages.size() && result.IsValid(); k++) {
      std::vector<int> at;
      if (k == 0) {
        at = stages[0];
      } else {
        for (int idx : stages[k]) {
          const std::vector<int> left =
              PiecesOf(TopoDS::Edge(picked(idx + 1)), result.GetShape());
          if (left.empty()) {
            const std::string* name = body->elementMap.EdgeNameOf(idx);
            error = "chamfer edge '" + (name != nullptr ? *name : "?") +
                    "' was consumed by the chamfers built before it at a "
                    "corner of three";
            result = CADShape();
            break;
          }
          at.insert(at.end(), left.begin(), left.end());
        }
        if (!result.IsValid())
          break;
      }
      ElementMap next;
      result = topo::EdgeBlendNamed(result, map, at, op, radius, GetId(), next,
                                    &error, &failure);
      map = std::move(next);
    }
  } else if (!past) {
    result = topo::EdgeBlendNamed(body->shape, body->elementMap, indices, op,
                                  radius, GetId(), map, &error, &failure);
  }
  std::string cause;
  if (past) {
    cause = past->why;
  } else if (!result.IsValid()) {
    cause = error.empty() ? std::string(what) + " failed" : error;
    if (!failure.atBuild) {
      // The edges, not the size: no size would build.
      SetError(cause);
      LOG_ERROR("FilletFeature '{}': {}", m_name, GetError());
      return false;
    }
  } else if (const std::string bad = Measure::InvalidReasons(result);
             !bad.empty()) {
    // Kept only when it is a valid solid: the blend builder reported success
    // on blends that crossed themselves, and the part went on with a body
    // check_geometry called invalid (tools/fuzz.py).
    cause = std::string(what) + " produced an invalid solid (" + bad + ")";
  } else {
    body->shape = std::move(result);
    body->elementMap = std::move(map);
    return true;
  }

  // The size does not fit: say the one that does
  // (docs/modeling/README.md#the-size-that-fits).
  std::optional<fit::Answer> answer =
      fitMemo.For(body->shape, indices, static_cast<int>(op), asked);
  if (!answer) {
    const CADShape& input = body->shape;
    // Past a refusing bound it is the seed: the least that refuses, and
    // nothing above it builds. Measuring every face as well cost more than
    // the one build under it (100 rims of a plate: 0.35 s against 0.3 s).
    const std::optional<fit::Bound> least =
        past ? past : fit::Least(fit::BlendBounds(*body, indices, op));
    answer = fit::LargestThatBuilds(
        asked, least,
        [&](double size) {
          const CADShape built =
              topo::EdgeBlendUnnamed(input, indices, op, size);
          return built.IsValid() && Measure::InvalidReasons(built).empty();
        },
        [] { return heavy::Cancelled(); },
        // A bound OpenCASCADE has been seen to build past is a first
        // guess, and the search goes on above it when it builds.
        {.seedIsBound = least && fit::Refuses(least->kind)});
    fitMemo = fit::Memo{body->shape, indices, static_cast<int>(op), *answer};
  }
  if (past)
    SetError(fit::BoundSentence(op, *answer, *past, failure.places),
             fit::BoundHint(*answer));
  else
    SetError(fit::Sentence(op, *answer, failure.places, cause),
             fit::HintOf(*answer));
  LOG_ERROR("FilletFeature '{}': {}", m_name, GetError());
  return false;
}
