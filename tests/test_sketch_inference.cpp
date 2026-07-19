#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/sketch/SketchInference.h"

using Catch::Matchers::WithinAbs;

// pixelsPerUV = 10 → a 1-UV distance is 10 px, so the 10 px point tolerance is
// 1 UV and the 8 px edge tolerance is 0.8 UV. Keeps the numbers easy.
static constexpr float kScale = 10.0f;

TEST_CASE("Infer snaps to a nearby vertex (coincident)", "[inference]") {
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 1}};
  // Cursor 0.3 UV (3 px) from the far endpoint (4,0) → within the 1-UV tol.
  sketch::Snap s = sketch::InferPoint(e, {4.2f, 0.2f}, std::nullopt, 0, kScale);
  REQUIRE(s.coincident.has_value());
  CHECK(s.coincident->entity == 1u);
  CHECK(s.coincident->which == 1);
  CHECK_THAT(s.uv.x, WithinAbs(4.0f, 1e-5f)); // snapped onto the vertex
  CHECK_THAT(s.uv.y, WithinAbs(0.0f, 1e-5f));
  CHECK_FALSE(s.pointOnLine.has_value());
}

TEST_CASE("Infer snaps onto a line body (point-on-line)", "[inference]") {
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 1}};
  // Near the middle of the segment, 0.3 UV above it (far from either vertex).
  sketch::Snap s = sketch::InferPoint(e, {2.0f, 0.3f}, std::nullopt, 0, kScale);
  CHECK_FALSE(s.coincident.has_value());
  REQUIRE(s.pointOnLine.has_value());
  CHECK(*s.pointOnLine == 1u);
  CHECK_THAT(s.uv.y, WithinAbs(0.0f, 1e-5f)); // dropped onto the line
}

TEST_CASE("Infer detects horizontal and vertical segments", "[inference]") {
  std::vector<sketch::Entity> e; // empty — no vertices to snap to
  // Drawn away from the origin/axes so datum snapping doesn't claim an axis.
  // Near-horizontal: from (2,2) to (7, 2.1) → within 2 deg.
  sketch::Snap h = sketch::InferPoint(e, {7.0f, 2.1f}, glm::vec2{2, 2}, 0, kScale);
  CHECK(h.horizontal);
  CHECK_FALSE(h.vertical);
  CHECK_THAT(h.uv.y, WithinAbs(2.0f, 1e-5f)); // snapped flat to the start

  sketch::Snap v = sketch::InferPoint(e, {2.1f, 7.0f}, glm::vec2{2, 2}, 0, kScale);
  CHECK(v.vertical);
  CHECK_THAT(v.uv.x, WithinAbs(2.0f, 1e-5f));

  // A clear 30 deg segment is neither.
  sketch::Snap d = sketch::InferPoint(e, {7.0f, 4.9f}, glm::vec2{2, 2}, 0, kScale);
  CHECK_FALSE(d.horizontal);
  CHECK_FALSE(d.vertical);
}

TEST_CASE("Infer detects a parallel segment", "[inference]") {
  // An existing line at ~45 degrees.
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {2, 2}, 1}};
  // Drawing another near-45-degree segment elsewhere → parallel to id 1.
  sketch::Snap s =
      sketch::InferPoint(e, {5.0f, 5.05f}, glm::vec2{3, 3}, 0, kScale);
  REQUIRE(s.parallelTo.has_value());
  CHECK(*s.parallelTo == 1u);
  CHECK_FALSE(s.perpendicularTo.has_value());
}

TEST_CASE("Infer excludes the entity being drawn", "[inference]") {
  // The only vertex belongs to skipId → no coincident snap.
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 7}};
  sketch::Snap s = sketch::InferPoint(e, {4.05f, 0.05f}, std::nullopt, 7, kScale);
  CHECK_FALSE(s.coincident.has_value());
  CHECK_FALSE(s.pointOnLine.has_value());
}

TEST_CASE("Infer aligns a point to another vertex's X (vertical align)",
          "[inference]") {
  // A lone point at (3,5); cursor shares its X (0.05 UV off) but is far in Y.
  std::vector<sketch::Entity> e = {sketch::Point{{3.0f, 5.0f}, 1}};
  sketch::Snap s = sketch::InferPoint(e, {3.05f, -2.0f}, std::nullopt, 0, kScale);
  CHECK_FALSE(s.coincident.has_value());
  REQUIRE(s.alignShareX.has_value());
  CHECK(s.alignShareX->entity == 1u);
  CHECK_FALSE(s.alignShareY.has_value());
  CHECK_THAT(s.uv.x, WithinAbs(3.0f, 1e-5f));  // X snapped to the vertex
  CHECK_THAT(s.uv.y, WithinAbs(-2.0f, 1e-5f)); // Y left alone
}

TEST_CASE("Infer aligns a point to another vertex's Y (horizontal align)",
          "[inference]") {
  std::vector<sketch::Entity> e = {sketch::Point{{3.0f, 5.0f}, 1}};
  sketch::Snap s = sketch::InferPoint(e, {-2.0f, 5.05f}, std::nullopt, 0, kScale);
  REQUIRE(s.alignShareY.has_value());
  CHECK(s.alignShareY->entity == 1u);
  CHECK_FALSE(s.alignShareX.has_value());
  CHECK_THAT(s.uv.y, WithinAbs(5.0f, 1e-5f));
  CHECK_THAT(s.uv.x, WithinAbs(-2.0f, 1e-5f));
}

