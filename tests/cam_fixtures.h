#pragma once

#ifdef HAVE_CAM

// Shared fixtures for the CAM test files (test_cam*.cpp).
//
// They were one 5,965-line test_cam.cpp until 2026-09-06. Splitting it
// (docs/architecture.md#source-layout) turned up a real
// bug on the way: `#endif // HAVE_CAM` sat at line 4322 with 38 test cases
// appended AFTER it, so -DENABLE_CAM=OFF did not compile. One guard per file
// cannot drift that way.

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamFeeds.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamMount.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamStockMesh.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/cam/JobDocument.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "ui/MachineActions.h"

#include <catch2/catch_test_macros.hpp>

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <set>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numbers>
#include <unistd.h>

// CAM's testable half (docs/cam/README.md): bodies in, moves out, then G-code.
// Every number here is one a machinist could check by hand, because the
// failure mode this suite exists to prevent is scrap metal.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A 40 x 30 x 20 block at the origin, its top at z = 20.
std::unique_ptr<Document> BlockDoc(double w = 40, double h = 30,
                                   double d = 20) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), w, h, d));
  return doc;
}

// A rotary this test's job posts for, stated as the identity -- along X, the
// chuck at the minus end, driven as A, the top of the part moving -Y on a
// positive jog (right-handed) -- and nothing holding a part whose model
// starts at its stock's end: the test is about the cut, not the chuck
// (docs/cam/rotary.md#how-the-rotary-is-installed).
inline void RotaryMachine(cam::Setup& s) {
  s.machine.hasRotaryAxis = true;
  s.machine.rotary.along = 'x';
  s.machine.rotary.chuckEnd = -1;
  s.machine.rotary.word = 'A';
  s.machine.rotary.topAxis = 'y';
  s.machine.rotary.topSign = -1;
  s.chuck = 0;
  s.tailstock = 0;
}

cam::Setup BasicSetup() {
  cam::Setup s;
  s.name = "Job";
  s.safeZ = 25.0;
  cam::Tool t;
  t.number = 1;
  t.name = "6mm flat";
  t.diameter = 6.0;
  t.feedCut = 600.0;
  t.feedPlunge = 200.0;
  t.spindleRpm = 12000.0;
  s.tools.push_back(t);
  return s;
}

} // namespace

// --- G2: the verbs, and the depths a click derives ------------------------

namespace {

// A 40 x 30 x 20 block with a 20 x 14 pocket cleared from its top down to
// z = 12 — the part the GUI's "Clear a pocket" verb is aimed at.
std::unique_ptr<Document> PocketDoc(bool island = false) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(10, 8, 12), 20, 14, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));
  if (island) {
    const uint64_t pocket = doc->GetFeature(2)->GetId();
    doc->AddFeature(
        std::make_unique<BoxFeature>("Boss", glm::vec3(18, 14, 12), 5, 5, 8));
    const uint64_t bossId = doc->GetFeature(3)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("WithBoss", pocket, bossId,
                                                     BooleanOp::Union));
  }
  return doc;
}

// A 120 x 90 x 20 block with an 80 x 60 pocket down to z = 12, and whichever
// inner wires the test needs in that floor: a HOLE through it at (60, 45), a
// BOSS standing on it at (44, 34), or both. Big enough that a 6 mm cutter has
// somewhere to go — the 20 x 14 pocket above leaves a region narrower than
// the tool once anything stands in it.
std::unique_ptr<Document> FlangeDoc(double holeDia, bool boss) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 120, 90, 20));
  uint64_t solid = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(20, 15, 12), 80, 60, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", solid, voidId,
                                                   BooleanOp::Subtract));
  solid = doc->GetFeature(2)->GetId();
  if (boss) {
    doc->AddFeature(
        std::make_unique<BoxFeature>("Boss", glm::vec3(40, 30, 12), 8, 8, 6));
    const uint64_t bossId = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("WithBoss", solid, bossId,
                                                     BooleanOp::Union));
    solid = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
  }
  if (holeDia > 0.0) {
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Bore", glm::vec3(60, 45, -1), glm::vec3(0, 0, 1), holeDia * 0.5,
        30.0));
    const uint64_t bore = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("Holed", solid, bore,
                                                     BooleanOp::Subtract));
  }
  return doc;
}

// A 40 x 30 x 20 block with a 12 x 14 slot straight through it, closed on
// every side. The case a face pick cannot reach: a through opening has no
// floor, so its walls are the only thing to point at.
std::unique_ptr<Document> SlottedDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(14, 8, -1), 12, 14, 22));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Slot", block, voidId,
                                                   BooleanOp::Subtract));
  return doc;
}

