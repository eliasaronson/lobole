// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef HAVE_CAM

// Travel: what happens to a toolpath that runs off the machine's bed.
//
// `CamTravel.cpp` is 617 lines and had no test file of its own, and
// `ClipToMachine` -- the function that rewrites EVERY move against the work
// area -- had no test anywhere. It was reached only through `GenerateAll` on
// fixtures that happened to fit. It is also the function a 4th axis has to
// teach about A, since a clipped move is a move split in two
// (docs/cam/rotary.md#verifying-a-turning-part), which is why it is being
// pinned down first.

#include "cam_fixtures.h"

namespace {

// A straight cutting pass along +X at one depth, sampled every `step` mm --
// the shape every 2.5D strategy emits, and the one whose arithmetic can be
// checked by hand.
cam::Toolpath PassAlongX(double x0, double x1, double y, double z,
                         double step = 5.0) {
  cam::Toolpath tp;
  tp.toolNumber = 1;
  cam::Move start;
  start.kind = cam::Move::Kind::Rapid;
  start.to = {x0, y, z};
  tp.moves.push_back(start);
  for (double x = x0 + step; x <= x1 + 1e-9; x += step) {
    cam::Move m;
    m.kind = cam::Move::Kind::Cut;
    m.to = {x, y, z};
    m.feed = 600.0;
    tp.moves.push_back(m);
  }
  return tp;
}

double CutLengthOutside(const cam::Toolpath& tp, const glm::dvec3& lo,
                        const glm::dvec3& hi) {
  double worst = 0.0;
  for (const cam::Move& m : tp.moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    worst = std::max(
        {worst, lo.x - m.to.x, m.to.x - hi.x, lo.y - m.to.y, m.to.y - hi.y});
  }
  return worst;
}

} // namespace

TEST_CASE("cam travel: a path that fits is not touched", "[cam]") {
  // The control. A clip that rewrites a path it did not need to touch would
  // move every job on every machine that states a work area, which is most of
  // them now -- and every other case below is measured against this one.
  const glm::dvec3 lo(0.0, 0.0, -100.0), hi(200.0, 200.0, 50.0);
  cam::Toolpath tp = PassAlongX(10.0, 100.0, 50.0, -2.0);
  const std::vector<cam::Move> before = tp.moves;
  cam::ClipToMachine(tp, lo, hi, /*safeZ=*/20.0, /*plungeFeed=*/200.0);
  REQUIRE(tp.moves.size() == before.size());
  for (size_t i = 0; i < before.size(); i++) {
    INFO("move " << i);
    CHECK(tp.moves[i].kind == before[i].kind);
    CHECK_THAT(tp.moves[i].to.x, WithinAbs(before[i].to.x, 1e-12));
    CHECK_THAT(tp.moves[i].to.y, WithinAbs(before[i].to.y, 1e-12));
    CHECK_THAT(tp.moves[i].to.z, WithinAbs(before[i].to.z, 1e-12));
  }
  CHECK_THAT(tp.clippedMm, WithinAbs(0.0, 1e-12));
}

TEST_CASE("cam travel: a cut that runs off the bed stops at the edge",
          "[cam]") {
  // The pass runs from X10 to X100 and the bed ends at X60. Forty millimetres
  // of cutting are off the machine, and the numbers below are that sentence
  // in arithmetic.
  const glm::dvec3 lo(0.0, 0.0, -100.0), hi(60.0, 200.0, 50.0);
  cam::Toolpath tp = PassAlongX(10.0, 100.0, 50.0, -2.0);
  cam::ClipToMachine(tp, lo, hi, /*safeZ=*/20.0, /*plungeFeed=*/200.0);

  INFO("worst overshoot " << CutLengthOutside(tp, lo, hi));
  CHECK_THAT(CutLengthOutside(tp, lo, hi), WithinAbs(0.0, 1e-6));
  CHECK_THAT(tp.clippedMm, WithinAbs(40.0, 1e-6));
  // The last thing it does inside is stop ON the boundary, not short of it:
  // a cut that gives up at the previous sample throws away up to a whole
  // step of perfectly reachable material.
  double furthest = 0.0;
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid)
      furthest = std::max(furthest, m.to.x);
  CHECK_THAT(furthest, WithinAbs(60.0, 1e-6));
  // ...and it lifts afterwards rather than dragging the cutter off the edge
  // at depth.
  CHECK(tp.moves.back().kind == cam::Move::Kind::Rapid);
  CHECK_THAT(tp.moves.back().to.z, WithinAbs(20.0, 1e-9));
}

