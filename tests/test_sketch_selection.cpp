#include <catch2/catch_test_macros.hpp>

#include "core/features/SketchFeature.h"
#include "core/sketch/SketchSelection.h"

#include <algorithm>

static bool has(const std::vector<uint32_t>& v, uint32_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

TEST_CASE("BoxSelect: window selects only fully-enclosed entities",
          "[selection]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t a = sk.AddEntity(sketch::Line{{0, 0}, {1, 1}}); // inside [-1,2]^2
  sk.AddEntity(sketch::Line{{5, 5}, {6, 6}});              // outside
  sk.AddEntity(sketch::Line{{-5, 0}, {5, 0}}); // spans, not enclosed
  auto ident = [](glm::vec2 p) { return p; };  // UV == screen

  auto sel = sketch::BoxSelect(sk.entities, {-1, -1}, {2, 2},
                               /*crossing=*/false, ident);
  REQUIRE(sel.size() == 1);
  CHECK(sel[0] == a);
}

TEST_CASE("BoxSelect: crossing catches an entity the box merely touches",
          "[selection]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t d = sk.AddEntity(sketch::Line{{-5, 0}, {5, 0}}); // both ends outside
  auto ident = [](glm::vec2 p) { return p; };

  // A small box around the origin: the line's endpoints are outside it, so
  // window select misses it but crossing (segment through the box) catches it.
  auto win = sketch::BoxSelect(sk.entities, {-1, -1}, {1, 1}, false, ident);
  CHECK_FALSE(has(win, d));
  auto cross = sketch::BoxSelect(sk.entities, {-1, -1}, {1, 1}, true, ident);
  CHECK(has(cross, d));
}

TEST_CASE("BoxSelect: rect corners may be given in any order", "[selection]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t a = sk.AddEntity(sketch::Line{{0, 0}, {1, 1}});
  auto ident = [](glm::vec2 p) { return p; };
  // max/min swapped (a right→left, bottom→top drag) still selects a.
  auto sel = sketch::BoxSelect(sk.entities, {2, 2}, {-1, -1}, false, ident);
  REQUIRE(sel.size() == 1);
  CHECK(sel[0] == a);
}

TEST_CASE("BoxSelect: a point outside the box is excluded", "[selection]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  sk.AddEntity(sketch::Line{{0, 0}, {1, 1}});
  uint32_t p = sk.AddEntity(sketch::Point{{3, 3}}); // outside [-1,2]^2
  auto ident = [](glm::vec2 q) { return q; };
  auto sel = sketch::BoxSelect(sk.entities, {-1, -1}, {2, 2}, true, ident);
  CHECK_FALSE(has(sel, p));
}
