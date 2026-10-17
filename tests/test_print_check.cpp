// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Feature.h"
#include "core/PrintAllowance.h"
#include "core/PrintCheck.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
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

namespace {

// A post of radius 3 standing on the bed, with a 45° head flaring out to
// radius 3.5 and a rim above it; a ring round it on the bed, bore 3.2 (0.2 a
// side), 2 high, with `layers` x 0.2 of air between the head and the ring's
// inner edge -- the coupon's air row, and the counter's axles.
std::vector<Body> PostAndRing(int layers) {
  const double rp = 3.0, ri = 3.2, rc = 3.5, ro = 6.0, H = 2.0;
  const double z0 = H + layers * 0.2 - (ri - rp); // the head meets the post
  const gp_Ax2 at(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
  const double top = z0 + (rc - rp) + 1.0;
  TopoDS_Shape post = BRepPrimAPI_MakeCylinder(at, rp, top).Shape();
  post = BRepAlgoAPI_Fuse(post, BRepPrimAPI_MakeCone(
                                    gp_Ax2(gp_Pnt(0, 0, z0), gp_Dir(0, 0, 1)),
                                    rp, rc, rc - rp)
                                    .Shape())
             .Shape();
  post = BRepAlgoAPI_Fuse(
             post, BRepPrimAPI_MakeCylinder(
                       gp_Ax2(gp_Pnt(0, 0, z0 + rc - rp), gp_Dir(0, 0, 1)), rc,
                       top - (z0 + rc - rp))
                       .Shape())
             .Shape();
  const TopoDS_Shape ring =
      BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(at, ro, H).Shape(),
                      BRepPrimAPI_MakeCylinder(at, ri, H).Shape())
          .Shape();
  std::vector<Body> out;
  out.push_back(Make(post, "post"));
  out.push_back(Make(ring, "ring"));
  return out;
}

} // namespace

TEST_CASE("A block one layer over another is off the bed and fused",
          "[print][check][printcheck]") {
  printcheck::Options o;
  o.minAirMm = 0.6;
  const auto r = Run({Box(0, 0, 0, 10, 10, 2), Box(0, 0, 2.2, 10, 10, 2)}, o);
  const printcheck::Stacking& st = r.stacking;
  CHECK(st.status == printcheck::Status::Fail);
  CHECK(st.limitMm == 0.6);
  CHECK(st.toleranceMm == Approx(0.06));
  REQUIRE(st.pieces.size() == 2);
  // Every piece starts on the bed: the upper one starts 2.2 over it.
  REQUIRE(st.lifted.size() == 1);
  CHECK(st.pieces[st.lifted[0].piece].bodies == std::vector<int>{1});
  CHECK(st.lifted[0].heightMm == Approx(2.2));
  CHECK(st.lifted[0].where.body == 1);
  // And its underside lies one layer over the lower block, all of it.
  REQUIRE(st.pairs.size() == 1);
  CHECK(st.pieces[st.pairs[0].above].bodies == std::vector<int>{1});
  CHECK(st.pieces[st.pairs[0].below].bodies == std::vector<int>{0});
  CHECK(st.pairs[0].minAirMm == Approx(0.2).margin(1e-6));
  CHECK(st.pairs[0].areaMm2 == Approx(100.0).epsilon(1e-6));
  CHECK(st.pairs[0].where.body == 1);
  CHECK_FALSE(r.Ok());
  // The other items see nothing wrong: the bed has the print's lowest point,
  // the blocks are apart, and a ceiling flat on the bed of its own piece
  // is no bridge.
  CHECK(r.bed.status == printcheck::Status::Pass);
  CHECK(r.shells.status == printcheck::Status::Pass);

  // Over the limit's air, side by side, both on the bed: it prints.
  const auto apart =
      Run({Box(0, 0, 0, 10, 10, 2), Box(12, 0, 0, 10, 10, 4)}, o);
  CHECK(apart.stacking.status == printcheck::Status::Pass);
  CHECK(apart.stacking.pairs.empty());
  CHECK(apart.stacking.lifted.empty());
}

TEST_CASE("A ring under a 45 degree head needs three layers of air",
          "[print][check][printcheck]") {
  printcheck::Options o;
  o.minAirMm = 0.6;
  o.gapSideMm = 0.2;
  const auto three = Run(PostAndRing(3), o);
  CHECK(three.stacking.status == printcheck::Status::Pass);
  CHECK(three.stacking.lifted.empty());
  CHECK(three.overhang.status == printcheck::Status::Pass);
  CHECK(three.Ok());
  // The head is listed over the ring, at the limit and not under it.
  for (const auto& pr : three.stacking.pairs) {
    CHECK(pr.minAirMm >= 0.6 - 0.06);
    CHECK(pr.areaMm2 == 0.0);
  }

  const auto one = Run(PostAndRing(1), o);
  const printcheck::Stacking& st = one.stacking;
  CHECK(st.status == printcheck::Status::Fail);
  CHECK(st.lifted.empty());
  REQUIRE(st.pairs.size() == 1);
  CHECK(st.pieces[st.pairs[0].above].bodies == std::vector<int>{0});
  CHECK(st.pieces[st.pairs[0].below].bodies == std::vector<int>{1});
  CHECK(st.pairs[0].minAirMm == Approx(0.2).margin(0.02));
  // The cone's area over the ring, 3.2 to 3.5, at 45°: all of it under
  // 0.54 of air, and none of what is over the bore's gap.
  const double cone = M_PI * (3.5 * 3.5 - 3.2 * 3.2) * std::sqrt(2.0);
  CHECK(st.pairs[0].areaMm2 == Approx(cone).epsilon(0.05));
  CHECK(st.pairs[0].where.body == 0);
}

