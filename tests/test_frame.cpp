#include <catch2/catch_test_macros.hpp>

#include "core/tube/Frame.h"

#include <algorithm>
#include <string>

namespace {

// A straight member between two points, sampled as the metal runs.
tube::FrameMember Bar(const std::string& name, glm::dvec3 a, glm::dvec3 b,
                      double od = 25.4) {
  tube::FrameMember m;
  m.name = name;
  m.outerDiameter = od;
  for (int i = 0; i <= 8; i++)
    m.centreline.push_back(a + (b - a) * (i / 8.0));
  return m;
}

int Count(const tube::CheckResult& r, const std::string& kind) {
  return static_cast<int>(
      std::count_if(r.violations.begin(), r.violations.end(),
                    [&](const tube::Violation& v) { return v.kind == kind; }));
}

} // namespace

TEST_CASE("A square bay with no diagonal is found", "[tube][frame]") {
  // Four bars round a square. In a pin-jointed frame that is a mechanism;
  // welded, it carries its load by bending its welds, which is the one thing
  // a space frame exists not to do.
  const glm::dvec3 a{0, 0, 0}, b{500, 0, 0}, c{500, 0, 500}, d{0, 0, 500};
  std::vector<tube::FrameMember> frame = {Bar("Bottom", a, b),
                                          Bar("Right", b, c), Bar("Top", c, d),
                                          Bar("Left", d, a)};
  const tube::CheckResult open = tube::CheckFrame(frame);
  CHECK(Count(open, "bay_not_triangulated") == 1);
  CHECK(Count(open, "member_end_unattached") == 0);

  // Put a diagonal in and it is a truss.
  frame.push_back(Bar("Diagonal", a, c));
  const tube::CheckResult braced = tube::CheckFrame(frame);
  CHECK(Count(braced, "bay_not_triangulated") == 0);

  // And it never claims more than it knows.
  CHECK(braced.violations.empty());
  CHECK(braced.notChecked.size() >= 2);
}

TEST_CASE("A member welded at one end only is found", "[tube][frame]") {
  // A closed triangle, so the only loose end in the frame is the one the
  // test is about. (A two-bar frame has free ends by construction, which is
  // the check being right and the test being wrong.)
  const glm::dvec3 a{0, 0, 0}, b{500, 0, 0}, c{250, 0, 400};
  std::vector<tube::FrameMember> frame = {
      Bar("Bottom", a, b), Bar("Right", b, c), Bar("Left", c, a),
      Bar("Post", {250, 0, 0}, {250, -400, 0})}; // free at its far end
  const tube::CheckResult r = tube::CheckFrame(frame);
  CHECK(Count(r, "member_end_unattached") == 1);
  const auto it = std::find_if(r.violations.begin(), r.violations.end(),
                               [](const tube::Violation& v) {
                                 return v.kind == "member_end_unattached";
                               });
  REQUIRE(it != r.violations.end());
  CHECK(it->member == "Post");
}

TEST_CASE("A joint landing on a bend is found", "[tube][frame]") {
  // The rail kinks, and a post stands exactly on the kink. A bend is a torus
  // there, thinned by the bending, with no straight to fit a cope against.
  tube::FrameMember rail = Bar("Rail", {0, -400, 0}, {0, 400, 0});
  rail.bends.push_back({12.0, 101.6, 400.0, 0.0});
  rail.corners.push_back({0, 0, 0});
  std::vector<tube::FrameMember> frame = {
      rail, Bar("Post", {0, 0, 0}, {0, 0, 400}, 19.05)};
  const tube::CheckResult on = tube::CheckFrame(frame);
  CHECK(Count(on, "joint_on_a_bend") == 1);
  const auto it = std::find_if(
      on.violations.begin(), on.violations.end(),
      [](const tube::Violation& v) { return v.kind == "joint_on_a_bend"; });
  REQUIRE(it != on.violations.end());
  CHECK(it->message.find("either side of that corner") != std::string::npos);

  // The same post a comfortable distance along the rail is fine.
  std::vector<tube::FrameMember> clear = {
      rail, Bar("Post", {0, 200, 0}, {0, 200, 400}, 19.05)};
  CHECK(Count(tube::CheckFrame(clear), "joint_on_a_bend") == 0);
}

TEST_CASE("A skew four-bar loop is not called a bay", "[tube][frame]") {
  // A bay is a FACE — four members round a roughly flat loop, which a
  // diagonal or a panel would close. A dense frame has combinatorially many
  // skew four-cycles, and reporting them teaches nothing.
  const glm::dvec3 a{0, 0, 0}, b{500, 0, 0}, c{500, 500, 400}, d{0, 500, 0};
  const std::vector<tube::FrameMember> frame = {
      Bar("One", a, b), Bar("Two", b, c), Bar("Three", c, d),
      Bar("Four", d, a)};
  CHECK(Count(tube::CheckFrame(frame), "bay_not_triangulated") == 0);
}
