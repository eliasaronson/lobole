// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Reading a round and a thread off a scan
// (docs/modeling/README.md#reading-a-round-off-a-scan): scanned prisms whose
// one corner is a round, a chamfer or sharp, between faces at 90 and 60
// degrees, and scanned threads of known pitch, diameter and hand, each
// against the generator's numbers.

#include "SectionOutlines.h"
#include "TestIO.h"
#include "mesh_organic.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/mesh/ReadRound.h"
#include "core/mesh/ReadThread.h"
#include "core/mesh/TriMesh.h"
#include "ui/CreateActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace outlines;

namespace {

constexpr double kPi = angles::kPi;

// A closed outline extruded `height` and scanned: rings of vertices every
// `spacing` round it and up the wall, each moved along the outline's normal
// by Gaussian noise; the caps fanned from `inside`.
mesh::TriMesh ScannedPrism(const std::vector<Prim>& prims, double height,
                           double spacing, double noise, uint32_t seed,
                           glm::dvec2 inside) {
  std::vector<glm::dvec2> ring, normal;
  for (const Prim& p : prims) {
    const int k = std::max(1, int(std::round(p.Length() / spacing)));
    for (int i = 0; i < k; i++) {
      const double s = p.Length() * i / k;
      ring.push_back(p.At(s));
      normal.push_back(p.Normal(s));
    }
  }
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, noise);
  mesh::TriMesh m;
  const int levels = std::max(1, int(std::round(height / spacing)));
  const uint32_t n = static_cast<uint32_t>(ring.size());
  for (int z = 0; z <= levels; z++)
    for (uint32_t i = 0; i < n; i++) {
      const glm::dvec2 p = ring[i] + normal[i] * gauss(rng);
      m.positions.push_back({p.x, p.y, height * z / levels});
    }
  for (int z = 0; z < levels; z++)
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t a = z * n + i, b = z * n + (i + 1) % n;
      m.triangles.push_back({a, b, b + n});
      m.triangles.push_back({a, b + n, a + n});
    }
  const uint32_t bottom = static_cast<uint32_t>(m.positions.size());
  m.positions.push_back({inside.x, inside.y, 0.0});
  m.positions.push_back({inside.x, inside.y, height});
  const uint32_t topRing = static_cast<uint32_t>(levels) * n;
  for (uint32_t i = 0; i < n; i++) {
    m.triangles.push_back({bottom, (i + 1) % n, i});
    m.triangles.push_back({bottom + 1, topRing + i, topRing + (i + 1) % n});
  }
  mesh::BuildAdjacency(m);
  return m;
}

enum class CornerKind { Round, Chamfer, Sharp };

// A parallelogram with sides of `side` whose corner at the origin is
// `insideDeg` between its faces and is a round of r, a chamfer of r along
// each face, or sharp; counter-clockwise. `at` gets the corner's middle.
std::vector<Prim> Corner(double insideDeg, CornerKind kind, double r,
                         double side, glm::dvec2& at) {
  const double th = angles::ToRadians(insideDeg);
  const glm::dvec2 a1(1, 0), a2(std::cos(th), std::sin(th));
  const glm::dvec2 far1 = a1 * side, far2 = a2 * side;
  std::vector<Prim> out;
  if (kind == CornerKind::Sharp) {
    out.push_back(L({0, 0}, far1));
    at = {0, 0};
  } else if (kind == CornerKind::Chamfer) {
    out.push_back(L(a2 * r, a1 * r));
    out.push_back(L(a1 * r, far1));
    at = 0.5 * (a1 + a2) * r;
  } else {
    const double t = r / std::tan(th / 2);
    const glm::dvec2 bis = glm::normalize(a1 + a2);
    const glm::dvec2 o = bis * (r / std::sin(th / 2));
    const glm::dvec2 s = a2 * t - o;
    out.push_back(
        A(o, r, angles::ToDegrees(std::atan2(s.y, s.x)), 180.0 - insideDeg));
    out.push_back(L(a1 * t, far1));
    at = o - bis * r;
  }
  out.push_back(L(far1, far1 + far2));
  out.push_back(L(far1 + far2, far2));
  const glm::dvec2 end = kind == CornerKind::Sharp ? glm::dvec2(0.0)
                         : kind == CornerKind::Chamfer
                             ? a2 * r
                             : a2 * (r / std::tan(th / 2));
  out.push_back(L(far2, end));
  // Start at the far side: the round is then away from the seam.
  std::rotate(out.begin(), out.end() - 2, out.end());
  return out;
}

