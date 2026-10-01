// The model is double (docs/conventions.md#the-model-is-double): a primitive
// stores the position it was given, so a face made from it lies where a
// plane typed at the same number does; an occurrence's placement and a
// mate's values save and load back bit for bit; and a solve or a
// regeneration repeated changes nothing. The sketch's half of this is
// test_sketch_precision.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {

// Awkward numbers: none of 0.1 * k, 1000.1 or their negatives is a binary
// fraction, so each one tells a double from its float neighbour.
std::vector<double> Heights() {
  std::vector<double> h;
  for (int k = 1; k <= 60; k++) {
    h.push_back(0.1 * k);
    h.push_back(-0.1 * k);
  }
  for (double t : {13.4, 24.7, 1e3 + 0.1, -(1e3 + 0.1), 99.9, -13.4})
    h.push_back(t);
  return h;
}

// The plane at height z whose normal points along `up` (+1 or -1) on Z.
SketchPlane PlaneAt(double z, double up) {
  return SketchPlane(glm::dvec3(0, 0, z), glm::dvec3(0, 0, up),
                     glm::dvec3(1, 0, 0));
}

uint64_t LastId(const Document& doc) {
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// A 20 x 20 x 5 box whose BOTTOM face is at z = t: the position the caller
// typed is the face, with no arithmetic between them.
uint64_t BoxOn(Document& doc, double t) {
  doc.AddFeature(std::make_unique<BoxFeature>("Box", glm::dvec3(0.0, 0.0, t),
                                              20.0, 20.0, 5.0));
  return LastId(doc);
}

// The middle 10 x 10 of that box, on the plane at t facing down (out of it).
uint64_t MiddleBelow(Document& doc, double t) {
  auto sk = std::make_unique<SketchFeature>("Sketch", PlaneAt(t, -1.0));
  // Facing down, V is -Y.
  sk->AddRectangle({5, -5}, {15, -15});
  doc.AddFeature(std::move(sk));
  return LastId(doc);
}

// Planar faces at height t (within 1 um) whose outward normal points along
// `dir` on Z.
int FacesAt(const Body& body, double t, double dir) {
  int n = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    gp_Dir nd = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      nd.Reverse();
    if (nd.Z() * dir > 0.5 && std::abs(surf.Plane().Location().Z() - t) < 1e-3)
      n++;
  }
  return n;
}

// The bottom face's height, read off the B-rep.
double BottomOf(const Body& body) {
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    gp_Dir nd = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      nd.Reverse();
    if (nd.Z() < -0.5)
      return surf.Plane().Location().Z();
  }
  return NAN;
}

} // namespace

TEST_CASE("A primitive stores the position it was given",
          "[modeling][precision]") {
  // A box at z = 24.7 used to sit at 24.70000076: its position was a
  // glm::vec3. Now the face is at the number that was typed.
  for (const double t : Heights()) {
    CAPTURE(t);
    Document doc;
    BoxOn(doc, t);
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(BottomOf(doc.GetBodies()[0]) == t);
  }
  const SphereFeature s("S", glm::dvec3(0.1, -1e3 - 0.1, 24.7), 0.3);
  CHECK(s.center == glm::dvec3(0.1, -1e3 - 0.1, 24.7));
  const CylinderFeature c("C", glm::dvec3(13.4, 0.7, -0.1),
                          glm::dvec3(0.1, 0.2, 0.3), 1.1, 2.2);
  CHECK(c.center == glm::dvec3(13.4, 0.7, -0.1));
  CHECK(c.direction == glm::dvec3(0.1, 0.2, 0.3));
}

TEST_CASE("A cut from a primitive's face opens it at any height",
          "[modeling][precision][extrude]") {
  // A pocket from a plane typed at a box's face height. Through float the
  // face sat microns off the plane, and the pocket kept a sealed skin over
  // itself -- unless the tool was moved behind the plane to find it.
  for (const double t : Heights()) {
    CAPTURE(t);
    Document doc;
    BoxOn(doc, t);
    const uint64_t sk = MiddleBelow(doc, t);
    doc.AddFeature(std::make_unique<ExtrudeFeature>(
        "Pocket", sk, 3.0, ExtrudeMode::Cut, /*reversed=*/true));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies()[0];
    CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
    CHECK(body.shape.SolidCount() == 1);
    CHECK(FacesAt(body, t, +1.0) == 0); // nothing looks into the part there
    CHECK_THAT(body.shape.Volume(),
               WithinRel(20.0 * 20.0 * 5.0 - 10.0 * 10.0 * 3.0, 1e-9));
  }
}

