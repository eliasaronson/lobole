#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/FrameMath.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/SketchFeature.h"

#include <filesystem>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

namespace {

fs::path TempPath(const std::string& name) {
  return fs::temp_directory_path() / name;
}

// A 40x40 plate `t` thick with four holes on a 20 mm square pattern centred
// at (20, 20) — the bolt circle a frame is built from. `plateId` is the
// extrude that OWNS the body (a cut modifies it without changing createdBy),
// so every element name below is keyed on that id.
std::unique_ptr<Document> BuildPlate(double t, uint64_t& plateId) {
  auto doc = std::make_unique<Document>();
  auto outline = std::make_unique<SketchFeature>("Outline", kXY);
  outline->AddRectangle({0, 0}, {40, 40});
  doc->AddFeature(std::move(outline));
  const uint64_t outlineId = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Plate", outlineId, t,
                                                   ExtrudeMode::NewBody));
  plateId = doc->GetFeature(1)->GetId();

  auto holes = std::make_unique<SketchFeature>("Holes", kXY);
  // Order matters: the frame's X points at the FIRST of them.
  holes->AddEntity(sketch::Circle{{30, 20}, 1.5});
  holes->AddEntity(sketch::Circle{{20, 30}, 1.5});
  holes->AddEntity(sketch::Circle{{10, 20}, 1.5});
  holes->AddEntity(sketch::Circle{{20, 10}, 1.5});
  doc->AddFeature(std::move(holes));
  const uint64_t holesId = doc->GetFeature(2)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Bores", holesId, t + 1.0,
                                                   ExtrudeMode::Cut));
  doc->Regenerate();
  return doc;
}

// Every cylindrical face of the body created by `owner`, as ElementRefs, in
// the order the element map holds them.
std::vector<ElementRef> Bores(const Document& doc, uint64_t owner) {
  std::vector<ElementRef> out;
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy != owner)
      continue;
    for (const auto& [index, name] : b.elementMap.names)
      if (const auto info = Measure::DescribeFaceAt(b, index);
          info && info->kind == "cylinder")
        out.push_back(ElementRef{owner, name});
  }
  return out;
}

// The name of the body's one face whose outward normal is +Z.
std::string TopFace(const Document& doc, uint64_t owner) {
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy != owner)
      continue;
    for (const auto& [index, name] : b.elementMap.names)
      if (const auto info = Measure::DescribeFaceAt(b, index);
          info && info->kind == "plane" && info->normal.z > 0.99f)
        return name;
  }
  return {};
}

uint64_t AddFrame(Document& doc, std::unique_ptr<FrameFeature> f) {
  doc.AddFeature(std::move(f));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

const FrameFeature& FrameAt(Document& doc, uint64_t id) {
  auto* f = dynamic_cast<FrameFeature*>(doc.FindFeatureById(id));
  REQUIRE(f != nullptr);
  return *f;
}

} // namespace

TEST_CASE("An explicit frame sits where it says and is orthonormal",
          "[frame]") {
  Document doc;
  auto f = std::make_unique<FrameFeature>("Seat", FrameKind::Explicit);
  f->origin[0] = 3.0;
  f->origin[1] = 4.0;
  f->origin[2] = 5.0;
  f->zAxis = glm::vec3(0.0f, 1.0f, 0.0f);
  // Deliberately not perpendicular to Z, and not unit: Build fixes both.
  f->xAxis = glm::vec3(2.0f, 7.0f, 0.0f);
  const uint64_t id = AddFrame(doc, std::move(f));
  REQUIRE(doc.Regenerate());

  const FrameFeature& fr = FrameAt(doc, id);
  CHECK(fr.GetError().empty());
  CHECK_THAT(fr.Origin().x, WithinAbs(3.0f, 1e-5f));
  CHECK_THAT(fr.Origin().y, WithinAbs(4.0f, 1e-5f));
  CHECK_THAT(fr.Origin().z, WithinAbs(5.0f, 1e-5f));
  CHECK_THAT(fr.ZAxis().y, WithinAbs(1.0f, 1e-5f));
  // X re-orthogonalized against Z, then normalized: (1,0,0).
  CHECK_THAT(fr.XAxis().x, WithinAbs(1.0f, 1e-5f));
  CHECK_THAT(glm::dot(fr.XAxis(), fr.ZAxis()), WithinAbs(0.0f, 1e-6f));
  CHECK_THAT(glm::length(fr.YAxis()), WithinAbs(1.0f, 1e-6f));
  // Right-handed: X cross Y = Z.
  CHECK_THAT(glm::dot(glm::cross(fr.XAxis(), fr.YAxis()), fr.ZAxis()),
             WithinAbs(1.0f, 1e-6f));
  // A datum contributes no body.
  CHECK(doc.GetBodies().empty());
}

