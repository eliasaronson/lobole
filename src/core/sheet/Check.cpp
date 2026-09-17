#include "core/sheet/Check.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sheet {
namespace {

gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Vec V(const glm::dvec3& v) { return gp_Vec(v.x, v.y, v.z); }

std::string Mm(double v) {
  std::ostringstream s;
  s.precision(2);
  s << std::fixed << v;
  return s.str();
}

bool Inside(const CADShape& shape, const gp_Pnt& p) {
  BRepClass3d_SolidClassifier classify(shape.GetShape(), p, 1e-6);
  return classify.State() == TopAbs_IN;
}

// How far the wall past `b` runs before it ends — either at the body's own
// extent, or at the next bend down the chain, whichever comes first. Taking
// the extent alone would read a chain's whole length as one flange and miss a
// short leg in the middle of it.
double WallLength(const Body& body, const BendRegion& b, size_t self,
                  double t) {
  const gp_Vec child =
      V(b.parentDir) * std::cos(b.angle) + V(b.upDir) * std::sin(b.angle);
  const gp_Pnt tangent = P(b.axisOrigin)
                             .Translated((V(b.parentDir) * std::sin(b.angle) -
                                          V(b.upDir) * std::cos(b.angle)) *
                                         b.radius);
  const auto along = [&](const gp_Pnt& p) {
    return gp_Vec(tangent, p).Dot(child);
  };

  double reach = 0.0;
  TopTools_IndexedMapOfShape verts;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_VERTEX, verts);
  for (int i = 1; i <= verts.Extent(); ++i)
    reach = std::max(reach, along(BRep_Tool::Pnt(TopoDS::Vertex(verts(i)))));

  for (size_t j = 0; j < body.bends.size(); ++j) {
    if (j == self)
      continue;
    const double d = along(P(body.bends[j].axisOrigin)) - body.bends[j].radius;
    if (d > t * 0.5 && d < reach)
      reach = d;
  }
  return reach;
}

} // namespace

