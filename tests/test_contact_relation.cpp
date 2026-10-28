// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The contact relation (docs/assemblies/relations.md#contact): a follower
// that stands where it first touches its cam, solved from the two bodies'
// shapes. Checked against the closed forms of an eccentric circle — under a
// flat face, under a roller, and under a rocker's roller — at random angles
// BETWEEN the table's nodes; the pressure angle against the textbook's; a
// variable edit followed with one table rebuilt; and each refusal.

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/ContactFollower.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <map>
#include <random>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr double kR = 15.0, kE = 4.0; // the eccentric: radius, offset

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

Mate MateOf(MateType type, uint64_t a, uint64_t b) {
  Mate m;
  m.type = type;
  m.a = Origin(a);
  m.b = Origin(b);
  m.flipped = false;
  return m;
}

// A cam turning about world Z with a follower carried by a slider along
// world X (the guide's Z) or by a revolute about a pivot.
struct Rig {
  std::map<uint64_t, std::unique_ptr<Document>> parts;
  AssemblyDocument asmDoc;
  uint64_t frame = 0, guide = 0, cam = 0, follower = 0;
  uint64_t axle = 0, slide = 0, rel = 0;
  Document* camPart = nullptr;
  assembly::PartResolver Resolver() {
    return [this](uint64_t id, uint64_t) -> const Document* {
      auto it = parts.find(id);
      return it == parts.end() ? nullptr : it->second.get();
    };
  }
};

enum class Kind { Flat, Roller, Rocker };
constexpr double kRoller = 4.0;
const glm::dvec2 kPivot{25.0, 30.0};
constexpr double kArm = 30.0;

Rig MakeRig(Kind kind, int toward = -1) {
  Rig t;
  t.parts[1] = std::make_unique<Document>(); // the frame: nothing to touch
  auto cam = std::make_unique<Document>();
  cam->AddFeature(std::make_unique<CylinderFeature>(
      "Eccentric", glm::dvec3(kE, 0.0, 0.0), glm::dvec3(0, 0, 1), kR, 6.0));
  t.camPart = cam.get();
  t.parts[2] = std::move(cam);
  auto fol = std::make_unique<Document>();
  if (kind == Kind::Flat) {
    // In the guide's frame: local Z is the slide (world +X), local X is
    // world -Z. The flat face is local z = 0.
    fol->AddFeature(std::make_unique<BoxFeature>(
        "Foot", glm::dvec3(-15.0, -15.0, 0.0), 30.0, 30.0, 10.0));
  } else if (kind == Kind::Roller) {
    fol->AddFeature(std::make_unique<CylinderFeature>(
        "Roller", glm::dvec3(1.0, 0.0, kRoller), glm::dvec3(-1, 0, 0), kRoller,
        8.0));
  } else {
    fol->AddFeature(
        std::make_unique<CylinderFeature>("Roller", glm::dvec3(kArm, 0.0, -1.0),
                                          glm::dvec3(0, 0, 1), kRoller, 8.0));
  }
  t.parts[3] = std::move(fol);

  Occurrence f;
  f.name = "Frame";
  f.partId = 1;
  f.fixed = true;
  t.frame = t.asmDoc.AddOccurrence(std::move(f));
  Occurrence g;
  g.name = "Guide";
  g.partId = 1;
  g.fixed = true;
  if (kind == Kind::Rocker) {
    g.position = glm::dvec3(kPivot, 0.0);
  } else {
    // Z -> world X: a quarter turn about Y.
    g.orientation = glm::angleAxis(M_PI / 2, glm::dvec3(0, 1, 0));
  }
  t.guide = t.asmDoc.AddOccurrence(std::move(g));
  Occurrence c;
  c.name = "Cam";
  c.partId = 2;
  t.cam = t.asmDoc.AddOccurrence(std::move(c));
  Occurrence fo;
  fo.name = "Follower";
  fo.partId = 3;
  t.follower = t.asmDoc.AddOccurrence(std::move(fo));
  t.axle = t.asmDoc.AddMate(MateOf(MateType::Revolute, t.frame, t.cam));
  t.slide = t.asmDoc.AddMate(
      MateOf(kind == Kind::Rocker ? MateType::Revolute : MateType::Slider,
             t.guide, t.follower));
  Relation r;
  r.kind = RelationKind::Contact;
  r.mateA = t.axle;
  r.mateB = t.slide;
  r.dofA = MateDof::Rotation;
  r.dofB = kind == Kind::Rocker ? MateDof::Rotation : MateDof::Offset;
  r.toward = toward;
  t.rel = t.asmDoc.AddRelation(r);
  return t;
}