TEST_CASE("A zero z_axis is refused rather than silently defaulted",
          "[frame]") {
  Document doc;
  auto f = std::make_unique<FrameFeature>("Bad", FrameKind::Explicit);
  f->zAxis = glm::vec3(0.0f);
  const uint64_t id = AddFrame(doc, std::move(f));
  doc.Regenerate();
  CHECK_THAT(FrameAt(doc, id).GetError(), ContainsSubstring("z_axis is zero"));
}

TEST_CASE("A frame from a planar face is its centroid and outward normal",
          "[frame]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);
  const std::string top = TopFace(*doc, plate);
  REQUIRE(!top.empty());

  auto f = std::make_unique<FrameFeature>("Top", FrameKind::FromElement);
  f->elements = {ElementRef{plate, top}};
  const uint64_t id = AddFrame(*doc, std::move(f));
  REQUIRE(doc->Regenerate());

  const FrameFeature& fr = FrameAt(*doc, id);
  CHECK(fr.GetError().empty());
  CHECK_THAT(fr.Origin().z, WithinAbs(4.0f, 1e-4f));
  CHECK_THAT(fr.ZAxis().z, WithinAbs(1.0f, 1e-5f));

  // flip turns the outward normal INTO the part, which is what a connector
  // seating something on this face wants.
  auto* mut = dynamic_cast<FrameFeature*>(doc->FindFeatureById(id));
  REQUIRE(mut != nullptr);
  mut->flip = true;
  REQUIRE(doc->Regenerate());
  CHECK_THAT(FrameAt(*doc, id).ZAxis().z, WithinAbs(-1.0f, 1e-5f));
}

