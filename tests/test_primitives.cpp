// The primitive tools (docs/modeling/README.md#primitives): a Box, Cylinder
// or Sphere asked for in one call is stored as a fully constrained Sketch on
// what it names, plus an Extrude or a Revolve, and follows that reference.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Primitives.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchSolver.h"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using json = nlohmann::json;

namespace {

constexpr double kPi = glm::pi<double>();

// A 60 x 40 x 10 plate from the internal fixture (the stored Box, which a
// test may still build), min corner at the origin. Returns its id.
uint64_t AddPlate(Document& doc, double thickness = 10.0) {
  doc.AddFeature(std::make_unique<BoxFeature>("Plate", glm::dvec3(0.0), 60.0,
                                              40.0, thickness));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The name of the face of `featureId`'s body whose outward normal is `n`.
std::string FaceFacing(const Document& doc, uint64_t featureId, glm::dvec3 n) {
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy != featureId)
      continue;
    for (const auto& [idx, name] : b.elementMap.names) {
      glm::dvec3 c, normal, u;
      if (b.shape.FacePlane(idx, c, normal, u) && glm::dot(normal, n) > 0.999)
        return name;
    }
  }
  return {};
}

const Body* BodyOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      return &b;
  return nullptr;
}

primitive::Added AddJson(Document& doc, CommandStack& commands, const json& j) {
  return primitive::Add(doc, commands, primitive::SpecFromJson(j));
}

// The sketch the primitive stored is exactly determined: no freedom left,
// nothing redundant or conflicting.
void RequireFullyConstrained(const Document& doc, uint64_t sketchId) {
  const auto* sk =
      dynamic_cast<const SketchFeature*>(doc.FindFeatureById(sketchId));
  REQUIRE(sk != nullptr);
  CHECK(sk->GetError().empty());
  sketch::Solver solver;
  solver.Load(*sk);
  const sketch::SolveResult r = solver.Solve();
  CHECK(r.ok);
  CHECK(r.dof == 0);
  CHECK(r.redundant.empty());
  CHECK(r.conflicting.empty());
}

double VolumeOf(const Document& doc, uint64_t id) {
  const Body* b = BodyOf(doc, id);
  REQUIRE(b != nullptr);
  return b->shape.Volume();
}

void BoundsOf(const Document& doc, uint64_t id, glm::dvec3& lo,
              glm::dvec3& hi) {
  const Body* b = BodyOf(doc, id);
  REQUIRE(b != nullptr);
  b->shape.GetBoundingBox(lo, hi, true);
}

} // namespace

TEST_CASE("each primitive on each kind of reference is a DOF-0 sketch and "
          "its solid",
          "[primitives]") {
  Document doc;
  CommandStack commands;
  const uint64_t plate = AddPlate(doc);
  const std::string top = FaceFacing(doc, plate, {0, 0, 1});
  REQUIRE(!top.empty());

  auto datum =
      std::make_unique<DatumPlaneFeature>("Datum", DatumPlaneKind::Offset);
  datum->offset = 30.0;
  doc.AddFeature(std::move(datum));
  const uint64_t datumId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  auto frame = std::make_unique<FrameFeature>("Seat");
  frame->origin[0] = 100.0;
  frame->zAxis = {1.0, 0.0, 0.0};
  frame->xAxis = {0.0, 1.0, 0.0};
  doc.AddFeature(std::move(frame));
  const uint64_t frameId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  auto host = std::make_unique<SketchFeature>(
      "Host", SketchPlane(PlaneType::YZ, glm::dvec3(-50.0, 0.0, 0.0)));
  doc.AddFeature(std::move(host));
  const uint64_t hostId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  const json ons[] = {
      "Top",
      {{"feature_id", plate}, {"element", top}},
      {{"feature_id", datumId}},
      {{"feature_id", hostId}},
      {{"feature_id", frameId}},
  };
  for (const json& on : ons) {
    INFO("on " << on.dump());
    const auto box = AddJson(doc, commands,
                             {{"type", "Box"},
                              {"on", on},
                              {"width", 8.0},
                              {"depth", 6.0},
                              {"height", 5.0}});
    RequireFullyConstrained(doc, box.sketchId);
    CHECK_THAT(VolumeOf(doc, box.solidId), WithinRel(8.0 * 6.0 * 5.0, 1e-9));

    const auto cyl = AddJson(
        doc, commands,
        {{"type", "Cylinder"}, {"on", on}, {"diameter", 6.0}, {"height", 4.0}});
    RequireFullyConstrained(doc, cyl.sketchId);
    CHECK_THAT(VolumeOf(doc, cyl.solidId), WithinRel(kPi * 9.0 * 4.0, 1e-6));

    const auto ball = AddJson(
        doc, commands, {{"type", "Sphere"}, {"on", on}, {"radius", 3.0}});
    RequireFullyConstrained(doc, ball.sketchId);
    CHECK_THAT(VolumeOf(doc, ball.solidId),
               WithinRel(4.0 / 3.0 * kPi * 27.0, 1e-6));
  }
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    INFO(doc.GetFeature(i)->GetName());
    CHECK(doc.GetFeature(i)->GetError().empty());
  }
}

