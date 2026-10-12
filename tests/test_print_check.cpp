// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Feature.h"
#include "core/PrintCheck.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gp_Ax2.hxx>

#include <cmath>

using Catch::Approx;

namespace {

Body Make(const TopoDS_Shape& s, const char* name) {
  Body b;
  b.name = name;
  b.shape = CADShape(s);
  return b;
}

Body Box(double x, double y, double z, double dx, double dy, double dz) {
  return Make(BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape(), "box");
}

// A cone standing on its small end: its side faces down at `deg` from
// vertical all the way round.
Body Funnel(double deg) {
  const double r1 = 5.0, h = 10.0;
  const double r2 = r1 + h * std::tan(deg * M_PI / 180.0);
  return Make(
      BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), r1, r2, h)
          .Shape(),
      "funnel");
}

printcheck::Report Run(const std::vector<Body>& bodies,
                       const printcheck::Options& o = {}) {
  std::vector<const Body*> ptrs;
  for (const Body& b : bodies)
    ptrs.push_back(&b);
  return printcheck::Check(ptrs, o);
}

} // namespace

TEST_CASE("A box on the bed prints; lifted, it does not", "[print][check]") {
  const auto r = Run({Box(0, 0, 0, 10, 10, 10)});
  CHECK(r.bed.status == printcheck::Status::Pass);
  CHECK(r.bed.lowestMm == Approx(0.0).margin(1e-9));
  CHECK(r.bed.contactAreaMm2 == Approx(100.0));
  CHECK(r.overhang.status == printcheck::Status::Pass);
  CHECK(r.overhang.areaMm2 == 0.0);
  CHECK(r.bridges.faces.empty());
  CHECK(r.shells.status == printcheck::Status::Pass);
  CHECK(r.shells.pieces == 1);
  CHECK(r.clearance.status == printcheck::Status::Skipped);
  CHECK(r.Ok());

  const auto up = Run({Box(0, 0, 2, 10, 10, 10)});
  CHECK(up.bed.status == printcheck::Status::Fail);
  CHECK(up.bed.lowestMm == Approx(2.0));
  CHECK_FALSE(up.Ok());
}

TEST_CASE("A designed 45 degree flank reads 45, and 60 fails",
          "[print][check]") {
  const auto ok = Run({Funnel(45.0)});
  CHECK(ok.overhang.worstDeg == Approx(45.0).margin(1e-6));
  CHECK(ok.overhang.areaMm2 == 0.0);
  CHECK(ok.overhang.status == printcheck::Status::Pass);

  const auto bad = Run({Funnel(60.0)});
  CHECK(bad.overhang.worstDeg == Approx(60.0).margin(1e-6));
  CHECK(bad.overhang.status == printcheck::Status::Fail);
  // The whole side is past the limit: pi (r1 + r2) * slant.
  const double r1 = 5.0, r2 = 5.0 + 10.0 * std::tan(M_PI / 3.0);
  const double slant = 10.0 / std::cos(M_PI / 3.0);
  CHECK(bad.overhang.areaMm2 == Approx(M_PI * (r1 + r2) * slant).epsilon(0.02));
  REQUIRE(bad.overhang.worst);
  CHECK(bad.overhang.worst->body == 0);

  // A stated limit past it passes; an allowance past the area passes.
  printcheck::Options loose;
  loose.maxOverhangDeg = 61.0;
  CHECK(Run({Funnel(60.0)}, loose).overhang.status == printcheck::Status::Pass);
}

TEST_CASE("A ceiling is a bridge with its span", "[print][check]") {
  // A block with a slot 12 wide (x) through its 10 deep (y), 5 high: the
  // slot's ceiling is anchored on its sides, so its span is the inscribed
  // diameter, 10.
  const TopoDS_Shape block = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 32, 10, 10);
  const TopoDS_Shape slot = BRepPrimAPI_MakeBox(gp_Pnt(10, -1, 0), 12, 12, 5);
  const Body arch = Make(BRepAlgoAPI_Cut(block, slot).Shape(), "arch");
  printcheck::Options o;
  o.maxBridgeMm = 9.0;
  const auto r = Run({arch}, o);
  REQUIRE(r.bridges.faces.size() == 1);
  CHECK(r.bridges.worstSpanMm == Approx(10.0).margin(0.05));
  CHECK(r.bridges.faces[0].heightMm == Approx(5.0));
  CHECK(r.bridges.areaMm2 == Approx(120.0));
  CHECK(r.bridges.status == printcheck::Status::Fail);
  o.maxBridgeMm = 10.5;
  CHECK(Run({arch}, o).bridges.status == printcheck::Status::Pass);
  // The ceiling is not an overhang.
  CHECK(r.overhang.areaMm2 == 0.0);
}