// The follower's DOF at cam angle `th` in closed form.
double Exact(Kind kind, double th, int toward = -1) {
  const glm::dvec2 E{kE * std::cos(th), kE * std::sin(th)};
  if (kind == Kind::Flat)
    return E.x + kR;
  const double Rp = kR + kRoller;
  if (kind == Kind::Roller)
    return E.x + std::sqrt(Rp * Rp - E.y * E.y) - kRoller;
  // The rocker: the roller's centre, kArm from the pivot, is Rp from E.
  const glm::dvec2 D = E - kPivot;
  const double d = glm::length(D), beta = std::atan2(D.y, D.x);
  const double c = (kArm * kArm + d * d - Rp * Rp) / (2 * kArm * d);
  const double start = -toward * M_PI; // reference 0, half a turn behind
  double best = 0, bestD = 1e9;
  for (double sgn : {1.0, -1.0}) {
    const double phi = beta + sgn * std::acos(c);
    double dd = std::fmod(toward * (phi - start), 2 * M_PI);
    if (dd < 0)
      dd += 2 * M_PI;
    if (dd < bestD) {
      bestD = dd;
      best = start + toward * dd;
    }
  }
  return best;
}

double Dof(const Rig& t, Kind kind) {
  const Mate* m = t.asmDoc.FindMateById(t.slide);
  return kind == Kind::Rocker ? m->rotation : m->offset;
}

} // namespace

TEST_CASE("contact: the follower matches the eccentric's closed form "
          "between the table's nodes",
          "[assembly][relations][contact]") {
  for (Kind kind : {Kind::Flat, Kind::Roller, Kind::Rocker}) {
    CAPTURE(int(kind));
    Rig t = MakeRig(kind);
    const auto parts = t.Resolver();
    assembly::Solve(t.asmDoc, parts);
    const Relation* r = t.asmDoc.FindRelationById(t.rel);
    REQUIRE(r->error.empty());
    REQUIRE(r->contactModel);
    const auto& m = *r->contactModel;
    const double tol = m.table.tolerance;
    INFO("nodes " << m.table.nodes.size() << " bound " << m.table.bound
                  << " tol " << tol);
    CHECK(m.table.drops.empty());
    CHECK(m.table.bound <= tol);
    std::mt19937 rng(1234 + int(kind));
    std::uniform_real_distribution<double> U(0.0, 2 * M_PI);
    double worst = 0;
    for (int k = 0; k < 1000; k++) {
      const double th = U(rng);
      assembly::SetMateDof(*t.asmDoc.FindMateById(t.axle), MateDof::Rotation,
                           th);
      assembly::PropagateRelations(t.asmDoc, t.axle, parts);
      worst = std::max(worst, std::abs(Dof(t, kind) - Exact(kind, th)));
    }
    INFO("worst " << worst);
    CHECK(worst <= tol);
  }
}