TEST_CASE("An add from a primitive's face fuses at any height",
          "[modeling][precision][extrude]") {
  for (const double t : Heights()) {
    CAPTURE(t);
    Document doc;
    BoxOn(doc, t);
    const uint64_t sk = MiddleBelow(doc, t);
    doc.AddFeature(
        std::make_unique<ExtrudeFeature>("Boss", sk, 2.0, ExtrudeMode::Add));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies()[0];
    CHECK(body.shape.SolidCount() == 1);
    CHECK_THAT(body.shape.Volume(),
               WithinRel(20.0 * 20.0 * 5.0 + 10.0 * 10.0 * 2.0, 1e-9));
  }
}

TEST_CASE("Two primitives placed face to face fuse at any coordinate",
          "[modeling][precision][boolean]") {
  // B's left face is at x = t, where A's right face is: A starts at t - 2
  // and is 2 wide, so the two meet to the double's last bit or so.
  for (const double t : Heights()) {
    CAPTURE(t);
    Document doc;
    doc.AddFeature(std::make_unique<BoxFeature>(
        "A", glm::dvec3(t - 2.0, 0.1, -0.7), 2.0, 3.0, 4.0));
    const uint64_t a = LastId(doc);
    doc.AddFeature(std::make_unique<BoxFeature>("B", glm::dvec3(t, 0.1, -0.7),
                                                1.0, 3.0, 4.0));
    const uint64_t b = LastId(doc);
    doc.AddFeature(
        std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].shape.SolidCount() == 1);
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
               WithinRel(3.0 * 3.0 * 4.0, 1e-9));
  }
}

TEST_CASE("A part's primitives save and load back bit for bit",
          "[modeling][precision][serialization]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Box", glm::dvec3(0.1, -1e3 - 0.1, 24.7), 0.3, 13.4, 0.7));
  doc.AddFeature(std::make_unique<SphereFeature>(
      "Ball", glm::dvec3(-0.3, 0.1 * 7, 1e3 + 0.1), 0.1 * 3));
  doc.AddFeature(
      std::make_unique<CylinderFeature>("Pin", glm::dvec3(13.4, -24.7, 0.1),
                                        glm::dvec3(0.1, 0.2, 0.3), 1.1, 2.2));
  const auto path = std::filesystem::temp_directory_path() /
                    "designer_model_precision_part.json";
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->FeatureCount() == 3);
  const auto* box = dynamic_cast<const BoxFeature*>(loaded->GetFeature(0));
  const auto* ball = dynamic_cast<const SphereFeature*>(loaded->GetFeature(1));
  const auto* pin = dynamic_cast<const CylinderFeature*>(loaded->GetFeature(2));
  REQUIRE((box && ball && pin));
  CHECK(box->position == glm::dvec3(0.1, -1e3 - 0.1, 24.7));
  CHECK(ball->center == glm::dvec3(-0.3, 0.1 * 7, 1e3 + 0.1));
  CHECK(pin->center == glm::dvec3(13.4, -24.7, 0.1));
  CHECK(pin->direction == glm::dvec3(0.1, 0.2, 0.3));
  for (size_t i = 0; i < 3; i++)
    CHECK(DocumentIO::FeatureToJson(*loaded->GetFeature(i)) ==
          DocumentIO::FeatureToJson(*doc.GetFeature(i)));
}

TEST_CASE("Regenerating a part of primitives changes nothing",
          "[modeling][precision]") {
  Document doc;
  BoxOn(doc, 1e3 + 0.1);
  const uint64_t sk = MiddleBelow(doc, 1e3 + 0.1);
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Pocket", sk, 3.0, ExtrudeMode::Cut, /*reversed=*/true));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  const double volume = doc.GetBodies()[0].shape.Volume();
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi, /*tight=*/true);
  for (int i = 0; i < 10; i++) {
    REQUIRE(doc.Regenerate());
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].shape.Volume() == volume);
    glm::dvec3 lo2, hi2;
    doc.GetBodies()[0].shape.GetBoundingBox(lo2, hi2, /*tight=*/true);
    CHECK(lo2 == lo);
    CHECK(hi2 == hi);
  }
}

namespace {

// A 4 x 4 x h pad whose top cap ("<ext>/cap1") is what the mates anchor on.
std::unique_ptr<Document> Pad(double h, uint64_t& extrudeId) {
  auto doc = std::make_unique<Document>();
  auto sk =
      std::make_unique<SketchFeature>("Sketch", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 4});
  doc->AddFeature(std::move(sk));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc->GetFeature(0)->GetId(), h, ExtrudeMode::NewBody));
  extrudeId = doc->GetFeature(1)->GetId();
  doc->Regenerate();
  return doc;
}

const glm::dquat kTilt =
    glm::angleAxis(0.7, glm::normalize(glm::dvec3(1.0, 2.0, 3.0)));