TEST_CASE("Print-in-place bodies keep the gap, and touching ones fuse",
          "[print][check]") {
  printcheck::Options o;
  o.gapSideMm = 0.25;
  o.stlDeflectionMm = 0.025;
  const auto apart =
      Run({Box(0, 0, 0, 10, 10, 10), Box(10.5, 0, 0, 10, 10, 10)}, o);
  CHECK(apart.clearance.status == printcheck::Status::Pass);
  REQUIRE(apart.clearance.minMm);
  CHECK(*apart.clearance.minMm == Approx(0.5));
  CHECK(apart.shells.pieces == 2);
  CHECK(apart.shells.status == printcheck::Status::Pass);
  CHECK(apart.chords.status == printcheck::Status::Pass);
  CHECK(apart.Ok());

  const auto close =
      Run({Box(0, 0, 0, 10, 10, 10), Box(10.1, 0, 0, 10, 10, 10)}, o);
  CHECK(close.clearance.status == printcheck::Status::Fail);
  CHECK(*close.clearance.minMm == Approx(0.1));
  CHECK(close.shells.status == printcheck::Status::Pass);

  // A fused print-in-place pair: one piece where two were meant.
  const auto fused =
      Run({Box(0, 0, 0, 10, 10, 10), Box(10, 0, 0, 10, 10, 10)}, o);
  CHECK(fused.shells.pieces == 1);
  CHECK(fused.shells.expected == 2);
  CHECK(fused.shells.status == printcheck::Status::Fail);
  REQUIRE(fused.shells.joined.size() == 1);
  CHECK(fused.clearance.status == printcheck::Status::Fail);
  CHECK_FALSE(fused.Ok());

  // Overlapping, the same.
  const auto into =
      Run({Box(0, 0, 0, 10, 10, 10), Box(9, 0, 0, 10, 10, 10)}, o);
  CHECK(into.shells.pieces == 1);
  CHECK(into.clearance.pairs.at(0).overlap);

  // Far apart, the pair is cleared by its boxes.
  const auto far =
      Run({Box(0, 0, 0, 10, 10, 10), Box(30, 0, 0, 10, 10, 10)}, o);
  CHECK(far.clearance.pruned == 1);
  CHECK(far.shells.pieces == 2);
  CHECK(far.clearance.status == printcheck::Status::Pass);
}

TEST_CASE("A body of loose solids is as many pieces", "[print][check]") {
  // One extrude of three discs is one body of three solids: the coupon's.
  TopoDS_Compound c;
  BRep_Builder bb;
  bb.MakeCompound(c);
  for (int i = 0; i < 3; ++i)
    bb.Add(c, BRepPrimAPI_MakeBox(gp_Pnt(i * 5.0, 0, 0), 4, 4, 4).Shape());
  printcheck::Options o;
  o.gapSideMm = 0.25;
  const auto r = Run({Make(c, "discs"), Box(0, 10, 0, 20, 10, 2)}, o);
  CHECK(r.shells.expected == 4);
  CHECK(r.shells.pieces == 4);
  CHECK(r.shells.status == printcheck::Status::Pass);
  CHECK(r.shells.split == std::vector<int>{0});
  CHECK(*r.clearance.minMm == Approx(1.0));
}

TEST_CASE("Chords coarser than a fifth of the gap fail", "[print][check]") {
  printcheck::Options o;
  o.gapSideMm = 0.25;
  o.stlDeflectionMm = 0.06;
  const auto r = Run({Box(0, 0, 0, 10, 10, 10)}, o);
  CHECK(r.chords.limitMm == Approx(0.05));
  CHECK(r.chords.status == printcheck::Status::Fail);
}

TEST_CASE("Another print direction", "[print][check]") {
  // A box resting on x = 0, printed along +X: its x = 0 face is the bed.
  printcheck::Options o;
  o.up = {1.0, 0.0, 0.0};
  const auto r = Run({Box(0, 0, 3, 10, 10, 10)}, o);
  CHECK(r.bed.status == printcheck::Status::Pass);
  CHECK(r.bed.contactAreaMm2 == Approx(100.0));
  o.up = {0.0, 0.0, 0.0};
  CHECK_THROWS(Run({Box(0, 0, 0, 1, 1, 1)}, o));
}