TEST_CASE("A body's own bridge over its own floor is not stacking",
          "[print][check][printcheck]") {
  // A block with a slot 0.2 high through it, 1 above the bed: its ceiling
  // is one layer over its own floor, the bridges item's business.
  const TopoDS_Shape block = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 4);
  const TopoDS_Shape slot = BRepPrimAPI_MakeBox(gp_Pnt(5, -1, 1), 8, 12, 0.2);
  printcheck::Options o;
  o.minAirMm = 0.6;
  const auto r =
      Run({Make(BRepAlgoAPI_Cut(block, slot).Shape(), "slotted")}, o);
  CHECK(r.stacking.status == printcheck::Status::Pass);
  CHECK(r.stacking.pairs.empty());
  CHECK(r.bridges.faces.size() == 1);
}

TEST_CASE("Stacking needs a minimum air, and refuses one not over 0",
          "[print][check][printcheck]") {
  const auto r = Run({Box(0, 0, 0, 10, 10, 2), Box(0, 0, 2.2, 10, 10, 2)});
  CHECK(r.stacking.status == printcheck::Status::Skipped);
  CHECK(r.stacking.note.find("min_air_mm") != std::string::npos);
  printcheck::Options o;
  o.minAirMm = 0.0;
  CHECK_THROWS(Run({Box(0, 0, 0, 1, 1, 1)}, o));
  o.minAirMm = -1.0;
  CHECK_THROWS(Run({Box(0, 0, 0, 1, 1, 1)}, o));
}

TEST_CASE("The minimum air comes from the profile, its layers, or FDM",
          "[print][check][printcheck]") {
  printfit::Process proc{"fdm", "pla", std::nullopt};
  CHECK(printfit::MinAirMm(proc) == Approx(0.6));
  CHECK(printfit::MinAirFrom(proc) == "process");
  CHECK(printfit::MinAirSource(proc).find("coupon") != std::string::npos);

  printfit::Profile prof = *printfit::ProfileFromBuiltin("K5", "fdm", "pla");
  prof.layerHeightMm = 0.16;
  proc = printfit::FromProfile(prof);
  CHECK(printfit::MinAirMm(proc) == Approx(0.48));
  CHECK(printfit::MinAirFrom(proc) == "layers");

  prof.zGapMm = 0.32;
  proc = printfit::FromProfile(prof);
  CHECK(printfit::MinAirMm(proc) == Approx(0.32));
  CHECK(printfit::MinAirFrom(proc) == "profile");
  CHECK(printfit::MinAirSource(proc).find("z_gap_mm") != std::string::npos);

  // A process that is not FDM knows no air: the item is skipped.
  proc.process = "sla";
  CHECK(printfit::MinAirMm(proc) == 0.0);
  CHECK(printfit::MinAirFrom(proc).empty());
  CHECK(printfit::MinAirSource(proc).find("not FDM") != std::string::npos);
}

TEST_CASE("A plate over the bed fails bed_fit with its shortfall",
          "[print][check][bed]") {
  // Two blocks side by side, 300 mm across in all: on a 256 mm bed it is 44
  // over in x, and fits in y and up.
  const std::vector<Body> plate = {Box(0, 0, 0, 150, 40, 10),
                                   Box(150, 0, 0, 150, 40, 30)};
  printcheck::Options o;
  o.bedMm = glm::dvec3(256.0, 256.0, 256.0);
  printcheck::Report r = Run(plate, o);
  CHECK(r.bedFit.status == printcheck::Status::Fail);
  CHECK(r.bedFit.extentMm.x == Approx(300.0));
  CHECK(r.bedFit.extentMm.y == Approx(40.0));
  CHECK(r.bedFit.extentMm.z == Approx(30.0));
  CHECK(r.bedFit.shortfallMm.x == Approx(44.0));
  CHECK(r.bedFit.shortfallMm.y == 0.0);
  CHECK(r.bedFit.shortfallMm.z == 0.0);
  CHECK_FALSE(r.Ok());

  // Turned onto its side it is 300 tall: the bed's z is the print's up.
  o.up = glm::dvec3(0.0, 1.0, 0.0);
  r = Run(plate, o);
  CHECK(r.bedFit.status == printcheck::Status::Fail);
  CHECK(r.bedFit.extentMm.z == Approx(40.0));

  // On a bigger bed it fits.
  o.up = glm::dvec3(0.0, 0.0, 1.0);
  o.bedMm = glm::dvec3(350.0, 350.0, 350.0);
  r = Run(plate, o);
  CHECK(r.bedFit.status == printcheck::Status::Pass);

  // Without a bed it is skipped, saying why: nothing guesses a printer.
  o.bedMm.reset();
  o.bedNote = "print profile 'X' states no bed_mm";
  r = Run(plate, o);
  CHECK(r.bedFit.status == printcheck::Status::Skipped);
  CHECK(r.bedFit.note == "print profile 'X' states no bed_mm");
}