TEST_CASE("a primitive is placed in the frame of what it is on",
          "[primitives]") {
  Document doc;
  CommandStack commands;
  // On Top: u is +X, v is +Y, the normal +Z; the offset is the centre.
  const auto a = AddJson(doc, commands,
                         {{"type", "Box"},
                          {"on", "Top"},
                          {"width", 10.0},
                          {"depth", 4.0},
                          {"height", 2.0},
                          {"offset", {20.0, 5.0}}});
  glm::dvec3 lo, hi;
  BoundsOf(doc, a.solidId, lo, hi);
  CHECK_THAT(lo.x, WithinAbs(15.0, 1e-7));
  CHECK_THAT(hi.x, WithinAbs(25.0, 1e-7));
  CHECK_THAT(lo.y, WithinAbs(3.0, 1e-7));
  CHECK_THAT(lo.z, WithinAbs(0.0, 1e-7));
  CHECK_THAT(hi.z, WithinAbs(2.0, 1e-7));

  // centered: false puts the low corner at the offset.
  const auto b = AddJson(doc, commands,
                         {{"type", "Box"},
                          {"on", "Top"},
                          {"width", 10.0},
                          {"depth", 4.0},
                          {"height", 2.0},
                          {"centered", false},
                          {"offset", {-30.0, -7.0}}});
  BoundsOf(doc, b.solidId, lo, hi);
  CHECK_THAT(lo.x, WithinAbs(-30.0, 1e-7));
  CHECK_THAT(lo.y, WithinAbs(-7.0, 1e-7));
  CHECK_THAT(hi.y, WithinAbs(-3.0, 1e-7));

  // A sphere's centre lies on the plane; a cylinder rises along the normal,
  // or against it reversed.
  const auto s = AddJson(doc, commands,
                         {{"type", "Sphere"},
                          {"on", "Right"},
                          {"diameter", 4.0},
                          {"offset", {7.0, 9.0}}});
  BoundsOf(doc, s.solidId, lo, hi);
  CHECK_THAT((lo.x + hi.x) / 2.0, WithinAbs(0.0, 1e-6));
  CHECK_THAT((lo.y + hi.y) / 2.0, WithinAbs(7.0, 1e-6));
  CHECK_THAT((lo.z + hi.z) / 2.0, WithinAbs(9.0, 1e-6));
  const auto c = AddJson(doc, commands,
                         {{"type", "Cylinder"},
                          {"on", "Top"},
                          {"diameter", 2.0},
                          {"height", 6.0},
                          {"reversed", true}});
  BoundsOf(doc, c.solidId, lo, hi);
  CHECK_THAT(lo.z, WithinAbs(-6.0, 1e-7));
  CHECK_THAT(hi.z, WithinAbs(0.0, 1e-7));
}

