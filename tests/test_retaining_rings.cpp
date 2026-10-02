// DIN 471 / DIN 472 retaining rings as generated standard parts, and the
// groove one sits in (docs/parts/README.md#retaining-rings): the table
// against Seeger-Orbis's data charts, the ring's body, the groove feature and
// its refusals, round trip, and the ring placed and counted in an assembly.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/RetainingRingFeature.h"
#include "core/features/RetainingRingGrooveFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/RetainingRingTable.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/RetainingRingActions.h"

#include <cmath>
#include <numbers>
#include <set>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using parts::RetainingRingStandard;

TEST_CASE("The retaining ring tables are Seeger-Orbis's DIN 471 and 472",
          "[parts][retaining_ring]") {
  // Rows read off the data charts "Seeger-Rings for shafts A3-A56 / DIN 471"
  // and "for bores J8-J58, J60-J150 / DIN 472": s, d3, b; d2, m, t, n.
  struct Want {
    RetainingRingStandard standard;
    double d1, s, d3, b, d2, m, t, n;
  };
  const Want wants[] = {
      {RetainingRingStandard::Din471, 10, 1.0, 9.3, 1.8, 9.6, 1.1, 0.2, 0.6},
      {RetainingRingStandard::Din471, 15, 1.0, 13.8, 2.2, 14.3, 1.1, 0.35, 1.1},
      {RetainingRingStandard::Din471, 25, 1.2, 23.2, 3.0, 23.9, 1.3, 0.55, 1.7},
      {RetainingRingStandard::Din471, 56, 2.0, 51.8, 5.5, 53.0, 2.15, 1.5, 4.5},
      {RetainingRingStandard::Din472, 8, 0.8, 8.7, 1.1, 8.4, 0.9, 0.2, 0.6},
      {RetainingRingStandard::Din472, 22, 1.0, 23.5, 2.5, 23.0, 1.1, 0.5, 1.5},
      {RetainingRingStandard::Din472, 47, 1.75, 50.5, 4.4, 49.5, 1.85, 1.25,
       3.8},
      {RetainingRingStandard::Din472, 72, 2.5, 76.5, 6.4, 75.0, 2.65, 1.5, 4.5},
  };
  for (const Want& w : wants) {
    INFO(parts::RetainingRingStandardName(w.standard) << " " << w.d1);
    const parts::RetainingRingRow* r =
        parts::FindRetainingRing(w.standard, w.d1);
    REQUIRE(r != nullptr);
    CHECK(r->s == w.s);
    CHECK(r->d3 == w.d3);
    CHECK(r->b == w.b);
    CHECK(r->d2 == w.d2);
    CHECK(r->m == w.m);
    CHECK(r->t == w.t);
    CHECK(r->n == w.n);
  }
  // Every row agrees with itself: the groove depth is half the step from
  // the nominal to the groove diameter, the free ring is smaller than the
  // groove on a shaft (larger in a bore) — that is its grip — the groove is
  // wider than the ring, and no size is listed twice.
  for (RetainingRingStandard st :
       {RetainingRingStandard::Din471, RetainingRingStandard::Din472}) {
    const bool shaft = st == RetainingRingStandard::Din471;
    std::set<double> seen;
    double last = 0.0;
    for (const parts::RetainingRingRow& r : parts::RetainingRings(st)) {
      INFO(parts::RetainingRingStandardName(st) << " " << r.d1);
      CHECK(seen.insert(r.d1).second);
      CHECK(r.d1 > last);
      last = r.d1;
      CHECK_THAT(r.t, WithinAbs(std::abs(r.d1 - r.d2) / 2.0, 0.011));
      if (shaft) {
        CHECK(r.d3 < r.d2);
        CHECK(r.d2 < r.d1);
      } else {
        CHECK(r.d3 > r.d2);
        CHECK(r.d2 > r.d1);
      }
      CHECK(r.m > r.s);
      CHECK(r.n > 0.0);
    }
  }
  CHECK(parts::RetainingRings(RetainingRingStandard::Din471).front().d1 == 3);
  CHECK(parts::RetainingRings(RetainingRingStandard::Din471).back().d1 == 56);
  CHECK(parts::RetainingRings(RetainingRingStandard::Din472).front().d1 == 8);
  CHECK(parts::RetainingRings(RetainingRingStandard::Din472).back().d1 == 72);
  const parts::RetainingRingRow* a25 =
      parts::FindRetainingRing(RetainingRingStandard::Din471, 25);
  REQUIRE(a25 != nullptr);
  CHECK(parts::RetainingRingDesignation(RetainingRingStandard::Din471, *a25) ==
        "DIN 471 25x1.2");
}

