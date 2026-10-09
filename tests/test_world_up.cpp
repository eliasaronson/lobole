// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The world-up gate (docs/conventions.md#world-axes).
//
// Four subsystems independently got the up axis wrong (simulation gravity,
// every named drawing view, CAM's spindle, the cutting simulation) because
// the app's choice lived in scattered literals and duplicated tables. The
// choice now lives in core/Conventions.h, and this test pins every consumer
// to it: the named-view table, the drawings' OrientationAxes, the base-plane
// names, the gravity and load defaults, the measure tool's "top"/"bottom"
// words, the default camera.
//
// Unlike test_conventions.cpp this is NOT a source scan: it calls the real
// functions and compares real values, so a hand-edited copy of a table fails
// here even if it greps clean.
//
#include <catch2/catch_test_macros.hpp>

#include "cad/SketchPlane.h"
#include "core/Conventions.h"
#include "core/Measure.h"
#include "core/cam/CamToolpath.h"
#include "core/drawing/DrawingDocument.h"
#include "core/fem/FemStudy.h"
#include "render/Camera.h"

#include <glm/glm.hpp>

#include <cstring>
#include <map>
#include <string>

namespace {

bool Eq(const glm::dvec3& a, const glm::dvec3& b) {
  return glm::length(a - b) < 1e-12;
}

} // namespace

TEST_CASE("the world basis is orthonormal and right-handed", "[world_up]") {
  using namespace conv;
  CHECK(std::abs(glm::length(kWorldUp) - 1.0) < 1e-12);
  CHECK(std::abs(glm::length(kWorldFront) - 1.0) < 1e-12);
  CHECK(std::abs(glm::length(kWorldRight) - 1.0) < 1e-12);
  CHECK(std::abs(glm::dot(kWorldUp, kWorldFront)) < 1e-12);
  CHECK(std::abs(glm::dot(kWorldUp, kWorldRight)) < 1e-12);
  CHECK(std::abs(glm::dot(kWorldFront, kWorldRight)) < 1e-12);
  // right x up = front: flipping kWorldUp without re-deriving kWorldFront
  // fails here, which is the point — the basis moves as one.
  CHECK(Eq(glm::cross(kWorldRight, kWorldUp), kWorldFront));
}

TEST_CASE("the named views derive from the world basis", "[world_up]") {
  using namespace conv;
  // Every view is a valid frame: unit vectors, and the up hint never
  // collinear with the view axis (iso's up is world up, deliberately not
  // perpendicular — the camera orthonormalizes the roll).
  for (const NamedView& v : kNamedViews) {
    INFO(v.name);
    CHECK(std::abs(glm::length(v.toCamera) - 1.0) < 1e-12);
    CHECK(std::abs(glm::length(v.up) - 1.0) < 1e-12);
    CHECK(std::abs(glm::dot(v.toCamera, v.up)) < 0.99);
  }
  // And each one is the view its name claims, in the world frame. A hand
  // edit of the table that survives the checks above still fails here.
  CHECK(Eq(FindView("front")->toCamera, kWorldFront));
  CHECK(Eq(FindView("back")->toCamera, -kWorldFront));
  CHECK(Eq(FindView("right")->toCamera, kWorldRight));
  CHECK(Eq(FindView("left")->toCamera, -kWorldRight));
  CHECK(Eq(FindView("top")->toCamera, kWorldUp));
  CHECK(Eq(FindView("bottom")->toCamera, -kWorldUp));
  CHECK(Eq(FindView("iso")->toCamera,
           glm::normalize(kWorldRight + kWorldUp + kWorldFront)));
  // The four side views and iso stand upright.
  for (const char* n : {"front", "back", "right", "left", "iso"})
    CHECK(Eq(FindView(n)->up, kWorldUp));
  // The plan-view fold: looking down, the front edge sits at the bottom of
  // the screen/sheet, so a drawing's top view folds up from its front view.
  CHECK(Eq(FindView("top")->up, -kWorldFront));
  CHECK(Eq(FindView("bottom")->up, kWorldFront));
}