TEST_CASE("A frame from a bolt circle centres the pattern and aims X at the "
          "first hole",
          "[frame]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);
  const std::vector<ElementRef> bores = Bores(*doc, plate);
  REQUIRE(bores.size() == 4);

  auto f = std::make_unique<FrameFeature>("Pattern", FrameKind::FromElement);
  f->elements = bores;
  const uint64_t id = AddFrame(*doc, std::move(f));
  REQUIRE(doc->Regenerate());

  const FrameFeature& fr = FrameAt(*doc, id);
  INFO(fr.GetError());
  CHECK(fr.GetError().empty());
  // The four holes sit on a 10 mm radius about (20, 20).
  CHECK_THAT(fr.Origin().x, WithinAbs(20.0f, 1e-3f));
  CHECK_THAT(fr.Origin().y, WithinAbs(20.0f, 1e-3f));
  // Their axes are Z, so the frame's is too (sign is the kernel's; both are
  // legitimate, and `flip` is how a caller picks).
  CHECK_THAT(std::abs(fr.ZAxis().z), WithinAbs(1.0f, 1e-5f));
  // X points at the FIRST element named — the frame is oriented by the
  // pattern, not by an accident of face order.
  glm::vec3 o, z, x;
  bool circular = false;
  std::string why;
  REQUIRE(
      frame::ElementFrame(doc->GetBodies(), bores[0], o, z, x, circular, why));
  const glm::vec3 toFirst = glm::normalize(o - fr.Origin());
  CHECK_THAT(glm::dot(fr.XAxis(), toFirst), WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("A bolt circle needs circular elements and one axis", "[frame]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);
  const std::vector<ElementRef> bores = Bores(*doc, plate);
  REQUIRE(bores.size() == 4);

  SECTION("a planar face among them is refused, naming which") {
    auto f = std::make_unique<FrameFeature>("Mixed", FrameKind::FromElement);
    f->elements = {bores[0], ElementRef{plate, TopFace(*doc, plate)}};
    const uint64_t id = AddFrame(*doc, std::move(f));
    doc->Regenerate();
    CHECK_THAT(FrameAt(*doc, id).GetError(),
               ContainsSubstring("element 2") && ContainsSubstring("planar"));
  }
  SECTION("one hole named twice is refused, not averaged into itself") {
    auto f = std::make_unique<FrameFeature>("Same", FrameKind::FromElement);
    f->elements = {bores[0], bores[0]};
    const uint64_t id = AddFrame(*doc, std::move(f));
    doc->Regenerate();
    CHECK_THAT(FrameAt(*doc, id).GetError(), ContainsSubstring("concentric"));
  }
  SECTION("no element at all says what one needs") {
    auto f = std::make_unique<FrameFeature>("Empty", FrameKind::FromElement);
    const uint64_t id = AddFrame(*doc, std::move(f));
    doc->Regenerate();
    CHECK_THAT(FrameAt(*doc, id).GetError(), ContainsSubstring("bolt circle"));
  }
}

TEST_CASE("A frame naming a missing element refuses, naming what is there",
          "[frame]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);

  SECTION("a name that no longer resolves") {
    auto f = std::make_unique<FrameFeature>("Gone", FrameKind::FromElement);
    f->elements = {ElementRef{plate, "not-a-face"}};
    const uint64_t id = AddFrame(*doc, std::move(f));
    doc->Regenerate();
    CHECK_THAT(FrameAt(*doc, id).GetError(),
               ContainsSubstring("not found") &&
                   ContainsSubstring("list_elements"));
  }
  SECTION("a feature id that creates no body names the ones that do") {
    auto f = std::make_unique<FrameFeature>("Nobody", FrameKind::FromElement);
    f->elements = {ElementRef{99999, "whatever"}};
    const uint64_t id = AddFrame(*doc, std::move(f));
    doc->Regenerate();
    CHECK_THAT(FrameAt(*doc, id).GetError(),
               ContainsSubstring("no body created by feature 99999") &&
                   ContainsSubstring("Plate"));
  }
}

TEST_CASE("An offset frame chains from its parent", "[frame]") {
  Document doc;
  auto base = std::make_unique<FrameFeature>("Base", FrameKind::Explicit);
  base->origin[2] = 10.0;
  const uint64_t baseId = AddFrame(doc, std::move(base));

  auto seat = std::make_unique<FrameFeature>("Seat", FrameKind::Offset);
  seat->parentId = baseId;
  seat->offset[0] = 5.0;
  seat->rotationDeg[2] = 90.0; // yaw about the parent's Z
  const uint64_t seatId = AddFrame(doc, std::move(seat));
  REQUIRE(doc.Regenerate());

  const FrameFeature& fr = FrameAt(doc, seatId);
  INFO(fr.GetError());
  CHECK(fr.GetError().empty());
  CHECK_THAT(fr.Origin().x, WithinAbs(5.0f, 1e-5f));
  CHECK_THAT(fr.Origin().z, WithinAbs(10.0f, 1e-5f));
  CHECK_THAT(fr.XAxis().y, WithinAbs(1.0f, 1e-5f)); // yawed 90 degrees

  SECTION("parent 0 is the part origin, not an error") {
    auto* mut = dynamic_cast<FrameFeature*>(doc.FindFeatureById(seatId));
    REQUIRE(mut != nullptr);
    mut->parentId = 0;
    REQUIRE(doc.Regenerate());
    CHECK_THAT(FrameAt(doc, seatId).Origin().z, WithinAbs(0.0f, 1e-5f));
  }
  SECTION("a parent that is not a Frame is refused, naming the frames") {
    auto* mut = dynamic_cast<FrameFeature*>(doc.FindFeatureById(seatId));
    REQUIRE(mut != nullptr);
    mut->parentId = 4242;
    doc.Regenerate();
    CHECK_THAT(FrameAt(doc, seatId).GetError(),
               ContainsSubstring("not an earlier Frame") &&
                   ContainsSubstring("Base"));
  }
}

TEST_CASE("A frame's origin follows its expression", "[frame][variables]") {
  Document doc;
  REQUIRE(doc.SetParameter("seat_z", "12"));
  auto f = std::make_unique<FrameFeature>("Seat", FrameKind::Explicit);
  f->originExpr[2] = "seat_z";
  const uint64_t id = AddFrame(doc, std::move(f));
  REQUIRE(doc.Regenerate());
  CHECK_THAT(FrameAt(doc, id).Origin().z, WithinAbs(12.0f, 1e-5f));

  REQUIRE(doc.SetParameter("seat_z", "30"));
  CHECK_THAT(FrameAt(doc, id).Origin().z, WithinAbs(30.0f, 1e-5f));
}

// The whole point of G0 (docs/agent-scale-plan.md): a mate anchored to a
// NAMED frame is the mate as designed, so it follows the geometry that
// defined it. A transcribed coordinate does not — that is how the SO-101
// gripper's flange ended up 7.57 mm off its axis.
TEST_CASE("A mate to a named frame follows the boss it was built from",
          "[frame][assembly][solve]") {
  // The boss: a 6 mm circle extruded `boss_h` tall, with a frame on its top
  // face. Change the variable and the frame moves with the face.
  auto part = std::make_unique<Document>();
  REQUIRE(part->SetParameter("boss_h", "12"));
  auto sk = std::make_unique<SketchFeature>("Boss profile", kXY);
  sk->AddEntity(sketch::Circle{{0, 0}, 6.0});
  part->AddFeature(std::move(sk));
  const uint64_t sketchId = part->GetFeature(0)->GetId();
  auto pad = std::make_unique<ExtrudeFeature>("Boss", sketchId, 12.0,
                                              ExtrudeMode::NewBody);
  pad->distanceExpr = "boss_h";
  part->AddFeature(std::move(pad));
  const uint64_t bossId = part->GetFeature(1)->GetId();
  REQUIRE(part->Regenerate());

  const std::string top = TopFace(*part, bossId);
  REQUIRE(!top.empty());
  auto f = std::make_unique<FrameFeature>("boss_seat", FrameKind::FromElement);
  f->elements = {ElementRef{bossId, top}};
  const uint64_t frameId = AddFrame(*part, std::move(f));
  REQUIRE(part->Regenerate());

  auto arm = std::make_unique<Document>();
  auto asketch = std::make_unique<SketchFeature>("Arm profile", kXY);
  asketch->AddRectangle({0, 0}, {4, 4});
  arm->AddFeature(std::move(asketch));
  arm->AddFeature(std::make_unique<ExtrudeFeature>(
      "Arm", arm->GetFeature(0)->GetId(), 2.0, ExtrudeMode::NewBody));
  REQUIRE(arm->Regenerate());

  const assembly::PartResolver resolve = [&](uint64_t id) -> const Document* {
    return id == 1 ? part.get() : arm.get();
  };
  AssemblyDocument asmDoc;
  Occurrence base;
  base.partId = 1;
  base.fixed = true;
  asmDoc.AddOccurrence(base); // id 1
  Occurrence mover;
  mover.partId = 2;
  mover.position = glm::vec3(100.0f, 100.0f, 100.0f); // parked far away
  asmDoc.AddOccurrence(mover);                        // id 2

  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.frameId = frameId; // the mate AS DESIGNED
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);

  assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
  INFO(asmDoc.GetMates()[0].error);
  CHECK(rep.erroredMates == 0);
  const Occurrence* placed = asmDoc.FindById(2);
  REQUIRE(placed != nullptr);
  CHECK_THAT(placed->position.z, WithinAbs(12.0f, 1e-3f));

  // The boss grows. Nothing about the mate changes, and the arm follows —
  // which is the property a transcribed coordinate cannot have.
  REQUIRE(part->SetParameter("boss_h", "30"));
  rep = assembly::Solve(asmDoc, resolve);
  CHECK(rep.erroredMates == 0);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(30.0f, 1e-3f));

  SECTION("a frame that no longer resolves refuses the mate, not silently "
          "reusing the last good pose") {
    auto* mut = dynamic_cast<FrameFeature*>(part->FindFeatureById(frameId));
    REQUIRE(mut != nullptr);
    mut->elements = {ElementRef{bossId, "gone"}};
    REQUIRE(!part->Regenerate()); // a failed datum stops regeneration
    rep = assembly::Solve(asmDoc, resolve);
    CHECK(rep.erroredMates == 1);
    CHECK_THAT(asmDoc.GetMates()[0].error,
               ContainsSubstring("boss_seat") &&
                   ContainsSubstring("did not resolve"));
  }
  SECTION("a frame id that is not a Frame refuses, naming the frames") {
    asmDoc.GetMates()[0].a.anchor.frameId = 77777;
    rep = assembly::Solve(asmDoc, resolve);
    CHECK(rep.erroredMates == 1);
    CHECK_THAT(asmDoc.GetMates()[0].error,
               ContainsSubstring("is not a Frame feature") &&
                   ContainsSubstring("boss_seat"));
  }
}

