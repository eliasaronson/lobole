// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The insertion check (docs/inspect/README.md#insertion): a part slid along
// its axis from clear to its seat, failing where it is forced through more
// than its own press. The shapes are the printed clock's: a Ø4.8 arbor
// D-flatted 0.8 deep, a hub pressed on it by a D bore 0.05 a side over it
// that prints 0.1 a side under (the allowance), and a pivot at the running
// gap (0.15 a side under the bore) where a section must slide through.

#include "core/Feature.h"
#include "core/MeasureInsertion.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gp_Ax2.hxx>

#include <optional>
#include <vector>

using Catch::Approx;

namespace {

Body Make(const TopoDS_Shape& s, const char* name) {
  Body b;
  b.name = name;
  b.shape = CADShape(s);
  return b;
}

gp_Ax2 Up(double z) { return gp_Ax2(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1)); }

// A round of radius r over z0..z1, D-flatted at +X from `flat` out when
// one is given.
TopoDS_Shape Round(double z0, double z1, double r,
                   std::optional<double> flat = std::nullopt) {
  TopoDS_Shape s = BRepPrimAPI_MakeCylinder(Up(z0), r, z1 - z0).Shape();
  if (flat)
    s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeBox(gp_Pnt(*flat, -r - 1, z0 - 1),
                                               r + 2, 2 * r + 2, z1 - z0 + 2)
                               .Shape())
            .Shape();
  return s;
}

struct Section {
  double z0, z1, r;
  std::optional<double> flat;
};

Body Shaft(const std::vector<Section>& sections) {
  TopoDS_Shape s;
  for (const Section& c : sections) {
    const TopoDS_Shape p = Round(c.z0, c.z1, c.r, c.flat);
    s = s.IsNull() ? p : BRepAlgoAPI_Fuse(s, p).Shape();
  }
  return Make(s, "arbor");
}

// A hub Ø12 over z0..z1 with its bore cut through.
Body Hub(double z0, double z1, const TopoDS_Shape& bore) {
  TopoDS_Shape s = BRepPrimAPI_MakeCylinder(Up(z0), 6.0, z1 - z0).Shape();
  return Make(BRepAlgoAPI_Cut(s, bore).Shape(), "hub");
}

// The arbor's sizes.
constexpr double kR = 2.4, kFlat = 1.6;       // the D it is pressed on
constexpr double kOver = 0.05;                // the bore, a side over it
constexpr double kAllowance = 0.1;            // how far under it prints
constexpr double kGap = 0.15;                 // the running gap a side
constexpr double kSlideR = kR + kOver - kGap; // slides through the bore
constexpr double kSlideFlat = kFlat + kOver - kGap;

TopoDS_Shape PressDBore(double z0, double z1) {
  return Round(z0 - 1, z1 + 1, kR + kOver, kFlat + kOver);
}

const glm::dmat4 I(1.0);
const glm::dvec3 O(0.0), Z(0.0, 0.0, 1.0);

Measure::InsertionReport Check(const Body& part, const Body& onto,
                               Measure::InsertionEntry entry,
                               double allowance = kAllowance) {
  Measure::InsertionOptions o;
  o.entry = entry;
  o.allowanceMm = allowance;
  auto r = Measure::Insertion({part}, I, {onto}, I, O, Z, o);
  REQUIRE(r.has_value());
  return *r;
}

} // namespace

TEST_CASE("A D bore cannot be forced over a round pivot to a D that stops "
          "short of the end",
          "[measure][insertion]") {
  // The stage-1 escape arbor as first printed: round pivot z -4.8..0, the
  // D from 0 on; the wheel's hub pressed on the D over 2..12.
  const Body arbor = Shaft({{-4.8, 0.0, kR}, {0.0, 20.0, kR, kFlat}});
  const Body hub = Hub(2.0, 12.0, PressDBore(2.0, 12.0));
  const auto r = Check(hub, arbor, Measure::InsertionEntry::Negative);
  CHECK_FALSE(r.pass);
  CHECK_FALSE(r.best.positive);
  CHECK(r.best.pressMm == Approx(10.0).margin(2 * r.stepMm));
  // Forced the whole hub's length and the pivot's past its press.
  CHECK(r.best.excessMm > 4.0);
  // The round meets the bore's flat 0.75 deep, and the print's 0.1 on top:
  // forced far deeper than the 0.05 the hub is pressed at its seat, the
  // pivot's whole length. That alone fails it, however the press's own
  // length reads.
  CHECK(r.best.worstDepthMm > 0.5);
  CHECK(r.best.deeperMm > 4.0);
  // Every orientation was tried; none clears.
  CHECK(r.tries.size() == 12);
}

TEST_CASE("A D run out through the end it goes on from is pressed exactly "
          "its own length",
          "[measure][insertion]") {
  // The pendulum rod on the anchor arbor: the press at the arbor's end.
  const Body arbor = Shaft({{-4.8, 0.0, kR}, {0.0, 20.0, kR, kFlat}});
  const Body hub = Hub(10.0, 20.0, PressDBore(10.0, 20.0));
  const auto r = Check(hub, arbor, Measure::InsertionEntry::Positive);
  CHECK(r.pass);
  CHECK(r.best.rotationDeg == 0.0);
  CHECK(r.best.pressMm == Approx(10.0).margin(2 * r.stepMm));
  CHECK(r.best.excessMm <= r.leadInMm);
  // From the other end it is forced over the round pivot.
  CHECK_FALSE(Check(hub, arbor, Measure::InsertionEntry::Negative).pass);
  // Either side: the one that works is reported.
  const auto e = Check(hub, arbor, Measure::InsertionEntry::Either);
  CHECK(e.pass);
  CHECK(e.best.positive);
}

