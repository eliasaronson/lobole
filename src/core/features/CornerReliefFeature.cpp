#include "core/features/CornerReliefFeature.h"

#include "core/ElementHistory.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

std::string CornerReliefStyleName(CornerReliefStyle s) {
  return s == CornerReliefStyle::Rectangular ? "rectangular" : "circular";
}

bool CornerReliefStyleFromString(const std::string& name,
                                 CornerReliefStyle& out) {
  if (name == "circular") {
    out = CornerReliefStyle::Circular;
    return true;
  }
  if (name == "rectangular") {
    out = CornerReliefStyle::Rectangular;
    return true;
  }
  return false;
}

const std::string& CornerReliefStyleList() {
  static const std::string list = "circular, rectangular";
  return list;
}

CornerReliefFeature::CornerReliefFeature(const std::string& name,
                                         uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

namespace {

gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Vec V(const glm::dvec3& v) { return gp_Vec(v.x, v.y, v.z); }

struct CornerAt {
  gp_Pnt q;
  gp_Dir up;
  std::vector<size_t> bends;
  double radius = 0.0; // the larger bend radius there
  bool square = true;
};

} // namespace

bool CornerReliefFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules (set_sheet_metal_rules)");
    return false;
  }
  const double t = ctx.sheetMetal->thickness;
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the sheet body to relieve no longer exists");
    return false;
  }
  for (const BendRegion& b : body->bends)
    if (b.flat) {
      SetError("this body has bends laid flat - relieve its corners folded");
      return false;
    }
  if (size < 0.0) {
    SetError("a corner relief's size cannot be negative");
    return false;
  }

  // Every pair of bends folding off the same face whose bend lines cross near
  // one end of each: a corner.
  std::vector<CornerAt> corners;
  const std::vector<BendRegion>& bends = body->bends;
  for (size_t i = 0; i < bends.size(); ++i) {
    for (size_t j = i + 1; j < bends.size(); ++j) {
      const BendRegion& a = bends[i];
      const BendRegion& b = bends[j];
      const gp_Vec ua = V(a.upDir), ub = V(b.upDir);
      if (ua.Dot(ub) < 1.0 - 1e-9)
        continue; // not folding off the same side of the same panel
      const gp_Pnt ta = P(a.axisOrigin).Translated(ua * -a.radius);
      const gp_Pnt tb = P(b.axisOrigin).Translated(ub * -b.radius);
      if (std::abs(gp_Vec(ta, tb).Dot(ua)) > 1e-6)
        continue; // parallel faces, not the same one
      const gp_Vec da = V(a.axisDir), db = V(b.axisDir);
      const double cross = da.Crossed(db).Magnitude();
      if (cross < 1e-9)
        continue; // parallel bend lines never meet
      // ta + s*da = tb + r*db, in the panel's plane.
      const gp_Vec w(ta, tb);
      const gp_Vec n = da.Crossed(db);
      const double s = w.Crossed(db).Dot(n) / n.SquareMagnitude();
      const double r = w.Crossed(da).Dot(n) / n.SquareMagnitude();
      const double tol = 2.0 * (std::max(a.radius, b.radius) + t) + t;
      const auto nearEnd = [&](double x, double width) {
        return std::abs(x) <= tol || std::abs(x - width) <= tol;
      };
      if (!nearEnd(s, a.width) || !nearEnd(r, b.width))
        continue;
      const gp_Pnt q = ta.Translated(da * s);
      auto found =
          std::find_if(corners.begin(), corners.end(), [&](const CornerAt& c) {
            return c.q.Distance(q) < 1e-6;
          });
      if (found == corners.end()) {
        corners.push_back({q, gp_Dir(ua), {}, 0.0, true});
        found = corners.end() - 1;
      }
      for (size_t k : {i, j})
        if (std::find(found->bends.begin(), found->bends.end(), k) ==
            found->bends.end())
          found->bends.push_back(k);
      found->radius = std::max({found->radius, a.radius, b.radius});
      if (std::abs(da.Dot(db)) > 1e-6)
        found->square = false;
    }
  }
  if (corners.empty()) {
    SetError("no two bends of this body meet at a corner");
    return false;
  }
  for (const CornerAt& c : corners) {
    if (c.bends.size() > 2) {
      SetError(std::to_string(c.bends.size()) +
               " bends meet at one corner - a relief shaped for two would "
               "not free three");
      return false;
    }
    if (!c.square) {
      SetError("two bends meet at other than a right angle - only a square "
               "corner can be relieved yet");
      return false;
    }
  }

  ArmOccSignalHandlers();
  try {
    CADShape working = body->shape;
    ElementMap map = body->elementMap;
    const double eps = std::max(t, 1.0) * 1e-3;
    for (const CornerAt& c : corners) {
      const double r = size > 0.0 ? size : c.radius + t;
      // Through the panel and up to where the walls start, and no higher: a
      // relief is for the corner, not for the walls above it.
      const double h = t + c.radius + 2.0 * eps;
      const gp_Pnt base = c.q.Translated(gp_Vec(c.up) * -(t + eps));
      CADShape tool;
      if (style == CornerReliefStyle::Circular) {
        tool = CADShape(
            BRepPrimAPI_MakeCylinder(gp_Ax2(base, c.up), r, h).Shape());
      } else {
        const BendRegion& a = bends[c.bends[0]];
        const gp_Vec x = V(a.axisDir) * r;
        const gp_Vec y = gp_Vec(c.up).Crossed(V(a.axisDir)) * r;
        const gp_Pnt o = base.Translated(-x - y);
        BRepBuilderAPI_MakePolygon poly(o, o.Translated(x * 2.0),
                                        o.Translated(x * 2.0 + y * 2.0),
                                        o.Translated(y * 2.0), Standard_True);
        tool = CADShape(
            BRepPrimAPI_MakePrism(
                BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Face(),
                gp_Vec(c.up) * h)
                .Shape());
      }
      ElementMap none, out;
      std::string why;
      CADShape cut = topo::BooleanNamed(BooleanOp::Subtract, working, map, tool,
                                        none, GetId(), out, &why);
      if (!cut.IsValid() || cut.Volume() >= working.Volume() - 1e-9) {
        SetError(why.empty() ? "a corner relief cut nothing - the corner is "
                               "already clear"
                             : why);
        return false;
      }
      working = std::move(cut);
      map = std::move(out);
    }
    body->shape = std::move(working);
    body->elementMap = std::move(map);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("corner relief failed: ") + e.GetMessageString());
    return false;
  }
}
