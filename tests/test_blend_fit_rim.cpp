// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The refusing bounds at circular rims, where two blends meet across a face
// and where an edge bends in a face
// (docs/modeling/README.md#bounds-that-refuse), checked against the
// builder both ways: just under the bound it builds, so the bound refuses
// nothing that fits; just past the refusal margin it does not, so the bound
// is the limit and not a guess above it.

#include <catch2/catch_test_macros.hpp>

#include "core/BlendFit.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/features/FilletFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Elips.hxx>

#include <cmath>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {

Body MakeBody(const TopoDS_Shape& s) {
  Body body;
  body.shape = CADShape(s);
  body.createdBy = 1;
  topo::NamePrimitiveElements(body.shape, "1", body.elementMap);
  return body;
}

using CurvePred = std::function<bool(const BRepAdaptor_Curve&)>;

std::vector<int> EdgesWhere(const TopoDS_Shape& s, const CurvePred& pred) {
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(s, TopAbs_EDGE, all);
  std::vector<int> out;
  for (int i = 1; i <= all.Extent(); i++) {
    const TopoDS_Edge& e = TopoDS::Edge(all(i));
    if (!BRep_Tool::Degenerated(e) && pred(BRepAdaptor_Curve(e)))
      out.push_back(i - 1);
  }
  return out;
}

// Circles on the z axis at height z.
std::vector<int> CirclesAt(const TopoDS_Shape& s, double z) {
  return EdgesWhere(s, [&](const BRepAdaptor_Curve& c) {
    return c.GetType() == GeomAbs_Circle &&
           std::abs(c.Circle().Location().Z() - z) < 1e-6 &&
           std::abs(c.Circle().Location().X()) < 1e-6 &&
           std::abs(c.Circle().Location().Y()) < 1e-6;
  });
}

bool Builds(const Body& b, const std::vector<int>& e, EdgeBlendOp op,
            double size) {
  const CADShape built = topo::EdgeBlendUnnamed(b.shape, e, op, size);
  return built.IsValid() && Measure::InvalidReasons(built).empty();
}

const char* OpName(EdgeBlendOp op) {
  return op == EdgeBlendOp::Chamfer ? "chamfer" : "fillet";
}

// The bound of `kind` on these edges is `expect` (when given), it is the
// least of every bound, and the builder agrees with it both ways: the
// search's first candidate under it builds, and a size past the refusal
// margin does not.
void Agrees(const char* what, const Body& b, const std::vector<int>& edges,
            EdgeBlendOp op, fit::BoundKind kind, std::optional<double> expect) {
  INFO(what << " " << OpName(op));
  const auto refusing = fit::RefusingBounds(b, edges, op, HUGE_VAL);
  const auto least = fit::Least(refusing);
  REQUIRE(least.has_value());
  INFO(least->why);
  CHECK(least->kind == kind);
  if (expect)
    CHECK(std::abs(least->size - *expect) < 1e-6 * *expect);
  // BlendBounds offers it too, so the search is seeded under it.
  bool offered = false;
  for (const fit::Bound& x : fit::BlendBounds(b, edges, op))
    offered = offered || (x.kind == kind && x.size == least->size);
  CHECK(offered);
  const double under = fit::RoundDown3(least->size * (1.0 - 1e-3));
  CHECK(Builds(b, edges, op, under));
  CHECK_FALSE(
      Builds(b, edges, op, least->size * (1.0 + fit::RefuseMargin(kind))));
  CHECK_FALSE(Builds(b, edges, op, least->size * 1.5));
}

// No refusing bound, and the builder does build past the cylinder's own
// radius: the axis is not the limit on this side.
void NoAxisBound(const char* what, const Body& b, const std::vector<int>& edges,
                 EdgeBlendOp op, double R) {
  INFO(what << " " << OpName(op));
  CHECK(fit::RimBounds(b, edges, op).empty());
  CHECK(Builds(b, edges, op, 1.5 * R));
}

TopoDS_Shape Plate(double w, double t) {
  return BRepPrimAPI_MakeBox(gp_Pnt(-w / 2, -w / 2, -t), w, w, t).Shape();
}

TopoDS_Shape Cyl(double r, double z0, double h, double x = 0, double y = 0) {
  return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z0), gp::DZ()), r, h)
      .Shape();
}

TopoDS_Shape Cone(double r0, double r1, double z0, double h) {
  return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, z0), gp::DZ()), r0, r1, h)
      .Shape();
}

constexpr EdgeBlendOp kOps[] = {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer};

} // namespace