struct Scanned {
  mesh::TriMesh mesh;
  glm::dvec3 at;
};

Scanned ScanCorner(double insideDeg, CornerKind kind, double r, double spacing,
                   double noise, uint32_t seed) {
  glm::dvec2 at;
  const double side = std::max(30.0, 8.0 * r);
  const auto prims = Corner(insideDeg, kind, r, side, at);
  const glm::dvec2 th(std::cos(angles::ToRadians(insideDeg)),
                      std::sin(angles::ToRadians(insideDeg)));
  const glm::dvec2 inside = 0.5 * side * (glm::dvec2(1, 0) + th);
  const double height = 40.0;
  return {ScannedPrism(prims, height, spacing, noise, seed, inside),
          {at.x, at.y, height / 2}};
}

mesh::RoundReading Read(const Scanned& s) {
  return mesh::ReadRound({&s.mesh}, s.at);
}

void Show(const char* what, const mesh::RoundReading& r) {
  std::printf("read_round %s: %s r %.4f +- %.4f, chamfer %.3f / %.3f, angle "
              "%.2f, noise %.4f, window %.2f mm, worse {round %.1f, chamfer "
              "%.1f, sharp %.1f}%s\n",
              what, mesh::ToString(r.verdict), r.radiusMm, r.sigmaMm,
              r.chamferMm[0], r.chamferMm[1], r.angleDeg, r.noiseMm, r.windowMm,
              r.worse[0], r.worse[1], r.worse[2],
              r.ok ? "" : (" -- " + r.why).c_str());
}

} // namespace

TEST_CASE("Rounds read off a scan within their sigma", "[mesh][read_round]") {
  struct C {
    double r, angle, spacing;
  };
  // A 0.5 mm round sampled every 0.05 mm, the larger every 0.1 and 0.2:
  // a round has to carry a dozen points to be read as one
  // (docs/sketching/README.md#the-pieces-fit).
  const C cases[] = {{0.5, 90, 0.05}, {2, 90, 0.1}, {6, 90, 0.2},
                     {0.5, 60, 0.05}, {2, 60, 0.1}, {6, 60, 0.2}};
  uint32_t seed = 300;
  for (const C& c : cases) {
    const Scanned s =
        ScanCorner(c.angle, CornerKind::Round, c.r, c.spacing, 0.02, seed++);
    const mesh::RoundReading r = Read(s);
    char what[64];
    std::snprintf(what, sizeof what, "r %.1f at %.0f deg", c.r, c.angle);
    Show(what, r);
    INFO(what << ": " << r.why);
    REQUIRE(r.ok);
    CHECK(r.verdict == mesh::EdgeReading::Round);
    CHECK(std::abs(r.angleDeg - c.angle) < 1.5);
    CHECK(r.sigmaMm > 0.0);
    CHECK(std::abs(r.radiusMm - c.r) <= 3.0 * r.sigmaMm);
    CHECK(r.sigmaMm < 0.1 * c.r);
  }
}

TEST_CASE("A sharp edge and a chamfer are named as such",
          "[mesh][read_round]") {
  {
    const Scanned s = ScanCorner(90, CornerKind::Sharp, 0, 0.1, 0.02, 400);
    const mesh::RoundReading r = Read(s);
    Show("sharp at 90 deg", r);
    REQUIRE(r.ok);
    CHECK(r.verdict == mesh::EdgeReading::Sharp);
  }
  {
    const Scanned s = ScanCorner(90, CornerKind::Chamfer, 1.5, 0.1, 0.02, 401);
    const mesh::RoundReading r = Read(s);
    Show("chamfer 1.5 at 90 deg", r);
    REQUIRE(r.ok);
    CHECK(r.verdict == mesh::EdgeReading::Chamfer);
    CHECK(std::abs(r.chamferMm[0] - 1.5) < 0.1);
    CHECK(std::abs(r.chamferMm[1] - 1.5) < 0.1);
  }
}