CheckResult Check(const Body& body, const SheetMetalRules& rules) {
  CheckResult out;
  // A brake's length, its die set and its back gauge are properties of a
  // SHOP. Saying so beats a clean verdict on a check nobody ran.
  out.notChecked.push_back(
      "bend length against the brake, and collisions with its back gauge - "
      "both need a shop profile, which this build has no concept of");
  if (!body.shape.IsValid() || body.bends.empty())
    return out;

  ArmOccSignalHandlers();
  const double t = rules.thickness;
  const double minR = MinBendRadius(rules.material, t);
  // The leg has to sit on the die to be bent at all. With no die table the
  // die opening is taken as 8t, the usual air-bending default, and the leg
  // has to reach half of it plus a thickness.
  const double dieWidth = 8.0 * t;
  const double minFlange = dieWidth / 2.0 + t;
  // A hole nearer than this to the tangent line distorts into an oval when
  // the metal moves. 1.5t is the common shop minimum.
  const double minHoleToBend = 1.5 * t;

  try {
    for (size_t i = 0; i < body.bends.size(); ++i) {
      const BendRegion& b = body.bends[i];
      out.bendsChecked++;

      // The radius the MATERIAL takes, re-checked here and not only when the
      // flange was made: changing the gauge or the alloy afterwards leaves
      // every existing bend exactly where it was, and a part that was fine
      // in aluminium cracks in stainless.
      // A hem is crushed flat rather than bent round a die, so the die's
      // minimum radius says nothing about it.
      if (!b.crushed && b.radius < minR - 1e-9) {
        out.violations.push_back(
            {"bend_below_minimum_radius",
             "bend " + std::to_string(i + 1) + " has an inside radius of " +
                 Mm(b.radius) + " mm; " + rules.material + " at " +
                 rules.gauge + " takes " + Mm(minR) + " mm at the tightest",
             i + 1, b.innerFace, b.radius, minR});
      }

      const double wall = WallLength(body, b, i, t);
      if (wall > 1e-6 && wall < minFlange - 1e-9) {
        out.violations.push_back(
            {"flange_below_minimum_height",
             "the wall past bend " + std::to_string(i + 1) + " is " + Mm(wall) +
                 " mm; it needs " + Mm(minFlange) + " mm to sit on a " +
                 Mm(dieWidth) + " mm die",
             i + 1, b.childFace, wall, minFlange});
      }

      // Where a bend line stops INSIDE a panel, the metal has nothing to move
      // into and tears the panel beside it. Probing just past each end of the
      // bend line, a thickness back into the panel, answers it directly: if
      // there is material there, nothing relieved it.
      if (b.width > 1e-9) {
        const gp_Pnt tangent =
            P(b.axisOrigin).Translated(V(b.upDir) * -b.radius);
        for (int end = 0; end < 2; ++end) {
          const double at =
              end == 0 ? -std::max(t, 0.5) : b.width + std::max(t, 0.5);
          const gp_Pnt probe = tangent.Translated(
              V(b.axisDir) * at - V(b.parentDir) * t - V(b.upDir) * (t / 2.0));
          if (!Inside(body.shape, probe))
            continue;
          out.violations.push_back(
              {"missing_bend_relief",
               "bend " + std::to_string(i + 1) +
                   " stops inside the panel with no relief at " +
                   (end == 0 ? "its start" : "its end") +
                   "; the metal will tear the panel beside it. A relief of " +
                   Mm(rules.EffectiveReliefWidth()) + " x " +
                   Mm(rules.EffectiveReliefDepth(b.radius)) + " mm clears it",
               i + 1, b.parentFace, 0.0, rules.EffectiveReliefWidth()});
        }
      }
    }

    // Holes: every full circle on the body, against every bend's tangent
    // line. A hole that survives the cut and dies at the brake is the most
    // expensive kind there is, because it is found last.
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
    for (int e = 1; e <= edges.Extent(); ++e) {
      const TopoDS_Edge edge = TopoDS::Edge(edges(e));
      BRepAdaptor_Curve curve(edge);
      if (curve.GetType() != GeomAbs_Circle || !BRep_Tool::IsClosed(edge))
        continue;
      const gp_Circ circ = curve.Circle();
      out.holesChecked++;
      for (size_t i = 0; i < body.bends.size(); ++i) {
        const BendRegion& b = body.bends[i];
        // Distance from the hole's nearest edge to the bend's near tangent
        // line, measured across the panel.
        const gp_Pnt tangent =
            P(b.axisOrigin).Translated(V(b.upDir) * -b.radius);
        const double across =
            gp_Vec(tangent, circ.Location()).Dot(V(b.parentDir));
        // Only holes on the PARENT side, and only ones the bend line
        // actually runs past.
        if (across > 0.0)
          continue;
        const double alongAxis =
            gp_Vec(tangent, circ.Location()).Dot(V(b.axisDir));
        if (alongAxis < -circ.Radius() || alongAxis > b.width + circ.Radius())
          continue;
        const double gap = -across - circ.Radius();
        if (gap >= minHoleToBend - 1e-9)
          continue;
        const std::string* name = nullptr;
        if (const auto* n = body.elementMap.EdgeNameOf(e - 1))
          name = n;
        out.violations.push_back(
            {"hole_too_close_to_bend",
             "a hole of " + Mm(circ.Radius() * 2.0) + " mm sits " + Mm(gap) +
                 " mm from bend " + std::to_string(i + 1) + "; it needs " +
                 Mm(minHoleToBend) +
                 " mm or it draws into an oval when the metal moves",
             i + 1, ElementRef{body.createdBy, name ? *name : std::string{}},
             gap, minHoleToBend});
      }
    }
  } catch (const Standard_Failure&) {
    out.notChecked.push_back(
        "the geometry checks stopped early - the kernel refused this body");
  }
  return out;
}

} // namespace sheet