TEST_CASE("A round on the axis side of a rim stops at the axis",
          "[fillet][fit]") {
  for (const double R : {1.0, 2.5, 5.0}) {
    const double T = 20.0;
    const TopoDS_Shape plate = Plate(30.0 * R, T);
    for (const EdgeBlendOp op : kOps) {
      // A shaft's end: R.
      const TopoDS_Shape boss = BRepAlgoAPI_Fuse(plate, Cyl(R, -1, T + 1));
      Agrees("boss top", MakeBody(boss), CirclesAt(boss, T), op,
             fit::BoundKind::Rim, R);
      // A blind bore's floor, concave: R.
      const TopoDS_Shape blind = BRepAlgoAPI_Cut(plate, Cyl(R, -T / 2, T));
      Agrees("blind bore floor", MakeBody(blind), CirclesAt(blind, -T / 2), op,
             fit::BoundKind::Rim, R);
      // A bore's rims and a boss's foot lean away from the axis.
      const TopoDS_Shape bore = BRepAlgoAPI_Cut(plate, Cyl(R, -T - 1, T + 2));
      NoAxisBound("bore top", MakeBody(bore), CirclesAt(bore, 0), op, R);
      NoAxisBound("boss foot", MakeBody(boss), CirclesAt(boss, 0), op, R);
      NoAxisBound("blind bore top", MakeBody(blind), CirclesAt(blind, 0), op,
                  R);
      // A frustum leaning in by alpha: a round of R tan(45 + alpha/2), a
      // chamfer of R across its top.
      for (const double lean : {0.25, 0.5, 1.0}) {
        const double h = 2.0 * R, alpha = std::atan(lean * R / h);
        const TopoDS_Shape frustum =
            BRepAlgoAPI_Fuse(plate, Cone((1.0 + lean) * R, R, 0.0, h));
        const double expect =
            op == EdgeBlendOp::Chamfer
                ? R
                : R * std::tan(std::numbers::pi / 4 + alpha / 2);
        Agrees("frustum top", MakeBody(frustum), CirclesAt(frustum, h), op,
               fit::BoundKind::Rim, expect);
      }
      // A tapered blind hole's floor: its wall leans out, the floor's own
      // setback reaches the axis first.
      for (const double flare : {0.25, 1.0}) {
        const double depth = T / 2;
        const TopoDS_Shape taper = BRepAlgoAPI_Cut(
            plate, Cone(R, (1.0 + flare) * R, -depth, depth + 1.0));
        const double slope = std::atan(flare * R / (depth + 1.0));
        const double expect =
            op == EdgeBlendOp::Chamfer
                ? R
                : R * std::tan(std::numbers::pi / 4 + slope / 2);
        Agrees("tapered floor", MakeBody(taper), CirclesAt(taper, -depth), op,
               fit::BoundKind::Rim, expect);
      }
      // A drilled hole's point: a cylinder meeting a cone, no plane at all.
      {
        const double depth = T / 2;
        const double tip = R * std::tan(std::numbers::pi / 6); // 120 deg point
        TopoDS_Shape drill = BRepAlgoAPI_Fuse(Cyl(R, -depth, depth + 1.0),
                                              Cone(0.0, R, -depth - tip, tip));
        const TopoDS_Shape drilled = BRepAlgoAPI_Cut(plate, drill);
        const auto rim = CirclesAt(drilled, -depth);
        REQUIRE(rim.size() == 1);
        // The cone runs R / sin 60 to its tip; a round, at 120 degrees to
        // the wall, takes r tan 30 of it: 2R, where the ball's centre
        // would cross the axis at R (the builder builds 1.5R).
        Agrees("drill point", MakeBody(drilled), rim, op, fit::BoundKind::Rim,
               op == EdgeBlendOp::Chamfer ? R / std::sin(std::numbers::pi / 3)
                                          : 2.0 * R);
      }
    }
  }
}