// The vertical planar face whose centroid is nearest (x, y) — a click on a
// wall.
ElementRef WallNear(const Document& doc, double x, double y) {
  ElementRef out;
  double best = 1e18;
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (std::abs(n.z) > 0.01f)
        continue;
      const double d = (c.x - x) * (c.x - x) + (c.y - y) * (c.y - y);
      if (d < best) {
        best = d;
        out = ElementRef{b.createdBy, name};
      }
    }
  }
  return out;
}

// The up-facing planar face whose centroid sits at height `z`. This is what
// the GUI hands the engine when the user clicks that face.
ElementRef UpFaceAt(const Document& doc, double z) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (n.z < 0.99f || std::abs(c.z - z) > 1e-6)
        continue;
      return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

// A vertical face of the block whose outward normal points along -X or +X —
// the wall an edge break follows.
ElementRef SideWall(const Document& doc) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (std::abs(n.z) < 0.01f)
        return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

// A tool of a given shape, otherwise the setup's own.
cam::Tool ShapedTool(cam::ToolType type, double diameter, double angle = 90.0) {
  cam::Tool t;
  t.number = 1;
  t.diameter = diameter;
  t.type = type;
  t.tipAngleDeg = angle;
  t.feedCut = 600.0;
  t.feedPlunge = 200.0;
  t.spindleRpm = 12000.0;
  return t;
}

// A 40 x 30 x 20 block with a 6 mm through bore at (20, 15) — the part a
// drill operation is aimed at.
std::unique_ptr<Document> DrilledDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::vec3(20, 15, 0), glm::vec3(0, 0, 1), 8.0, 20.0));
  const uint64_t bore = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Drilled", block, bore,
                                                   BooleanOp::Subtract));
  return doc;
}

// The vertical cylindrical face (the bore wall) — what list_elements hands
// an agent that picked the hole.
ElementRef BoreWall(const Document& doc) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 o(0.0f), axis(0.0f), x(0.0f);
      if (!b.shape.FaceAxis(index, o, axis, x))
        continue;
      if (std::abs(axis.z) > 0.99f)
        return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

} // namespace

namespace {

// The floor of the pocket in these fixtures, at z = 12.
ElementRef PocketFloorRef(const Document& doc) {
  ElementRef floor;
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u) && n.z > 0.99f &&
          std::abs(c.z - 12.0f) < 1e-3f)
        floor = ElementRef{b.createdBy, name};
    }
  return floor;
}

// Through the SAME derivation the verb uses, so the job under test is the job
// a click makes — depths, side, stepdown and the pocket/adaptive choice all
// come from the model rather than from numbers typed here.
cam::Setup PocketJob(const Document& doc, const ElementRef& floor,
                     bool useOutline) {
  cam::Setup s = BasicSetup();
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc.GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.useOutline = useOutline;
  s.operations.push_back(op);
  return s;
}

// Does any cutting move pass within `r` of (x, y)?
bool CutsOver(const cam::Toolpath& tp, double x, double y, double r) {
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid &&
        std::hypot(m.to.x - x, m.to.y - y) < r)
      return true;
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// R3: a hole wider than the tool is bored; a deep hole pecks.

namespace {
// A 40 x 30 block with one through hole of `dia` at its centre.
std::unique_ptr<Document> HoleDoc(double dia, double height = 20.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, height));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(20, 15, -1), glm::vec3(0, 0, 1), dia * 0.5,
      height + 2.0));
  const uint64_t hole = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Drilled", block, hole,
                                                   BooleanOp::Subtract));
  return doc;
}
ElementRef HoleWall(const Document& doc) {
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, o, n, u))
        return ElementRef{b.createdBy, name};
    }
  return {};
}
} // namespace

namespace {

// A bar ON the rotary axis (setup-space +X) with a journal turned into it:
// two diameters and two shoulders, the smallest shape with an inside corner a
// ball cannot reach -- so a rotary check reports a real leftover, not zero by
// construction. Defaults: Phi 30 x 40 with a Phi 22 journal from x = 12 to 28.
std::unique_ptr<Document>
NeckedShaftDoc(double radius = 15.0, double length = 40.0, double neckAt = 12.0,
               double neckLength = 16.0, double neckRadius = 11.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), radius, length));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Waist", glm::vec3(neckAt, 0, 0), glm::vec3(1, 0, 0), radius,
      neckLength));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Necked", doc->GetFeature(0)->GetId(), doc->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::vec3(neckAt, 0, 0), glm::vec3(1, 0, 0), neckRadius,
      neckLength));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Shaft", doc->GetFeature(2)->GetId(), doc->GetFeature(3)->GetId(),
      BooleanOp::Union));
  return doc;
}

} // namespace

#endif // HAVE_CAM