TEST_CASE("cam travel: a cut that leaves and comes back re-enters at depth",
          "[cam]") {
  // Out of the box and back in: the tool must lift at the boundary, travel
  // over at safe Z, and go back DOWN to the depth it was cutting at -- at the
  // plunge feed, not the cutting feed, because it is entering material
  // vertically.
  const glm::dvec3 lo(0.0, 0.0, -100.0), hi(200.0, 200.0, 50.0);
  cam::Toolpath tp;
  tp.toolNumber = 1;
  const auto add = [&](cam::Move::Kind k, glm::dvec3 to, double feed) {
    cam::Move m;
    m.kind = k;
    m.to = to;
    m.feed = feed;
    tp.moves.push_back(m);
  };
  add(cam::Move::Kind::Rapid, {50.0, 50.0, 5.0}, 0.0);
  add(cam::Move::Kind::Cut, {250.0, 50.0, 5.0}, 600.0); // out through X200
  add(cam::Move::Kind::Cut, {250.0, 80.0, 5.0}, 600.0); // entirely outside
  add(cam::Move::Kind::Cut, {50.0, 80.0, 5.0}, 600.0);  // back in
  cam::ClipToMachine(tp, lo, hi, /*safeZ=*/20.0, /*plungeFeed=*/200.0);

  CHECK_THAT(CutLengthOutside(tp, lo, hi), WithinAbs(0.0, 1e-6));
  // 50 out on the way there, 30 entirely outside, 50 on the way back.
  // The boundary carries a 1e-6 slop (kEps), so this is 130 to within a
  // hundredth of a micron rather than exactly.
  CHECK_THAT(tp.clippedMm, WithinAbs(130.0, 1e-4));

  // Somewhere there is a rapid at safe Z followed by a PLUNGE back to 5.
  bool reentered = false;
  for (size_t i = 1; i < tp.moves.size(); i++)
    if (tp.moves[i].kind == cam::Move::Kind::Plunge &&
        tp.moves[i - 1].kind == cam::Move::Kind::Rapid &&
        std::abs(tp.moves[i - 1].to.z - 20.0) < 1e-9 &&
        std::abs(tp.moves[i].to.z - 5.0) < 1e-9 &&
        std::abs(tp.moves[i].feed - 200.0) < 1e-9)
      reentered = true;
  CHECK(reentered);
  // And nothing cuts at depth while crossing the gap: every move between the
  // exit and the re-entry is a rapid.
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid)
      CHECK(m.to.x <= hi.x + 1e-6);
}

TEST_CASE("cam travel: a pass entirely off the bed is dropped whole", "[cam]") {
  const glm::dvec3 lo(0.0, 0.0, -100.0), hi(60.0, 200.0, 50.0);
  cam::Toolpath tp = PassAlongX(100.0, 150.0, 50.0, -2.0);
  cam::ClipToMachine(tp, lo, hi, /*safeZ=*/20.0, /*plungeFeed=*/200.0);
  for (const cam::Move& m : tp.moves)
    CHECK(m.kind == cam::Move::Kind::Rapid);
  CHECK_THAT(tp.clippedMm, WithinAbs(50.0, 1e-6));
}

TEST_CASE("cam travel: clipping keeps the rotary index it was given", "[cam]") {
  // An indexed setup stamps its A on every move (C9). `ClipToMachine` splits
  // moves and INSERTS new ones -- the retract at the boundary, the traverse
  // over, the plunge back -- and those were built from scratch, so they came
  // out with `hasA = false` in the middle of a path where every other move
  // carried one.
  //
  // Harmless in the file, because A is modal and the machine holds the last
  // one. Not harmless in the toolpath: `LastCollinear` breaks a run wherever
  // `hasA` differs, so a clipped indexed path decimated worse than the same
  // path unclipped, and every consumer that reads A off a move saw it blink.
  const glm::dvec3 lo(0.0, 0.0, -100.0), hi(60.0, 200.0, 50.0);
  cam::Toolpath tp = PassAlongX(10.0, 100.0, 50.0, -2.0);
  for (cam::Move& m : tp.moves) {
    m.hasA = true;
    m.a = 90.0;
  }
  cam::ClipToMachine(tp, lo, hi, /*safeZ=*/20.0, /*plungeFeed=*/200.0);
  REQUIRE(!tp.moves.empty());
  for (size_t i = 0; i < tp.moves.size(); i++) {
    INFO("move " << i << " kind " << static_cast<int>(tp.moves[i].kind));
    CHECK(tp.moves[i].hasA);
    CHECK_THAT(tp.moves[i].a, WithinAbs(90.0, 1e-12));
  }

  // ...and a 3-axis path stays free of A, which is the other half: an
  // inserted move must inherit what the path had, not assert something.
  cam::Toolpath plain = PassAlongX(10.0, 100.0, 50.0, -2.0);
  cam::ClipToMachine(plain, lo, hi, 20.0, 200.0);
  for (const cam::Move& m : plain.moves)
    CHECK_FALSE(m.hasA);
}

#endif // HAVE_CAM