TEST_CASE("Frames round-trip through the document format", "[frame][io]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);
  REQUIRE(doc->SetParameter("seat_z", "7"));
  const std::vector<ElementRef> bores = Bores(*doc, plate);
  REQUIRE(bores.size() == 4);

  auto pattern =
      std::make_unique<FrameFeature>("bolt_circle", FrameKind::FromElement);
  pattern->elements = bores;
  pattern->flip = true;
  const uint64_t patternId = AddFrame(*doc, std::move(pattern));

  auto explicitFrame =
      std::make_unique<FrameFeature>("seat", FrameKind::Explicit);
  explicitFrame->originExpr[2] = "seat_z";
  explicitFrame->zAxis = glm::vec3(0.0f, 1.0f, 0.0f);
  const uint64_t seatId = AddFrame(*doc, std::move(explicitFrame));

  auto offsetFrame =
      std::make_unique<FrameFeature>("lifted", FrameKind::Offset);
  offsetFrame->parentId = patternId;
  offsetFrame->offset[2] = 3.0;
  offsetFrame->rotationDeg[2] = 45.0;
  const uint64_t liftedId = AddFrame(*doc, std::move(offsetFrame));
  REQUIRE(doc->Regenerate());

  const glm::mat4 before[3] = {FrameAt(*doc, patternId).Matrix(),
                               FrameAt(*doc, seatId).Matrix(),
                               FrameAt(*doc, liftedId).Matrix()};

  const fs::path path = TempPath("frame-roundtrip.json");
  REQUIRE(testio::Save(*doc, path.string()));
  auto reloaded = testio::Load(path.string());
  REQUIRE(reloaded);

  const uint64_t ids[3] = {patternId, seatId, liftedId};
  for (int i = 0; i < 3; i++) {
    auto* fr = dynamic_cast<FrameFeature*>(reloaded->FindFeatureById(ids[i]));
    REQUIRE(fr != nullptr);
    INFO(fr->GetName() << ": " << fr->GetError());
    CHECK(fr->GetError().empty());
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        CHECK_THAT(fr->Matrix()[c][r], WithinAbs(before[i][c][r], 1e-4f));
  }
  // The expression survived as an expression, not as the 7 it evaluated to.
  auto* seat = dynamic_cast<FrameFeature*>(reloaded->FindFeatureById(seatId));
  REQUIRE(seat != nullptr);
  CHECK(seat->originExpr[2] == "seat_z");
  CHECK(
      dynamic_cast<FrameFeature*>(reloaded->FindFeatureById(patternId))->flip);

  std::error_code ec;
  fs::remove(path, ec);
}

