// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/ExternalPrint.h"

#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Measure.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdio>

namespace sketch {
namespace {

// The tolerances of the direct edits' print (directedit::ResolvePicked).
constexpr double kRelative = 1e-6;

bool SameSize(double was, double now) {
  return std::abs(was - now) <= kRelative * std::max(1.0, std::abs(was));
}

double Diagonal(const Body& body) {
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi);
  return std::max(1.0, glm::length(hi - lo));
}

std::string Num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

} // namespace

bool IsFileOwn(const RegenContext& ctx, const std::string& name) {
  const topo::ElementOrigin o = topo::DecodeElementName(name);
  if (!o.ok || o.role != topo::ElementRole::Positional || !o.derived.empty() ||
      !o.modifiedBy.empty() || o.madeBy != o.seedTag)
    return false;
  const Feature* seed = ctx.FindById(o.seedTag);
  return seed != nullptr && seed->TypeName() == "Import";
}

std::optional<ElementPrint> PrintAt(const Body& body, bool face, int index,
                                    const std::string& name) {
  ElementPrint p;
  p.element = name;
  if (face) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (!info)
      return std::nullopt;
    p.shape = info->kind;
    p.size = info->area;
    p.centroid = info->centroid;
    return p;
  }
  const auto info = Measure::DescribeEdgeAt(body, index);
  if (!info)
    return std::nullopt;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  if (index < 0 || index >= edges.Extent())
    return std::nullopt;
  // The curve's centre of mass, not its parametric midpoint: a full
  // circle's midpoint is wherever the file put its seam.
  GProp_GProps props;
  BRepGProp::LinearProperties(edges(index + 1), props);
  const gp_Pnt c = props.CentreOfMass();
  p.shape = info->kind;
  p.size = props.Mass();
  p.centroid = {c.X(), c.Y(), c.Z()};
  return p;
}

bool SamePrint(const ElementPrint& was, const ElementPrint& now,
               double bodyDiagonal) {
  return was.shape == now.shape && SameSize(was.size, now.size) &&
         glm::length(was.centroid - now.centroid) <= kRelative * bodyDiagonal;
}

std::vector<Found> FindPrinted(const RegenContext& ctx, uint64_t featureId,
                               bool face, const ElementPrint& was) {
  std::vector<Found> out;
  for (const Body& body : ctx.bodies) {
    if (body.createdBy != featureId || body.shape.GetShape().IsNull())
      continue;
    const double diagonal = Diagonal(body);
    const auto& names =
        face ? body.elementMap.names : body.elementMap.edgeNames;
    // Size and centre of mass first, from the shapes in hand: describing
    // every element of a servo to find four would cost a second each.
    std::vector<TopoDS_Shape> shapes;
    if (face) {
      for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
           ex.Next())
        shapes.push_back(ex.Current());
    } else {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent(); i++)
        shapes.push_back(edges(i));
    }
    for (const auto& [index, name] : names) {
      if (index < 0 || index >= static_cast<int>(shapes.size()) ||
          !IsFileOwn(ctx, name))
        continue;
      GProp_GProps props;
      if (face)
        BRepGProp::SurfaceProperties(shapes[index], props);
      else
        BRepGProp::LinearProperties(shapes[index], props);
      const gp_Pnt c = props.CentreOfMass();
      if (!SameSize(was.size, props.Mass()) ||
          glm::length(was.centroid - glm::dvec3(c.X(), c.Y(), c.Z())) >
              kRelative * diagonal)
        continue;
      const auto now = PrintAt(body, face, index, name);
      if (now && SamePrint(was, *now, diagonal))
        out.push_back({&body, index, name});
    }
  }
  return out;
}

std::string Describe(const ElementPrint& p, bool face) {
  const std::string at = " at (" + Num(p.centroid.x) + ", " +
                         Num(p.centroid.y) + ", " + Num(p.centroid.z) + ")";
  if (face)
    return "a " + p.shape + " face of " + Num(p.size) + " mm²" + at;
  return "a " + p.shape + " " + Num(p.size) + " mm long" + at;
}

} // namespace sketch