TEST_CASE("Two blends that meet across a face stop where they meet",
          "[fillet][fit]") {
  for (const EdgeBlendOp op : kOps) {
    // Two bores, both top rims: the land between them, shared.
    for (const double R : {1.0, 3.0})
      for (const double land : {1.0, 4.0}) {
        const double T = 10.0, x = R + land / 2;
        const TopoDS_Shape two = BRepAlgoAPI_Cut(
            BRepAlgoAPI_Cut(Plate(60, T), Cyl(R, -T - 1, T + 2, -x, 0)),
            Cyl(R, -T - 1, T + 2, x, 0));
        const auto rims = EdgesWhere(two, [](const BRepAdaptor_Curve& c) {
          return c.GetType() == GeomAbs_Circle &&
                 std::abs(c.Circle().Location().Z()) < 1e-6;
        });
        REQUIRE(rims.size() == 2);
        Agrees("two bores", MakeBody(two), rims, op, fit::BoundKind::Meet,
               land / 2);
      }
    // A bore beside the plate's side: its rim and the side's top edge.
    {
      const double T = 10.0, R = 2.0, land = 1.5;
      const TopoDS_Shape near = BRepAlgoAPI_Cut(
          Plate(40, T), Cyl(R, -T - 1, T + 2, -20 + land + R, 0));
      auto edges = EdgesWhere(near, [](const BRepAdaptor_Curve& c) {
        if (c.GetType() == GeomAbs_Circle)
          return std::abs(c.Circle().Location().Z()) < 1e-6;
        const gp_Pnt a = c.Value(c.FirstParameter()),
                     b = c.Value(c.LastParameter());
        return std::abs(a.Z()) < 1e-6 && std::abs(b.Z()) < 1e-6 &&
               std::abs(a.X() + 20) < 1e-6 && std::abs(b.X() + 20) < 1e-6;
      });
      REQUIRE(edges.size() == 2);
      Agrees("bore by a side", MakeBody(near), edges, op, fit::BoundKind::Meet,
             land / 2);
    }
    // A prism whose sides lean in: its top's two long edges meet at 120 deg
    // to it, so a round takes r tan(30 deg) of the top, a chamfer its
    // distance.
    {
      const double w = 6.0, h = 4.0, lean = h * std::tan(std::numbers::pi / 6);
      BRepBuilderAPI_MakePolygon poly;
      poly.Add(gp_Pnt(-w / 2 - lean, 0, 0));
      poly.Add(gp_Pnt(w / 2 + lean, 0, 0));
      poly.Add(gp_Pnt(w / 2, 0, h));
      poly.Add(gp_Pnt(-w / 2, 0, h));
      poly.Close();
      const TopoDS_Shape prism =
          BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(poly.Wire()).Face(),
                                gp_Vec(0, 40, 0))
              .Shape();
      const auto top = EdgesWhere(prism, [&](const BRepAdaptor_Curve& c) {
        return c.GetType() == GeomAbs_Line &&
               std::abs(c.Value(c.FirstParameter()).Z() - h) < 1e-6 &&
               std::abs(c.Value(c.LastParameter()).Z() - h) < 1e-6 &&
               std::abs(c.Line().Direction().Y()) > 0.5;
      });
      REQUIRE(top.size() == 2);
      const double k =
          op == EdgeBlendOp::Chamfer ? 1.0 : std::tan(std::numbers::pi / 6);
      Agrees("leaning prism", MakeBody(prism), top, op, fit::BoundKind::Meet,
             w / (2 * k));
    }
  }
}

TEST_CASE("A blend past where it meets its tangent chain names the chain",
          "[fillet][fit]") {
  // One corner arc of a plate's outline, picked, brings the whole outline
  // in by tangency; a bore 2 mm in from a side meets it at 1.
  const double T = 10.0, R = 3.0;
  TopoDS_Shape plate = Plate(40, T);
  // Rounded vertical corners, then the bore.
  {
    const auto corners = EdgesWhere(plate, [](const BRepAdaptor_Curve& c) {
      return c.GetType() == GeomAbs_Line &&
             std::abs(c.Line().Direction().Z()) > 0.5;
    });
    REQUIRE(corners.size() == 4);
    plate = topo::EdgeBlendUnnamed(CADShape(plate), corners,
                                   EdgeBlendOp::Fillet, 4.0)
                .GetShape();
  }
  const TopoDS_Shape s =
      BRepAlgoAPI_Cut(plate, Cyl(R, -T - 1, T + 2, -20 + 2 + R, 0));
  auto edges = EdgesWhere(s, [&](const BRepAdaptor_Curve& c) {
    return c.GetType() == GeomAbs_Circle &&
           std::abs(c.Value(c.FirstParameter()).Z()) < 1e-6 &&
           std::abs(c.Value(c.LastParameter()).Z()) < 1e-6 &&
           std::abs(c.Circle().Radius() - R) > 1e-6;
  });
  REQUIRE(edges.size() == 4); // the corner arcs
  edges.resize(1);
  const auto rim = EdgesWhere(s, [&](const BRepAdaptor_Curve& c) {
    return c.GetType() == GeomAbs_Circle &&
           std::abs(c.Circle().Radius() - R) < 1e-6 &&
           std::abs(c.Circle().Location().Z()) < 1e-6;
  });
  REQUIRE(rim.size() == 1);
  edges.push_back(rim[0]);
  const Body b = MakeBody(s);
  for (const EdgeBlendOp op : kOps) {
    Agrees("arc and bore", b, edges, op, fit::BoundKind::Meet, 1.0);
    const auto meet = fit::MeetBound(b, edges, op, HUGE_VAL);
    REQUIRE(meet.has_value());
    CHECK(meet->why.find("(tangent to a picked edge, so blended with it)") !=
          std::string::npos);
  }
}