TEST_CASE("A small round in a coarse scan says what else it may be",
          "[mesh][read_round]") {
  // A 1 mm round at 0.03 mm noise every 0.2 mm: the scan carries six points
  // on it. Reported, with what the evidence does not tell it from.
  int rounds = 0, ambiguous = 0;
  for (uint32_t seed = 0; seed < 5; seed++) {
    const Scanned s =
        ScanCorner(90, CornerKind::Round, 1.0, 0.2, 0.03, 500 + seed);
    const mesh::RoundReading r = Read(s);
    Show("r 1 coarse", r);
    REQUIRE(r.ok);
    rounds += r.verdict == mesh::EdgeReading::Round;
    ambiguous += !r.alsoFits.empty();
    // Whatever it is read as, a round it is not told from is named.
    if (r.verdict != mesh::EdgeReading::Round)
      CHECK(std::find(r.alsoFits.begin(), r.alsoFits.end(),
                      mesh::EdgeReading::Round) != r.alsoFits.end());
  }
  std::printf("r 1 coarse: %d of 5 read as a round, %d with another reading "
              "not told from it\n",
              rounds, ambiguous);
}

TEST_CASE("No scan and no edge are refused, naming why", "[mesh][read_round]") {
  const Scanned s = ScanCorner(90, CornerKind::Round, 2, 0.2, 0.02, 600);
  const mesh::RoundReading far = mesh::ReadRound({&s.mesh}, {100, 100, 100});
  CHECK(!far.ok);
  CHECK(far.noScan);
  // The middle of a face: one face, no edge.
  const mesh::RoundReading flat = mesh::ReadRound({&s.mesh}, {15, 0, 5});
  CHECK(!flat.ok);
  CHECK_THAT(flat.why, ContainsSubstring("no edge"));
}

namespace {

// A threaded cylinder's surface, scanned: a grid round the axis (z) every
// `spacing`, the radius from the profile at the point's phase along the
// helix; right-hand when the phase advances with the angle. `internal`
// faces it towards the axis (a nut's thread).
template <class Profile>
mesh::TriMesh ScannedThread(double rMajor, double pitch, bool rightHand,
                            bool internal, double length, double spacing,
                            double noise, uint32_t seed, Profile profile) {
  const int nt = std::max(16, int(std::round(2 * kPi * rMajor / spacing)));
  const int nz = std::max(2, int(std::round(length / spacing)));
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, noise);
  mesh::TriMesh m;
  const double hand = rightHand ? 1.0 : -1.0;
  for (int k = 0; k <= nz; k++)
    for (int i = 0; i < nt; i++) {
      const double th = 2 * kPi * i / nt, z = length * k / nz;
      double ph = (z - hand * pitch * th / (2 * kPi)) / pitch;
      ph -= std::floor(ph);
      const double r = profile(ph) + gauss(rng);
      m.positions.push_back({r * std::cos(th), r * std::sin(th), z});
    }
  for (int k = 0; k < nz; k++)
    for (int i = 0; i < nt; i++) {
      const uint32_t a = k * nt + i, b = k * nt + (i + 1) % nt;
      if (internal) {
        m.triangles.push_back({a, b + nt, b});
        m.triangles.push_back({a, a + nt, b + nt});
      } else {
        m.triangles.push_back({a, b, b + nt});
        m.triangles.push_back({a, b + nt, a + nt});
      }
    }
  mesh::BuildAdjacency(m);
  return m;
}