TEST_CASE("contact: the pressure angle is the textbook's",
          "[assembly][relations][contact]") {
  // A radial roller: tan(phi) = s' / (s + Rp), s + Rp the roller centre's
  // distance from the axis along its line, s' = dq/dtheta.
  Rig t = MakeRig(Kind::Roller);
  const auto parts = t.Resolver();
  assembly::Solve(t.asmDoc, parts);
  const auto& m = *t.asmDoc.FindRelationById(t.rel)->contactModel;
  std::mt19937 rng(9);
  std::uniform_real_distribution<double> U(0.0, 2 * M_PI);
  for (int k = 0; k < 200; k++) {
    const double th = U(rng);
    const contact::Touch touch =
        contact::FirstTouch(m.camOutline, m.camSense * th, m.followerOutline,
                            m.path, m.toward, 0.0, m.tolerance);
    REQUIRE(touch.found);
    double slope = 0;
    const double q = contact::TableValue(m.table, th, &slope);
    const double phi =
        std::acos(std::abs(glm::dot(touch.normal, touch.motion)));
    // The closed form: the normal runs through the roller's centre and the
    // eccentric's, so tan(phi) = e sin(t) / S, S = sqrt(Rp^2 - e^2 sin^2 t);
    // and the table's slope is the derivative of that centre's distance.
    const double Rp = kR + kRoller, es = kE * std::sin(th);
    const double S = std::sqrt(Rp * Rp - es * es);
    const double xc = kE * std::cos(th) + S;
    CHECK_THAT(phi, WithinAbs(std::atan(std::abs(es) / S), 1e-5));
    // The table's slope is its cubic's: within a few tenths of a percent
    // (the follower check differentiates exact solves instead).
    CHECK_THAT(slope, WithinAbs(-es * xc / S, 5e-3 * (1 + std::abs(slope))));
    // The roller is convex and so is the eccentric.
    CHECK_THAT(touch.camRadius, WithinAbs(kR, 1e-6));
    CHECK_THAT(touch.followerRadius, WithinAbs(kRoller, 1e-6));
  }
}

TEST_CASE("contact: an edit to the cam moves the follower, one table rebuilt",
          "[assembly][relations][contact]") {
  Rig t = MakeRig(Kind::Flat);
  const auto parts = t.Resolver();
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axle), MateDof::Rotation, 1.0);
  assembly::Solve(t.asmDoc, parts);
  const double before = Dof(t, Kind::Flat);
  const auto builds = contact::Stats().builds;
  // Solving again builds nothing.
  assembly::Solve(t.asmDoc, parts);
  CHECK(contact::Stats().builds == builds);
  auto* cyl = dynamic_cast<CylinderFeature*>(t.camPart->GetFeature(0));
  cyl->radius = kR + 2.0;
  t.camPart->Regenerate();
  assembly::Solve(t.asmDoc, parts);
  CHECK(contact::Stats().builds == builds + 1);
  const auto& mm = *t.asmDoc.FindRelationById(t.rel)->contactModel;
  INFO(std::setprecision(10)
       << "before " << before << " exact " << Exact(Kind::Flat, 1.0)
       << " after " << Dof(t, Kind::Flat) << " nodes " << mm.table.nodes.size()
       << " bound " << mm.table.bound << " tol " << mm.tolerance << " camPts "
       << mm.camOutline.pts.size());
  CHECK_THAT(Dof(t, Kind::Flat) - before, WithinAbs(2.0, 2 * mm.tolerance));
}

TEST_CASE("contact: a follower a loop's solve leaves a micrometre off "
          "hits the cache",
          "[assembly][relations][contact]") {
  // A follower closed into a mate loop is placed by the loop's solve, which
  // leaves it some 1e-5 mm from where the last solve did, and the frames
  // the key is made of carry rounding: an exact key built the table again
  // at samples of a study, half a minute a time.
  Rig t = MakeRig(Kind::Roller);
  const auto parts = t.Resolver();
  assembly::Solve(t.asmDoc, parts);
  const Relation& r = *t.asmDoc.FindRelationById(t.rel);
  std::string why;
  REQUIRE(contact::ModelFor(t.asmDoc, r, parts, &why));
  const auto builds = contact::Stats().builds;
  // The guide where the solve leaves it: a micrometre and the sign of a
  // zero's rounding off where it was (the exact key printed both).
  Occurrence* guide = t.asmDoc.FindById(t.guide);
  const glm::dvec3 at = guide->position;
  guide->position = at + glm::dvec3(1e-5, -1e-17, 1e-5);
  REQUIRE(contact::ModelFor(t.asmDoc, r, parts, &why));
  CHECK(contact::Stats().builds == builds);
  // A guide moved by a tenth of a millimetre is another contact.
  guide->position = at + glm::dvec3(0.1, 0.0, 0.1);
  REQUIRE(contact::ModelFor(t.asmDoc, r, parts, &why));
  CHECK(contact::Stats().builds == builds + 1);
}