// A prism on an ellipse, its side extruded square to its caps: the ellipse
// bends tightest at the ends of its major axis, with a radius of b^2 / a.
TEST_CASE("A blend along an edge that bends in a planar face stops at its "
          "tightest bend",
          "[fillet][fit]") {
  const double a = 30.0, b = 6.0, h = 10.0, rho = b * b / a;
  const auto ellipse = [&](double z) {
    const TopoDS_Edge e = BRepBuilderAPI_MakeEdge(
        gp_Elips(gp_Ax2(gp_Pnt(0, 0, z), gp::DZ(), gp::DX()), a, b));
    return BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(e).Wire()).Face();
  };
  const auto ellipsesAt = [](const TopoDS_Shape& s, double z) {
    return EdgesWhere(s, [&](const BRepAdaptor_Curve& c) {
      return c.GetType() == GeomAbs_Ellipse &&
             std::abs(c.Ellipse().Location().Z() - z) < 1e-6;
    });
  };
  const TopoDS_Shape pad =
      BRepPrimAPI_MakePrism(ellipse(0.0), gp_Vec(0, 0, h)).Shape();
  const TopoDS_Shape plate = Plate(4.0 * a, h);
  const TopoDS_Shape pocket = BRepAlgoAPI_Cut(
      plate, BRepPrimAPI_MakePrism(ellipse(-h / 2), gp_Vec(0, 0, h)).Shape());
  const TopoDS_Shape bore = BRepAlgoAPI_Cut(
      plate,
      BRepPrimAPI_MakePrism(ellipse(-h - 1), gp_Vec(0, 0, h + 2)).Shape());
  for (const EdgeBlendOp op : kOps) {
    // The pad's top: its cap lies inside the ellipse.
    Agrees("ellipse pad top", MakeBody(pad), ellipsesAt(pad, h), op,
           fit::BoundKind::Bend, rho);
    // A pocket's floor, concave: the floor lies inside it too.
    Agrees("ellipse pocket floor", MakeBody(pocket), ellipsesAt(pocket, -h / 2),
           op, fit::BoundKind::Bend, rho);
    // A half ellipse closed by its major axis bends tightest at its two
    // ends, where the fold runs off the arc into the corner: no bound, and
    // the builder rounds the arc past it.
    {
      INFO("half ellipse pad top " << OpName(op));
      const gp_Elips el(gp_Ax2(gp_Pnt(0, 0, 0), gp::DZ(), gp::DX()), a, b);
      const TopoDS_Edge arc =
          BRepBuilderAPI_MakeEdge(el, 0.0, std::numbers::pi);
      const TopoDS_Edge axis =
          BRepBuilderAPI_MakeEdge(gp_Pnt(-a, 0, 0), gp_Pnt(a, 0, 0));
      const TopoDS_Face half =
          BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(arc, axis).Wire());
      const TopoDS_Shape s =
          BRepPrimAPI_MakePrism(half, gp_Vec(0, 0, h)).Shape();
      const Body hb = MakeBody(s);
      const auto top = ellipsesAt(s, h);
      REQUIRE(top.size() == 1);
      CHECK(fit::BendBounds(hb, top, op).empty());
      CHECK(Builds(hb, top, op, 1.5 * rho));
    }
    // A bore's rim bends away from the plate's top: no bound, and the
    // builder builds past the bend.
    INFO("ellipse bore top " << OpName(op));
    const Body bb = MakeBody(bore);
    // The cut may hand the rim back in pieces, or as another curve kind.
    const auto rim = EdgesWhere(bore, [](const BRepAdaptor_Curve& c) {
      return c.GetType() != GeomAbs_Line &&
             std::abs(c.Value(c.FirstParameter()).Z()) < 1e-6 &&
             std::abs(c.Value(c.LastParameter()).Z()) < 1e-6;
    });
    REQUIRE(!rim.empty());
    CHECK(fit::BendBounds(bb, rim, op).empty());
    CHECK(Builds(bb, rim, op, 1.5 * rho));
  }
}