// How tests/golden/v86-frames.json was made. Set DESIGNER_WRITE_GOLDEN=<path>
// to write it again.
TEST_CASE("the v86 frames golden builds from this recipe", "[frame][golden]") {
  uint64_t plate = 0;
  auto doc = BuildPlate(4.0, plate);
  REQUIRE(doc->SetParameter("seat_z", "6"));
  const std::vector<ElementRef> bores = Bores(*doc, plate);
  REQUIRE(bores.size() == 4);

  auto pattern =
      std::make_unique<FrameFeature>("bolt_circle", FrameKind::FromElement);
  pattern->elements = bores;
  const uint64_t patternId = AddFrame(*doc, std::move(pattern));

  auto seat = std::make_unique<FrameFeature>("seat", FrameKind::Explicit);
  seat->originExpr[2] = "seat_z";
  AddFrame(*doc, std::move(seat));

  auto lifted = std::make_unique<FrameFeature>("lifted", FrameKind::Offset);
  lifted->parentId = patternId;
  lifted->offset[2] = 10.0;
  AddFrame(*doc, std::move(lifted));
  REQUIRE(doc->Regenerate());
  REQUIRE(doc->GetBodies().size() == 1);

  const char* out = std::getenv("DESIGNER_WRITE_GOLDEN");
  const fs::path path = out ? fs::path(out) : TempPath("frames-v86.json");
  REQUIRE(testio::Save(*doc, path.string()));
  auto reloaded = testio::Load(path.string());
  REQUIRE(reloaded);
  CHECK(reloaded->GetBodies().size() == 1);
  if (!out) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}