// ISO metric's basic profile: a crest flat of P/8, a root flat of P/4, the
// flanks 30 degrees from the radial plane, 5H/8 deep.
auto Metric(double rMajor, double pitch) {
  return [=](double ph) {
    const double depth = 0.5413 * pitch;
    // Crest flat centred on 0, flanks, root flat centred on 0.5.
    const double u = std::abs(ph - std::round(ph)); // 0 at crest, .5 at root
    if (u <= 1.0 / 16)
      return rMajor;
    if (u >= 0.5 - 1.0 / 8)
      return rMajor - depth;
    return rMajor - depth * (u - 1.0 / 16) / (0.5 - 1.0 / 8 - 1.0 / 16);
  };
}

// ISO trapezoidal: 15 degree flanks, P/2 deep, equal crest and root flats.
auto TrapezoidalProfile(double rMajor, double pitch) {
  return [=](double ph) {
    const double depth = 0.5 * pitch;
    const double flank = depth * std::tan(angles::ToRadians(15.0)) / pitch;
    const double flat = 0.25 - flank / 2; // each flat's half-width
    const double u = std::abs(ph - std::round(ph));
    if (u <= flat)
      return rMajor;
    if (u >= 0.5 - flat)
      return rMajor - depth;
    return rMajor - depth * (u - flat) / flank;
  };
}

void ShowThread(const char* what, const mesh::ThreadReading& t) {
  std::printf("read_thread %s: pitch %.4f +- %.4f, major %.3f, minor %.3f, "
              "flank %.1f, %s, %s, %d of %d windows, phase %.2f, nearest %s"
              "%s\n",
              what, t.pitchMm, t.pitchSigmaMm, t.majorDiameterMm,
              t.minorDiameterMm, t.flankDeg, t.rightHand ? "RH" : "LH",
              t.internal ? "internal" : "external", t.windowsAgree, t.windows,
              t.phaseShare, t.nearest.c_str(),
              t.ok ? "" : (" -- " + t.why).c_str());
}

} // namespace

TEST_CASE("An M8 x 1.25 scanned thread reads as one", "[mesh][read_thread]") {
  const mesh::TriMesh m = ScannedThread(4.0, 1.25, true, false, 20.0, 0.08,
                                        0.02, 700, Metric(4.0, 1.25));
  const mesh::ThreadReading t = mesh::ReadThread({&m}, {4.0, 0.0, 10.0});
  ShowThread("M8x1.25", t);
  INFO(t.why);
  REQUIRE(t.ok);
  CHECK(std::abs(t.pitchMm - 1.25) < 0.005);
  CHECK(std::abs(t.majorDiameterMm - 8.0) < 0.1);
  CHECK(t.rightHand);
  CHECK(!t.internal);
  CHECK(std::abs(t.flankDeg - 30.0) < 5.0);
  CHECK(t.nearest == "M8");
}

TEST_CASE("A left-hand trapezoidal nut thread reads as one",
          "[mesh][read_thread]") {
  const mesh::TriMesh m = ScannedThread(8.0, 4.0, false, true, 40.0, 0.1, 0.02,
                                        701, TrapezoidalProfile(8.0, 4.0));
  const mesh::ThreadReading t = mesh::ReadThread({&m}, {7.0, 0.0, 20.0});
  ShowThread("Tr16x4 LH nut", t);
  INFO(t.why);
  REQUIRE(t.ok);
  CHECK(std::abs(t.pitchMm - 4.0) < 0.01);
  CHECK(std::abs(t.majorDiameterMm - 16.0) < 0.15);
  CHECK(!t.rightHand);
  CHECK(t.internal);
  CHECK(std::abs(t.flankDeg - 15.0) < 4.0);
  CHECK(t.nearest == "Tr16x4 LH");
}

TEST_CASE("A plain bore is refused as no thread", "[mesh][read_thread]") {
  const mesh::TriMesh m = ScannedThread(5.0, 1.0, true, true, 20.0, 0.1, 0.02,
                                        702, [](double) { return 5.0; });
  const mesh::ThreadReading t = mesh::ReadThread({&m}, {5.0, 0.0, 10.0});
  ShowThread("plain bore", t);
  CHECK(!t.ok);
  CHECK(t.noThread);
  CHECK_THAT(t.why, ContainsSubstring("no thread"));
}

