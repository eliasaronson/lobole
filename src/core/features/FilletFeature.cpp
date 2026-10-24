// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/FilletFeature.h"
#include "core/Measure.h" // InvalidReasons

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRep_Tool.hxx>
#include <ChFi3d.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>

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

  ElementMap map;
  std::string error;
  CADShape result = topo::EdgeBlendNamed(body->shape, body->elementMap, indices,
                                         op, radius, GetId(), map, &error);
  if (!result.IsValid()) {
    SetError(error.empty() ? std::string(what) +
                                 " failed (radius too large for the geometry?)"
                           : error);
    LOG_ERROR("FilletFeature '{}': {}", m_name, GetError());
    return false;
  }

  // Kept only when it is a valid solid: the blend builder reported success
  // on blends that crossed themselves, and the part went on with a body
  // check_geometry called invalid (tools/fuzz.py).
  if (const std::string bad = Measure::InvalidReasons(result); !bad.empty()) {
    SetError(std::string(what) + " produced an invalid solid (" + bad +
             "); a smaller radius, or fewer edges in one " + what +
             ", usually builds");
    return false;
  }
  body->shape = std::move(result);
  body->elementMap = std::move(map);
  return true;
}