TEST_CASE("contact: refusals say what a contact relation takes",
          "[assembly][relations][contact]") {
  SECTION("a follower sliding along the cam's axis") {
    Rig t = MakeRig(Kind::Flat);
    t.asmDoc.FindById(t.guide)->orientation = glm::dquat(1, 0, 0, 0);
    assembly::Solve(t.asmDoc, t.Resolver());
    CHECK_THAT(t.asmDoc.FindRelationById(t.rel)->error,
               ContainsSubstring("disc cam"));
  }
  SECTION("a horizontal slide cannot take its sense from gravity") {
    Rig t = MakeRig(Kind::Flat);
    std::string why;
    CHECK(contact::TowardFromGravity(t.asmDoc,
                                     *t.asmDoc.FindRelationById(t.rel),
                                     t.Resolver(), &why) == 0);
    CHECK_THAT(why, ContainsSubstring("`toward`"));
  }
  SECTION("a vertical slide takes it: the follower falls onto the cam") {
    Rig t = MakeRig(Kind::Flat);
    // The cam's axis along world X, the slide straight up.
    t.asmDoc.FindById(t.frame)->orientation =
        glm::angleAxis(M_PI / 2, glm::dvec3(0, 1, 0));
    t.asmDoc.FindById(t.guide)->orientation = glm::dquat(1, 0, 0, 0);
    std::string why;
    CHECK(contact::TowardFromGravity(t.asmDoc,
                                     *t.asmDoc.FindRelationById(t.rel),
                                     t.Resolver(), &why) == -1);
    INFO(why);
  }
  SECTION("a follower that never touches") {
    Rig t = MakeRig(Kind::Flat);
    t.asmDoc.FindById(t.guide)->position = glm::dvec3(0, 60, 0);
    assembly::Solve(t.asmDoc, t.Resolver());
    CHECK_THAT(t.asmDoc.FindRelationById(t.rel)->error,
               ContainsSubstring("never touches"));
  }
  SECTION("bases that move relative to each other") {
    Rig t = MakeRig(Kind::Flat);
    t.asmDoc.FindById(t.guide)->fixed = false;
    t.asmDoc.AddMate(MateOf(MateType::Revolute, t.frame, t.guide));
    assembly::Solve(t.asmDoc, t.Resolver());
    CHECK_THAT(t.asmDoc.FindRelationById(t.rel)->error,
               ContainsSubstring("must not move relative to each other"));
  }
  SECTION("a follower beside the cam, not across it") {
    Rig t = MakeRig(Kind::Flat);
    t.asmDoc.FindById(t.frame)->position = glm::dvec3(0, 0, 40);
    assembly::Solve(t.asmDoc, t.Resolver());
    CHECK_THAT(t.asmDoc.FindRelationById(t.rel)->error,
               ContainsSubstring("does not overlap the cam along its axis"));
  }
}

TEST_CASE("contact: the relation round-trips and loads from its golden",
          "[assembly][relations][contact][golden]") {
  Rig t = MakeRig(Kind::Roller);
  const json j = DocumentIO::RelationToJson(*t.asmDoc.FindRelationById(t.rel));
  CHECK(j.at("kind") == "contact");
  CHECK(j.at("toward") == "negative");
  CHECK_FALSE(j.contains("cam"));
  const Relation back = DocumentIO::RelationFromJson(j);
  CHECK(back.kind == RelationKind::Contact);
  CHECK(back.toward == -1);
  json bad = j;
  bad["toward"] = "down";
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(bad),
                    ContainsSubstring("positive or negative"));
  json noToward = j;
  noToward.erase("toward");
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(noToward),
                    ContainsSubstring("needs `toward`"));

  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());
  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v197-contact-follower.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 1);
  const Relation& gr = (*g)->GetRelations()[0];
  CHECK(gr.kind == RelationKind::Contact);
  // ...and it drives: at a quarter turn the roller stands where the closed
  // form puts it, solved from the parts' shapes.
  assembly::SetMateDof(*(*g)->FindMateById(t.axle), MateDof::Rotation,
                       M_PI / 2);
  assembly::Solve(**g, t.Resolver());
  CHECK(gr.error.empty());
  CHECK_THAT((*g)->FindMateById(t.slide)->offset,
             WithinAbs(Exact(Kind::Roller, M_PI / 2), 1e-4));
}