TEST_CASE("A pivot at the running gap under the bore slides through it",
          "[measure][insertion]") {
  // The new arbor: the pivot the hub passes is a D the running gap under
  // the bore, the press D only where the hub sits.
  const Body arbor = Shaft({{-4.8, 2.0, kSlideR, kSlideFlat},
                            {2.0, 12.0, kR, kFlat},
                            {12.0, 20.0, kR}});
  const Body hub = Hub(2.0, 12.0, PressDBore(2.0, 12.0));
  const auto r = Check(hub, arbor, Measure::InsertionEntry::Negative);
  CHECK(r.pass);
  CHECK(r.best.excessMm <= r.leadInMm);
  // Pushed from the round end, the round binds in the bore's flat.
  CHECK_FALSE(Check(hub, arbor, Measure::InsertionEntry::Positive).pass);
  // Read as modelled (no allowance) the press is no press at all.
  const auto m = Check(hub, arbor, Measure::InsertionEntry::Negative, 0.0);
  CHECK(m.pass);
  CHECK(m.best.pressMm == 0.0);
}

TEST_CASE("A plain press of its own length passes; a bore too small for its "
          "whole length does not",
          "[measure][insertion]") {
  const Body shaft =
      Make(BRepPrimAPI_MakeCylinder(Up(0), 4.0, 30.0).Shape(), "shaft");
  // Interference in the model, no allowance: a ring at the shaft's end.
  const Body ring =
      Make(BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(Up(20), 8.0, 10.0).Shape(),
                           BRepPrimAPI_MakeCylinder(Up(19), 3.99, 12.0).Shape())
               .Shape(),
           "ring");
  const auto r = Check(ring, shaft, Measure::InsertionEntry::Positive, 0.0);
  CHECK(r.pass);
  CHECK(r.best.pressMm == Approx(10.0).margin(2 * r.stepMm));
  CHECK(r.best.worstVolumeMm3 > 0.0);

  // The same bore seated mid-shaft has the rest of the shaft to pass,
  // from either end.
  const Body mid =
      Make(BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(Up(10), 8.0, 10.0).Shape(),
                           BRepPrimAPI_MakeCylinder(Up(9), 3.95, 12.0).Shape())
               .Shape(),
           "ring");
  const auto f = Check(mid, shaft, Measure::InsertionEntry::Either, 0.0);
  CHECK_FALSE(f.pass);
  CHECK(f.best.excessMm == Approx(10.0).margin(2 * f.stepMm));
  CHECK(f.best.fromMm == Approx(20.0).margin(2 * f.stepMm));
}

TEST_CASE("A part slides at the orientation that clears and says which",
          "[measure][insertion]") {
  // A round shaft with a lug at -X on its entry end; the hub seats on the
  // plain round, its keyway at +X as it is placed: only turned half round
  // does the keyway pass the lug.
  TopoDS_Shape s = BRepPrimAPI_MakeCylinder(Up(0), kR, 30.0).Shape();
  s = BRepAlgoAPI_Fuse(
          s, BRepPrimAPI_MakeBox(gp_Pnt(-kR - 1.0, -0.8, 15.0), 1.5, 1.6, 15.0)
                 .Shape())
          .Shape();
  const Body shaft = Make(s, "shaft");
  TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(Up(-1), kR + kGap, 12.0).Shape();
  bore = BRepAlgoAPI_Fuse(bore, BRepPrimAPI_MakeBox(gp_Pnt(0.0, -1.0, -1.0),
                                                    kR + 1.4, 2.0, 12.0)
                                    .Shape())
             .Shape();
  const Body hub = Hub(0.0, 10.0, bore);
  const auto r = Check(hub, shaft, Measure::InsertionEntry::Positive);
  CHECK(r.pass);
  CHECK(r.best.rotationDeg == Approx(180.0));
  CHECK_FALSE(r.tries.front().pass);
}

TEST_CASE("The insertion check refuses what it cannot read",
          "[measure][insertion]") {
  const Body shaft =
      Make(BRepPrimAPI_MakeCylinder(Up(0), 4.0, 30.0).Shape(), "shaft");
  Measure::InsertionOptions o;
  CHECK_FALSE(
      Measure::Insertion({shaft}, I, {shaft}, I, O, glm::dvec3(0.0), o));
  CHECK_FALSE(Measure::Insertion({}, I, {shaft}, I, O, Z, o));
  o.allowanceMm = -0.1;
  CHECK_FALSE(Measure::Insertion({shaft}, I, {shaft}, I, O, Z, o));
  // A screw on its nut goes on by turning, not by sliding.
  o.allowanceMm = 0.0;
  Body screw = shaft, nut = shaft;
  screw.threads.push_back(ThreadMark{});
  nut.threads.push_back(ThreadMark{});
  const auto r = Measure::Insertion({screw}, I, {nut}, I, O, Z, o);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("turning") != std::string::npos);
}