TEST_CASE("drawings' OrientationAxes IS the named-view table", "[world_up]") {
  // The drawings bug behind docs/conventions.md#world-axes: a second,
  // hand-written copy of this table shipped every named view wrong. There is no
  // second copy now, and this proves it stays that way.
  const ViewOrientation all[] = {
      ViewOrientation::Front, ViewOrientation::Back, ViewOrientation::Left,
      ViewOrientation::Right, ViewOrientation::Top,  ViewOrientation::Bottom,
      ViewOrientation::Iso,
  };
  for (ViewOrientation o : all) {
    const char* name = ViewOrientationToString(o);
    INFO(name);
    const conv::NamedView* v = conv::FindView(name);
    REQUIRE(v != nullptr); // every orientation spelling is a named view
    glm::dvec3 viewDir(0.0), up(0.0);
    OrientationAxes(o, viewDir, up);
    CHECK(Eq(viewDir, -v->toCamera)); // the viewer looks AT the part
    CHECK(Eq(up, v->up));
  }
}

TEST_CASE("base-plane names follow the world frame", "[world_up]") {
  // The plane a person calls "Top" is the one whose normal is world up —
  // whichever coordinate plane that happens to be.
  std::map<std::string, int> seen;
  for (PlaneType t : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ}) {
    const SketchPlane p(t);
    const glm::dvec3 n = glm::dvec3(p.GetNormal());
    INFO(p.GetName());
    if (std::abs(glm::dot(n, conv::kWorldUp)) > 0.9)
      CHECK(p.GetName() == "Top Plane");
    else if (std::abs(glm::dot(n, conv::kWorldFront)) > 0.9)
      CHECK(p.GetName() == "Front Plane");
    else
      CHECK(p.GetName() == "Right Plane");
    // The ctor name and the shared helper agree (FeatureTreePanel lists
    // planes through the helper).
    CHECK(p.GetName() == SketchPlane::StandardPlaneName(t));
    seen[p.GetName()]++;
  }
  // All three names exist, once each.
  CHECK(seen.size() == 3);
}

TEST_CASE("gravity and the default load point down", "[world_up]") {
  CHECK(Eq(conv::kGravity, -9.81 * conv::kWorldUp));
  CHECK(Eq(fem::Load{}.direction, -conv::kWorldUp));
}

TEST_CASE("the measure tool's facing words follow world up", "[world_up]") {
  using Measure::FacingWord;
  REQUIRE(FacingWord(conv::kWorldUp) != nullptr);
  CHECK(std::strcmp(FacingWord(conv::kWorldUp), "top") == 0);
  REQUIRE(FacingWord(-conv::kWorldUp) != nullptr);
  CHECK(std::strcmp(FacingWord(-conv::kWorldUp), "bottom") == 0);
  REQUIRE(FacingWord(conv::kWorldFront) != nullptr);
  CHECK(std::strcmp(FacingWord(conv::kWorldFront), "side") == 0);
  REQUIRE(FacingWord(conv::kWorldRight) != nullptr);
  CHECK(std::strcmp(FacingWord(conv::kWorldRight), "side") == 0);
  // A diagonal has no word.
  CHECK(FacingWord(glm::normalize(glm::dvec3(1, 1, 1))) == nullptr);
}

TEST_CASE("CAM's setup frame agrees with the world frame", "[world_up]") {
  // Setup space is Z-up-spindle by machine-tool convention (ISO 841). For a
  // plain 3-axis job (rotationA = 0) it must be the IDENTITY on the world:
  // this disagreement — spindle +Z in a world that called +Y up — was the
  // fourth axis bug and one docs/conventions.md#world-axes rules out. If this
  // fails, either the world flipped without CAM's knowledge or someone added a
  // compensating rotation that must not exist.
  cam::Setup setup;
  const glm::dmat3 m = cam::SetupToModel(setup);
  CHECK(Eq(m * glm::dvec3(0, 0, 1), conv::kWorldUp));
  CHECK(Eq(m * glm::dvec3(1, 0, 0), conv::kWorldRight));
}

TEST_CASE("the default camera stands upright above the floor", "[world_up]") {
  // The fresh-session iso view: camera above the ground plane, screen-up
  // agreeing with world up.
  CHECK(glm::dot(Camera::DefaultIsometricPosition(), conv::kWorldUpF) > 0.0f);
  Camera cam;
  CHECK(glm::dot(cam.GetUp(), conv::kWorldUpF) > 0.5f);
}