TEST_CASE("a primitive on a face follows the face", "[primitives]") {
  Document doc;
  CommandStack commands;
  const uint64_t plate = AddPlate(doc, 10.0);
  const std::string top = FaceFacing(doc, plate, {0, 0, 1});
  const auto boss = AddJson(doc, commands,
                            {{"type", "Cylinder"},
                             {"on", {{"feature_id", plate}, {"element", top}}},
                             {"diameter", 10.0},
                             {"height", 5.0}});
  glm::dvec3 lo, hi;
  BoundsOf(doc, boss.solidId, lo, hi);
  // Centred on the face: its centroid is the frame's origin.
  CHECK_THAT((lo.x + hi.x) / 2.0, WithinAbs(30.0, 1e-6));
  CHECK_THAT((lo.y + hi.y) / 2.0, WithinAbs(20.0, 1e-6));
  CHECK_THAT(lo.z, WithinAbs(10.0, 1e-7));

  // A thicker plate: the boss rides up with its face.
  auto* box = dynamic_cast<BoxFeature*>(doc.FindFeatureById(plate));
  REQUIRE(box != nullptr);
  box->depth = 24.7;
  doc.Regenerate();
  BoundsOf(doc, boss.solidId, lo, hi);
  CHECK_THAT(lo.z, WithinAbs(24.7, 1e-9));
  CHECK_THAT(hi.z, WithinAbs(29.7, 1e-9));
}

TEST_CASE("a lost reference is an error, never the world plane",
          "[primitives]") {
  Document doc;
  CommandStack commands;
  const uint64_t plate = AddPlate(doc);
  auto datum =
      std::make_unique<DatumPlaneFeature>("Datum", DatumPlaneKind::Offset);
  datum->offset = 40.0;
  doc.AddFeature(std::move(datum));
  const uint64_t datumId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  const auto box = AddJson(doc, commands,
                           {{"type", "Box"},
                            {"on", {{"feature_id", datumId}}},
                            {"width", 5.0},
                            {"depth", 5.0},
                            {"height", 5.0}});
  const double plateVolume = VolumeOf(doc, plate);
  REQUIRE(doc.GetBodies().size() == 2);

  size_t index = 0;
  REQUIRE(doc.FindFeatureById(datumId, &index) != nullptr);
  REQUIRE(commands.Execute(std::make_unique<RemoveFeatureCommand>(index), doc));
  const Feature* sk = doc.FindFeatureById(box.sketchId);
  REQUIRE(sk != nullptr);
  CHECK_THAT(sk->GetError(), ContainsSubstring("gone"));
  CHECK(!doc.FindFeatureById(box.solidId)->GetError().empty());
  // The plate is untouched and the box is NOT built at the world origin.
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(VolumeOf(doc, plate), WithinRel(plateVolume, 1e-12));

  // A face that no longer exists is the same: the sketch says so.
  Document doc2;
  CommandStack commands2;
  const uint64_t plate2 = AddPlate(doc2);
  const auto pin =
      AddJson(doc2, commands2,
              {{"type", "Cylinder"},
               {"on",
                {{"feature_id", plate2},
                 {"element", FaceFacing(doc2, plate2, {0, 0, 1})}}},
               {"diameter", 4.0},
               {"height", 4.0}});
  doc2.GetFeature(0)->SetSuppressed(true);
  doc2.Regenerate();
  CHECK(!doc2.FindFeatureById(pin.sketchId)->GetError().empty());
  CHECK(doc2.GetBodies().empty());
}

TEST_CASE("one undo removes every feature a primitive made", "[primitives]") {
  Document doc;
  CommandStack commands;
  AddPlate(doc);
  const size_t before = doc.FeatureCount();
  AddJson(doc, commands,
          {{"type", "Sphere"}, {"on", "Front"}, {"radius", 2.0}});
  CHECK(doc.FeatureCount() == before + 2);
  REQUIRE(commands.CanUndo());
  commands.Undo(doc);
  CHECK(doc.FeatureCount() == before);
  CHECK(!commands.CanUndo());
  commands.Redo(doc);
  CHECK(doc.FeatureCount() == before + 2);
}