TEST_CASE("An unknown retaining ring is refused naming what is carried",
          "[parts][retaining_ring]") {
  std::string why;
  // DIN 471 has no 43 (the chart goes 42, 44).
  CHECK(parts::FindRetainingRing(RetainingRingStandard::Din471, 43, &why) ==
        nullptr);
  CHECK_THAT(why, ContainsSubstring("Ø43 shaft"));
  CHECK_THAT(why, ContainsSubstring("42 or 44"));
  CHECK_THAT(why, ContainsSubstring("3, 4, 5"));
  CHECK(parts::FindRetainingRing(RetainingRingStandard::Din472, 7, &why) ==
        nullptr);
  CHECK_THAT(why, ContainsSubstring("Ø7 bore"));
  CHECK_THAT(why, ContainsSubstring("nearest carried are 8"));
  RetainingRingStandard out{};
  CHECK_FALSE(parts::RetainingRingStandardFromString("din6799", out));
  CHECK_FALSE(parts::RetainingRingStandardFromString("DIN471", out));

  // The feature says the same, as its error.
  Document doc;
  doc.AddFeature(std::make_unique<RetainingRingFeature>("R", "din471", 25.5));
  CHECK_FALSE(doc.Regenerate());
  CHECK_THAT(doc.GetFeature(0)->GetError(), ContainsSubstring("Ø25.5 shaft"));
  Document bad;
  bad.AddFeature(std::make_unique<RetainingRingFeature>("R", "seeger", 25));
  CHECK_FALSE(bad.Regenerate());
  CHECK_THAT(bad.GetFeature(0)->GetError(),
             ContainsSubstring("din471 (external, for shafts)"));
}