namespace {

uint64_t AddScan(Document& doc, const mesh::TriMesh& m,
                 const std::string& tag) {
  const auto stl = fixtures::WriteStl(m, tag);
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  doc.AddFeature(std::move(imp));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The element of `feature`'s body nearest `at` by its bounding box's middle:
// an edge (`edges`) or a face.
ElementRef Nearest(const Document& doc, uint64_t feature, glm::dvec3 at,
                   bool edges) {
  ElementRef best;
  double bd = 1e30;
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy != feature)
      continue;
    const auto& names = edges ? b.elementMap.edgeNames : b.elementMap.names;
    for (const auto& [index, name] : names) {
      const ElementRef ref{feature, name};
      const auto box = Measure::ElementBounds(doc.GetBodies(), ref);
      if (!box)
        continue;
      const double d = glm::length(0.5 * (box->min + box->max) - at);
      if (d < bd) {
        bd = d;
        best = ref;
      }
    }
  }
  return best;
}

} // namespace

TEST_CASE("A model's edge at a scanned round takes the round's radius",
          "[mesh][read_round][mesh_body]") {
  // The Fillet tool's default on a model drawn over a scan
  // (docs/ui/tool-sessions.md#defaults-and-refusals): the model's sharp
  // edge stands off the scan's 2 mm round, and the round is read there.
  Document doc;
  const Scanned s = ScanCorner(90, CornerKind::Round, 2.0, 0.1, 0.02, 800);
  AddScan(doc, s.mesh, "blend-default");
  auto sk =
      std::make_unique<SketchFeature>("Square", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Line{{0, 0}, {30, 0}});
  sk->AddEntity(sketch::Line{{30, 0}, {30, 30}});
  sk->AddEntity(sketch::Line{{30, 30}, {0, 30}});
  sk->AddEntity(sketch::Line{{0, 30}, {0, 0}});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Box", skId, 40.0,
                                                  ExtrudeMode::NewBody));
  const uint64_t box = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  REQUIRE(doc.GetBodies().size() == 2);
  const ElementRef edge = Nearest(doc, box, {0, 0, 20}, true);
  REQUIRE(edge.IsSet());
  const auto r = ui::BlendFromScan(doc.GetBodies(), edge, false);
  REQUIRE(r.has_value());
  CHECK(*r == 2.0); // the reading put on the 0.5 mm step
  // A chamfer asked of a round: the scan says no chamfer.
  CHECK(!ui::BlendFromScan(doc.GetBodies(), edge, true).has_value());
  // An edge away from the scan's round: the far corner is sharp.
  const ElementRef sharpEdge = Nearest(doc, box, {30, 0, 20}, true);
  CHECK(!ui::BlendFromScan(doc.GetBodies(), sharpEdge, false).has_value());
}

TEST_CASE("A model's cylinder in a scanned thread takes the thread",
          "[mesh][read_thread][mesh_body]") {
  Document doc;
  AddScan(doc,
          ScannedThread(4.0, 1.25, true, false, 20.0, 0.08, 0.02, 801,
                        Metric(4.0, 1.25)),
          "thread-default");
  auto sk = std::make_unique<SketchFeature>("Rod", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{{0, 0}, 3.6});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Rod", skId, 20.0,
                                                  ExtrudeMode::NewBody));
  const uint64_t rod = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  REQUIRE(doc.GetBodies().size() == 2);
  // The side face: its box's middle is on the axis, half way up.
  const ElementRef face = Nearest(doc, rod, {0, 0, 10}, false);
  REQUIRE(face.IsSet());
  const auto t = ui::ThreadFromScan(doc.GetBodies(), face);
  REQUIRE(t.has_value());
  CHECK(t->designation == "M8");
  CHECK(t->pitch == 1.25);
  CHECK(t->rightHanded);
}