TEST_CASE("primitive sizes and offsets take expressions", "[primitives]") {
  Document doc;
  CommandStack commands;
  doc.SetParameter("w", "12");
  doc.Regenerate();
  const auto box = AddJson(doc, commands,
                           {{"type", "Box"},
                            {"on", "Top"},
                            {"width", 1.0},
                            {"width_expr", "w"},
                            {"depth", 3.0},
                            {"height", 1.0},
                            {"height_expr", "w / 4"},
                            {"offset", {0.0, 0.0}},
                            {"offset_expr", {"w", ""}}});
  CHECK_THAT(VolumeOf(doc, box.solidId), WithinRel(12.0 * 3.0 * 3.0, 1e-9));
  RequireFullyConstrained(doc, box.sketchId);
  doc.SetParameter("w", "20");
  doc.Regenerate();
  CHECK_THAT(VolumeOf(doc, box.solidId), WithinRel(20.0 * 3.0 * 5.0, 1e-9));
  glm::dvec3 lo, hi;
  BoundsOf(doc, box.solidId, lo, hi);
  CHECK_THAT((lo.x + hi.x) / 2.0, WithinAbs(20.0, 1e-7));
}

TEST_CASE("primitive defaults are sized from what it is on", "[primitives]") {
  Document doc;
  CommandStack commands;
  const uint64_t plate = AddPlate(doc); // 60 x 40 top face
  const auto box =
      AddJson(doc, commands,
              {{"type", "Box"},
               {"on",
                {{"feature_id", plate},
                 {"element", FaceFacing(doc, plate, {0, 0, 1})}}}});
  // Half the face's smaller span, every side.
  CHECK_THAT(VolumeOf(doc, box.solidId), WithinRel(20.0 * 20.0 * 20.0, 1e-9));

  // On a plane in an empty part there is nothing to size from: refused,
  // asking for the sizes.
  Document empty;
  CommandStack c2;
  CHECK_THROWS_WITH(AddJson(empty, c2, {{"type", "Cylinder"}, {"on", "Top"}}),
                    ContainsSubstring("diameter, height"));
  CHECK(empty.FeatureCount() == 0);
}

TEST_CASE("a primitive without a reference is refused, naming what is "
          "accepted",
          "[primitives]") {
  Document doc;
  CommandStack commands;
  const uint64_t plate = AddPlate(doc);
  const auto refused = [&](const json& j, const std::string& says) {
    INFO(j.dump());
    CHECK_THROWS_WITH(AddJson(doc, commands, j), ContainsSubstring(says));
  };
  refused({{"type", "Box"}, {"width", 1.0}}, "needs `on`");
  refused({{"type", "Box"}, {"on", "Bottom"}}, "\"Top\", \"Front\"");
  refused({{"type", "Box"}, {"on", {{"plane", "xy"}}}}, "{feature_id}");
  refused({{"type", "Box"}, {"on", "Top"}, {"position", {0, 0, 0}}},
          "no longer takes `position`");
  refused({{"type", "Cylinder"}, {"on", "Top"}, {"center", {0, 0, 0}}},
          "no longer takes `center`");
  refused({{"type", "Sphere"}, {"on", "Top"}, {"radius", 1.0}, {"height", 2.0}},
          "unknown field 'height'");
  refused(
      {{"type", "Sphere"}, {"on", "Top"}, {"radius", 1.0}, {"diameter", 2.0}},
      "not both");
  refused({{"type", "Box"}, {"on", "Top"}, {"mode", "fuse"}},
          "new_body, add, cut");
  refused({{"type", "Box"}, {"on", {{"feature_id", plate}}}}, "is not a plane");
  refused(
      {{"type", "Box"}, {"on", {{"feature_id", plate}, {"element", "nope"}}}},
      "has no face named");
  refused({{"type", "Box"},
           {"on", "Top"},
           {"width", -1.0},
           {"depth", 1.0},
           {"height", 1.0}},
          "must be positive");
  // Nothing refused was added.
  CHECK(doc.FeatureCount() == 1);

  // A round face is not a plane: the refusal points at a Frame.
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Shaft", glm::dvec3(100.0, 0.0, 0.0), glm::dvec3(0, 0, 1), 5.0, 20.0));
  const uint64_t shaft = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  std::string round;
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == shaft)
      for (const auto& [idx, name] : b.elementMap.names) {
        glm::dvec3 c, n, u;
        if (!b.shape.FacePlane(idx, c, n, u))
          round = name;
      }
  REQUIRE(!round.empty());
  refused(
      {{"type", "Box"}, {"on", {{"feature_id", shaft}, {"element", round}}}},
      "Frame");
}