TEST_CASE("Infer aligns both axes to different vertices (rectangle corner)",
          "[inference]") {
  // Corner should line up under vertex A in X and beside vertex B in Y.
  std::vector<sketch::Entity> e = {sketch::Point{{0.0f, 7.0f}, 1},
                                   sketch::Point{{6.0f, 3.0f}, 2}};
  sketch::Snap s = sketch::InferPoint(e, {0.1f, 3.1f}, std::nullopt, 0, kScale);
  REQUIRE(s.alignShareX.has_value());
  REQUIRE(s.alignShareY.has_value());
  CHECK(s.alignShareX->entity == 1u); // X from A
  CHECK(s.alignShareY->entity == 2u); // Y from B
  CHECK_THAT(s.uv.x, WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(s.uv.y, WithinAbs(3.0f, 1e-5f));
}

TEST_CASE("Coincident wins over axis alignment", "[inference]") {
  // Cursor near a vertex on both axes → the whole-point coincident snap, not a
  // pair of alignments.
  std::vector<sketch::Entity> e = {sketch::Point{{3.0f, 5.0f}, 1}};
  sketch::Snap s = sketch::InferPoint(e, {3.05f, 5.05f}, std::nullopt, 0, kScale);
  REQUIRE(s.coincident.has_value());
  CHECK_FALSE(s.alignShareX.has_value());
  CHECK_FALSE(s.alignShareY.has_value());
}

TEST_CASE("X alignment combines with a horizontal segment (close the rectangle)",
          "[inference]") {
  // Drawing from (6,3) toward a point that lines up under vertex V in X; the
  // segment is also near-horizontal, so Y snaps flat to the start.
  std::vector<sketch::Entity> e = {sketch::Point{{0.0f, 5.0f}, 1}};
  sketch::Snap s =
      sketch::InferPoint(e, {0.05f, 3.05f}, glm::vec2{6.0f, 3.0f}, 0, kScale);
  REQUIRE(s.alignShareX.has_value()); // X aligned to V
  CHECK(s.horizontal);                // segment flattened
  CHECK_THAT(s.uv.x, WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(s.uv.y, WithinAbs(3.0f, 1e-5f)); // = start Y
}

TEST_CASE("Infer snaps to circle, arc and ellipse vertices", "[inference]") {
  // Curve centres/endpoints are coincident and alignment candidates just like
  // line endpoints (any draw tool can snap to them).
  std::vector<sketch::Entity> e = {
      sketch::Circle{{3.0f, 4.0f}, 1.0, 1},
      sketch::Ellipse{{-5.0f, 2.0f}, 2.0, 1.0, 0.0, 2}};

  // Coincident with the circle centre.
  sketch::Snap c = sketch::InferPoint(e, {3.05f, 4.05f}, std::nullopt, 0, kScale);
  REQUIRE(c.coincident.has_value());
  CHECK(c.coincident->entity == 1u);
  CHECK(c.coincident->which == 0);

  // Coincident with the ellipse centre.
  sketch::Snap el = sketch::InferPoint(e, {-4.95f, 2.05f}, std::nullopt, 0, kScale);
  REQUIRE(el.coincident.has_value());
  CHECK(el.coincident->entity == 2u);

  // Vertical alignment (shared X) with the circle centre from far below.
  sketch::Snap ax = sketch::InferPoint(e, {3.05f, -6.0f}, std::nullopt, 0, kScale);
  CHECK_FALSE(ax.coincident.has_value());
  REQUIRE(ax.alignShareX.has_value());
  CHECK(ax.alignShareX->entity == 1u);
  CHECK_THAT(ax.uv.x, WithinAbs(3.0f, 1e-5f));

  // Coincident with an arc endpoint: quarter arc from 0 to pi/2 about (10,0),
  // radius 2 → start vertex at (12,0).
  std::vector<sketch::Entity> a = {
      sketch::Arc{{10.0f, 0.0f}, 2.0, 0.0, 1.5707963, 3}};
  sketch::Snap as = sketch::InferPoint(a, {12.05f, 0.05f}, std::nullopt, 0, kScale);
  REQUIRE(as.coincident.has_value());
  CHECK(as.coincident->entity == 3u);
  CHECK(as.coincident->which == 1); // start point
}

TEST_CASE("Infer snaps to the origin datum", "[inference][datum]") {
  std::vector<sketch::Entity> e; // empty sketch — the origin is always present
  sketch::Snap s = sketch::InferPoint(e, {0.3f, -0.2f}, std::nullopt, 0, kScale);
  REQUIRE(s.coincident.has_value());
  CHECK(s.coincident->entity == sketch::kOrigin);
  CHECK_THAT(s.uv.x, WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(s.uv.y, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Infer aligns vertical/horizontal to the origin", "[inference][datum]") {
  std::vector<sketch::Entity> e; // empty — the origin is always present
  // Shares the origin's X (x ~ 0), far in Y → vertical alignment to the origin.
  sketch::Snap vx = sketch::InferPoint(e, {0.05f, 5.0f}, std::nullopt, 0, kScale);
  REQUIRE(vx.alignShareX.has_value());
  CHECK(vx.alignShareX->entity == sketch::kOrigin);
  CHECK_THAT(vx.uv.x, WithinAbs(0.0f, 1e-5f));

  // Shares the origin's Y → horizontal alignment to the origin.
  sketch::Snap hy = sketch::InferPoint(e, {5.0f, -0.05f}, std::nullopt, 0, kScale);
  REQUIRE(hy.alignShareY.has_value());
  CHECK(hy.alignShareY->entity == sketch::kOrigin);
  CHECK_THAT(hy.uv.y, WithinAbs(0.0f, 1e-5f));
}