namespace {

// The face of `body` whose name's seed is "<feature>/<role>": a later
// feature's steps follow it (";M4" once the groove's cut has run).
std::optional<Measure::FaceInfo> NamedFace(const Body& body,
                                           const std::string& role) {
  for (const auto& [index, full] : body.elementMap.names) {
    const std::string name = full.substr(0, full.find(';'));
    if (name.size() > role.size() &&
        name.compare(name.size() - role.size(), role.size(), role) == 0 &&
        name[name.size() - role.size() - 1] == '/')
      return Measure::DescribeFaceAt(body, index);
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("A retaining ring is built from its row", "[parts][retaining_ring]") {
  SECTION("DIN 471: inside the free diameter, outside d3 + 2b") {
    Document doc;
    doc.AddFeature(std::make_unique<RetainingRingFeature>("R", "din471", 25));
    REQUIRE(doc.Regenerate());
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies()[0];
    const double ri = 23.2 / 2.0, ro = ri + 3.0, s = 1.2;
    CHECK_THAT(body.shape.Volume(),
               WithinAbs(std::numbers::pi * (ro * ro - ri * ri) * s, 1e-6));
    const auto bore = NamedFace(body, "bore");
    const auto outside = NamedFace(body, "outside");
    const auto sideA = NamedFace(body, "ring_side_a");
    const auto sideB = NamedFace(body, "ring_side_b");
    REQUIRE(bore);
    REQUIRE(outside);
    REQUIRE(sideA);
    REQUIRE(sideB);
    CHECK_THAT(bore->radius, WithinAbs(ri, 1e-9));
    CHECK_THAT(outside->radius, WithinAbs(ro, 1e-9));
    CHECK_THAT(sideA->centroid.z, WithinAbs(0.0, 1e-9));
    CHECK_THAT(sideB->centroid.z, WithinAbs(s, 1e-9));
    CHECK(body.material == "steel");
    // The names decode to their roles.
    for (const auto& [index, name] : body.elementMap.names) {
      const topo::ElementOrigin o = topo::DecodeElementName(name);
      INFO(name);
      CHECK(o.ok);
      if (name.ends_with("ring_side_a"))
        CHECK(o.role == topo::ElementRole::RingSide);
    }
  }
  SECTION("DIN 472: outside the free diameter, inside d3 - 2b") {
    Document doc;
    doc.AddFeature(std::make_unique<RetainingRingFeature>("R", "din472", 47));
    REQUIRE(doc.Regenerate());
    const Body& body = doc.GetBodies()[0];
    const auto bore = NamedFace(body, "bore");
    const auto outside = NamedFace(body, "outside");
    REQUIRE(bore);
    REQUIRE(outside);
    CHECK_THAT(outside->radius, WithinAbs(50.5 / 2.0, 1e-9));
    CHECK_THAT(bore->radius, WithinAbs(50.5 / 2.0 - 4.4, 1e-9));
    CHECK_THAT(NamedFace(body, "ring_side_b")->centroid.z,
               WithinAbs(1.75, 1e-9));
  }
}

namespace {

// A Ø40 x 10 collar at z 0..10 and a Ø`journal` x 30 journal on it to z 40:
// a 6005's shaft, shoulder and all.
struct Shaft {
  Document doc;
  uint64_t body = 0;
  ElementRef journal;   // the journal's cylinder
  ElementRef rim;       // the journal's edge on the shoulder, z = 10
  ElementRef end;       // its edge at the free end, z = 40
  ElementRef collarRim; // the collar's outer edge, z = 10
  ElementRef top;       // the journal's end face, z = 40
};

void BuildShaft(Shaft& s, double journal = 25.0) {
  Document& doc = s.doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Collar", glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), 20.0, 10.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::dvec3(0, 0, 10), glm::dvec3(0, 0, 1), journal / 2.0,
      30.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Join", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Union));
  REQUIRE(doc.Regenerate());
  const Body& body = doc.GetBodies().front();
  s.body = body.createdBy;
  for (const auto& [index, mapped] : body.elementMap.names) {
    const auto face = Measure::DescribeFaceAt(body, index);
    if (!face)
      continue;
    if (face->kind == "cylinder" &&
        std::abs(face->radius - journal / 2.0) < 1e-6)
      s.journal = ElementRef{s.body, mapped};
    if (face->kind == "plane" && std::abs(face->centroid.z - 40.0) < 1e-6)
      s.top = ElementRef{s.body, mapped};
  }
  for (const auto& [index, mapped] : body.elementMap.edgeNames) {
    const auto edge = Measure::DescribeEdgeAt(body, index);
    if (!edge || edge->kind != "circle")
      continue;
    if (std::abs(edge->radius - journal / 2.0) < 1e-6 &&
        std::abs(edge->midpoint.z - 10.0) < 1e-6)
      s.rim = ElementRef{s.body, mapped};
    if (std::abs(edge->radius - journal / 2.0) < 1e-6 &&
        std::abs(edge->midpoint.z - 40.0) < 1e-6)
      s.end = ElementRef{s.body, mapped};
    if (std::abs(edge->radius - 20.0) < 1e-6 &&
        std::abs(edge->midpoint.z - 10.0) < 1e-6)
      s.collarRim = ElementRef{s.body, mapped};
  }
  REQUIRE(s.journal.IsSet());
  REQUIRE(s.rim.IsSet());
  REQUIRE(s.end.IsSet());
  REQUIRE(s.collarRim.IsSet());
  REQUIRE(s.top.IsSet());
}

RetainingRingGrooveFeature* AddGroove(Shaft& s, const ElementRef& face,
                                      const ElementRef& from, double offset,
                                      const std::string& standard = {}) {
  auto groove = std::make_unique<RetainingRingGrooveFeature>("Groove", face,
                                                             from, offset);
  groove->standard = standard;
  RetainingRingGrooveFeature* out = groove.get();
  s.doc.AddFeature(std::move(groove));
  return out;
}

} // namespace

TEST_CASE("A DIN 471 groove is cut in a shaft at the stated offset",
          "[parts][retaining_ring]") {
  Shaft s;
  BuildShaft(s);
  const double before = s.doc.GetBodies().front().shape.Volume();
  // A 6005 (B = 12) against the shoulder, the ring on its far side.
  AddGroove(s, s.journal, s.rim, 12.0);
  REQUIRE(s.doc.Regenerate());
  const Body& body = s.doc.GetBodies().front();
  // DIN 471 for Ø25: d2 23.9, m 1.3 — the ring of material between Ø23.9
  // and Ø25 over 1.3 mm is gone.
  const double removed =
      std::numbers::pi * (12.5 * 12.5 - (23.9 / 2.0) * (23.9 / 2.0)) * 1.3;
  CHECK_THAT(before - body.shape.Volume(), WithinAbs(removed, 1e-6));
  const auto floor = NamedFace(body, "groove_floor");
  const auto flankA = NamedFace(body, "groove_flank_a");
  const auto flankB = NamedFace(body, "groove_flank_b");
  REQUIRE(floor);
  REQUIRE(flankA);
  REQUIRE(flankB);
  CHECK(floor->kind == "cylinder");
  CHECK_THAT(floor->radius, WithinAbs(23.9 / 2.0, 1e-9));
  CHECK_THAT(floor->centroid.z, WithinAbs(10.0 + 12.0 + 0.65, 1e-6));
  // The near flank, the one the ring bears on, is the bearing's width from
  // the shoulder; the far one is the groove's width past it.
  CHECK(flankA->kind == "plane");
  CHECK_THAT(flankA->centroid.z, WithinAbs(22.0, 1e-9));
  CHECK_THAT(flankB->centroid.z, WithinAbs(23.3, 1e-9));
  // The names decode to their roles, a later step and all.
  int floors = 0, flanks = 0;
  for (const auto& [index, name] : body.elementMap.names) {
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    floors += o.role == topo::ElementRole::GrooveFloor;
    flanks += o.role == topo::ElementRole::GrooveFlank;
  }
  CHECK(floors == 1);
  CHECK(flanks == 2);

  // Measured from the free end instead, the groove runs the other way.
  Shaft t;
  BuildShaft(t);
  AddGroove(t, t.journal, t.end, 5.0);
  REQUIRE(t.doc.Regenerate());
  const Body& tb = t.doc.GetBodies().front();
  CHECK_THAT(NamedFace(tb, "groove_flank_a")->centroid.z,
             WithinAbs(35.0, 1e-9));
  CHECK_THAT(NamedFace(tb, "groove_flank_b")->centroid.z,
             WithinAbs(33.7, 1e-9));

  // Round trip.
  const nlohmann::json j = DocumentIO::FeatureToJson(*s.doc.GetFeature(3));
  CHECK(j.at("type") == "RetainingRingGroove");
  CHECK(j.at("offset") == 12.0);
  CHECK(j.at("from").at("element") == s.rim.name);
  auto reloaded = DocumentIO::FeatureFromJson(j);
  auto* g = dynamic_cast<RetainingRingGrooveFeature*>(reloaded.get());
  REQUIRE(g != nullptr);
  CHECK(g->face.name == s.journal.name);
  CHECK(g->from.name == s.rim.name);
  CHECK(g->offset == 12.0);
}

TEST_CASE("A groove that does not fit its face is refused",
          "[parts][retaining_ring]") {
  const auto error = [](double journal, auto&& add) {
    Shaft s;
    BuildShaft(s, journal);
    add(s);
    CHECK_FALSE(s.doc.Regenerate());
    return s.doc.GetFeature(3)->GetError();
  };
  // Runs off the 30 mm journal.
  CHECK_THAT(
      error(25.0, [](Shaft& s) { AddGroove(s, s.journal, s.rim, 29.0); }),
      ContainsSubstring("runs off this face"));
  // Leaves 0.7 mm past it where DIN 471 wants n = 1.7: the land that
  // carries the thrust. The refusal says how far it may go.
  const std::string land =
      error(25.0, [](Shaft& s) { AddGroove(s, s.journal, s.rim, 28.0); });
  CHECK_THAT(land, ContainsSubstring("edge distance n is 1.7"));
  CHECK_THAT(land, ContainsSubstring("at most 27"));
  // A bore's groove asked of a shaft.
  CHECK_THAT(
      error(25.0,
            [](Shaft& s) { AddGroove(s, s.journal, s.rim, 12.0, "din472"); }),
      ContainsSubstring("takes din471"));
  // A diameter the table does not carry.
  CHECK_THAT(
      error(25.5, [](Shaft& s) { AddGroove(s, s.journal, s.rim, 12.0); }),
      ContainsSubstring("no DIN 471 ring for a Ø25.5 shaft"));
  // Not a cylinder.
  CHECK_THAT(error(25.0, [](Shaft& s) { AddGroove(s, s.top, s.end, 1.0); }),
             ContainsSubstring("CYLINDRICAL face"));
  // An edge that is not this face's.
  CHECK_THAT(
      error(25.0, [](Shaft& s) { AddGroove(s, s.journal, s.collarRim, 12.0); }),
      ContainsSubstring("not a circular edge of this face"));
  // Negative.
  CHECK_THAT(
      error(25.0, [](Shaft& s) { AddGroove(s, s.journal, s.rim, -1.0); }),
      ContainsSubstring("negative"));
  // An unknown standard is refused naming the two.
  CHECK_THAT(
      error(25.0,
            [](Shaft& s) { AddGroove(s, s.journal, s.rim, 12.0, "din6799"); }),
      ContainsSubstring("din472 (internal, for bores)"));
}

TEST_CASE("A DIN 472 groove is cut in a bore", "[parts][retaining_ring]") {
  // A 20 mm plate with a Ø47 bore through it: a 6005's housing.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Plate", glm::dvec3(-40, -40, 0),
                                              80.0, 80.0, 20.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 23.5, 22.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(doc.Regenerate());
  const Body& body = doc.GetBodies().front();
  ElementRef bore, rim;
  for (const auto& [index, mapped] : body.elementMap.names) {
    const auto face = Measure::DescribeFaceAt(body, index);
    if (face && face->kind == "cylinder")
      bore = ElementRef{body.createdBy, mapped};
  }
  for (const auto& [index, mapped] : body.elementMap.edgeNames) {
    const auto edge = Measure::DescribeEdgeAt(body, index);
    if (edge && edge->kind == "circle" && std::abs(edge->midpoint.z) < 1e-6)
      rim = ElementRef{body.createdBy, mapped};
  }
  REQUIRE(bore.IsSet());
  REQUIRE(rim.IsSet());
  const double before = body.shape.Volume();
  doc.AddFeature(
      std::make_unique<RetainingRingGrooveFeature>("Groove", bore, rim, 12.0));
  REQUIRE(doc.Regenerate());
  const Body& cut = doc.GetBodies().front();
  // DIN 472 for Ø47: d2 49.5, m 1.85.
  const double removed =
      std::numbers::pi * ((49.5 / 2.0) * (49.5 / 2.0) - 23.5 * 23.5) * 1.85;
  CHECK_THAT(before - cut.shape.Volume(), WithinAbs(removed, 1e-6));
  const auto floor = NamedFace(cut, "groove_floor");
  REQUIRE(floor);
  CHECK_THAT(floor->radius, WithinAbs(49.5 / 2.0, 1e-9));
  CHECK_THAT(NamedFace(cut, "groove_flank_a")->centroid.z,
             WithinAbs(12.0, 1e-9));
}

TEST_CASE("A retaining ring is placed and counted as a bought part",
          "[actions][parts][retaining_ring]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  std::string error;
  // Not on a part tab.
  CHECK(ui::AddRetainingRing(state, "din471", 25, {}, {1, 0, 0, 0}, &error) ==
        0);
  CHECK_THAT(error, ContainsSubstring("ASSEMBLY"));
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  const size_t tabs = state.parts.size();
  // Refused before a tab is made for it.
  CHECK(ui::AddRetainingRing(state, "din471", 43, {}, {1, 0, 0, 0}, &error) ==
        0);
  CHECK_THAT(error, ContainsSubstring("42 or 44"));
  CHECK(ui::AddRetainingRing(state, "din6799", 25, {}, {1, 0, 0, 0}, &error) ==
        0);
  CHECK_THAT(error, ContainsSubstring("din471"));
  CHECK(state.parts.size() == tabs);

  REQUIRE(ui::AddRetainingRing(state, "din471", 25, {}, {1, 0, 0, 0}, &error) !=
          0);
  REQUIRE(ui::AddRetainingRing(state, "din471", 25, {0, 0, 10}, {1, 0, 0, 0},
                               &error) != 0);
  REQUIRE(ui::AddRetainingRing(state, "din472", 47, {0, 0, 20}, {1, 0, 0, 0},
                               &error) != 0);
  const ui::BomTable bom = ui::ComputeBom(state);
  REQUIRE(bom.rows.size() == 2);
  for (const ui::BomRow& row : bom.rows) {
    INFO(row.partName);
    CHECK(row.bought());
    if (row.standard == "din471") {
      CHECK(row.designation == "25x1.2");
      CHECK(row.partName == "DIN 471 25x1.2");
      CHECK(row.quantity == 2);
    } else {
      CHECK(row.standard == "din472");
      CHECK(row.designation == "47x1.75");
      CHECK(row.quantity == 1);
    }
  }
}