// A fixed base far from the origin, turned on an awkward axis, and an arm on
// a revolute mate at an awkward angle and seat.
AssemblyDocument Mechanism(uint64_t ext) {
  AssemblyDocument asmDoc;
  Occurrence base;
  base.partId = 1;
  base.position = {1e3 + 0.1, -0.3, 24.7};
  base.orientation = kTilt;
  base.fixed = true;
  asmDoc.AddOccurrence(base); // id 1
  Occurrence arm;
  arm.partId = 1;
  arm.position = {0.1, 0.2, 0.3};
  asmDoc.AddOccurrence(arm); // id 2
  Mate m;
  m.type = MateType::Revolute;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  m.rotation = 0.1 * 3;
  m.offset = 13.4;
  asmDoc.AddMate(m);
  return asmDoc;
}

} // namespace

TEST_CASE("An occurrence's placement saves and loads back bit for bit",
          "[assembly][precision][serialization]") {
  AssemblyDocument asmDoc;
  Occurrence o;
  o.partId = 7;
  o.position = {1e3 + 0.1, -0.3, 24.7};
  o.orientation = kTilt;
  asmDoc.AddOccurrence(o); // id 1
  Occurrence rider;
  rider.partId = 7;
  rider.attachedTo = 1;
  rider.attachOffset = {0.1, 0.2, -13.4};
  rider.attachRotation = glm::angleAxis(0.3, glm::dvec3(0.0, 0.6, 0.8));
  asmDoc.AddOccurrence(rider); // id 2
  Mate m;
  m.type = MateType::Planar;
  m.a.occurrenceId = 1;
  m.a.anchor.origin = true;
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.rotation = 0.1 * 3;
  m.offset = 24.7;
  m.dx = -0.7;
  m.dy = 1e3 + 0.1;
  asmDoc.AddMate(m);

  const auto path = std::filesystem::temp_directory_path() /
                    "designer_model_precision_asm.json";
  REQUIRE(testio::SaveAssembly(asmDoc, path.string()));
  auto loaded = testio::LoadAssembly(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded != nullptr);
  const Occurrence* a = loaded->FindById(1);
  const Occurrence* b = loaded->FindById(2);
  REQUIRE((a && b));
  CHECK(a->position == o.position);
  CHECK(a->orientation == o.orientation);
  CHECK(b->attachOffset == rider.attachOffset);
  CHECK(b->attachRotation == rider.attachRotation);
  REQUIRE(loaded->GetMates().size() == 1);
  const Mate& lm = loaded->GetMates()[0];
  CHECK(lm.rotation == 0.1 * 3);
  CHECK(lm.offset == 24.7);
  CHECK(lm.dx == -0.7);
  CHECK(lm.dy == 1e3 + 0.1);
}

TEST_CASE("A mate solve does not move what it already solved",
          "[assembly][precision][solve]") {
  uint64_t ext = 0;
  auto pad = Pad(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return pad.get();
  };
  AssemblyDocument asmDoc = Mechanism(ext);
  REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
  const Occurrence first = *asmDoc.FindById(2);
  // The arm sits ON the base's cap, 13.4 up its axis: solved in double, the
  // seat is exact to rounding, not to float's 1e-4 mm at a metre.
  const glm::dmat4 baseM =
      glm::translate(glm::dmat4(1.0), asmDoc.FindById(1)->position) *
      glm::mat4_cast(asmDoc.FindById(1)->orientation);
  const glm::dvec3 cap = glm::dvec3(baseM * glm::dvec4(2.0, 2.0, 3.0, 1.0));
  const glm::dvec3 up = glm::mat3_cast(kTilt) * glm::dvec3(0.0, 0.0, 1.0);
  CHECK(glm::length(first.position - (cap + up * 13.4)) < 1e-9);
  // Solving again, and again, changes nothing: not a bit.
  for (int i = 0; i < 20; i++) {
    REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
    CHECK(asmDoc.FindById(2)->position == first.position);
    CHECK(asmDoc.FindById(2)->orientation == first.orientation);
  }
  // Nor does a save, a load and another solve.
  const auto path = std::filesystem::temp_directory_path() /
                    "designer_model_precision_solve.json";
  REQUIRE(testio::SaveAssembly(asmDoc, path.string()));
  auto loaded = testio::LoadAssembly(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded != nullptr);
  CHECK(loaded->FindById(2)->position == first.position);
  REQUIRE(assembly::Solve(*loaded, resolve).erroredMates == 0);
  CHECK(loaded->FindById(2)->position == first.position);
  CHECK(loaded->FindById(2)->orientation == first.orientation);
}
