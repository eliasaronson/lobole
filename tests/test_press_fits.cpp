// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Press fits (docs/tolerances/README.md#press-fits): an overlap two size
// bands allow is relieved by the contact check, exactly up to the fit's
// maximum interference; past it, with no band, with a band on the wrong
// side, a clearance fit or two faces off one axis, it is still a clash and
// the answer says why. Checked through Measure::ContactBetween — the one
// boolean every contact check (interference, the sweeps, contact_report,
// exact motion) takes — on real part documents whose bands the Document
// stamps from its own tolerances, print process and bearing.
#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/MotionStudy.h"
#include "core/PrintAllowance.h"
#include "core/features/BearingFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/io/Serialization.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <numbers>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

// The one cylindrical face of `doc`'s body of diameter `dia`.
ElementRef RoundFace(const Document& doc, double dia) {
  const Body& body = doc.GetBodies().front();
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder" && std::abs(2.0 * f->radius - dia) < 1e-9)
      return ElementRef{body.createdBy, name};
  }
  return {};
}

// A 30 x 30 x `length` block with a bore of `bore` through it along Z,
// centred on the origin.
std::unique_ptr<Document> Housing(double bore, double length = 10.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::dvec3(-15, -15, 0),
                                               30.0, 30.0, length));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), bore / 2.0,
      length + 2.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Bore", doc->GetFeature(0)->GetId(), doc->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(doc->Regenerate());
  return doc;
}

// A pin of `dia`, 20 long, standing from z = -5 along Z.
std::unique_ptr<Document> Pin(double dia) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Pin", glm::dvec3(0, 0, -5), glm::dvec3(0, 0, 1), dia / 2.0, 20.0));
  REQUIRE(doc->Regenerate());
  return doc;
}

void Tolerate(Document& doc, double dia, const std::string& cls) {
  Tolerance t;
  t.ref = RoundFace(doc, dia);
  REQUIRE(t.ref.IsSet());
  t.isoClass = cls;
  doc.SetTolerance(t);
}

void Deviate(Document& doc, double dia, double lower, double upper) {
  Tolerance t;
  t.ref = RoundFace(doc, dia);
  REQUIRE(t.ref.IsSet());
  t.lower = lower;
  t.upper = upper;
  doc.SetTolerance(t);
}

Measure::SetContact Contact(const Document& a, const Document& b,
                            const glm::dmat4& xfB = glm::dmat4(1.0)) {
  const auto c = Measure::ContactBetween(a.GetBodies(), glm::dmat4(1.0),
                                         b.GetBodies(), xfB, false);
  REQUIRE(c);
  return *c;
}

// The annulus between two diameters over a length.
double Annulus(double outer, double inner, double length) {
  return std::numbers::pi / 4.0 * (outer * outer - inner * inner) * length;
}

} // namespace

TEST_CASE("A dowel pin at the top of its interference fit is a press fit",
          "[measure][tolerances][press_fit]") {
  // Ø8 H7/p6: the bore 0/+15 um, the pin +15/+24, so at most 24 um of
  // interference. A pin modelled Ø8.024 in a Ø8 bore is the fit's tightest
  // pair, drawn as made.
  auto housing = Housing(8.0);
  auto pin = Pin(8.024);
  Tolerate(*housing, 8.0, "H7");
  Tolerate(*pin, 8.024, "p6");
  REQUIRE(housing->GetBodies().front().fitBands.size() == 1);
  REQUIRE(pin->GetBodies().front().fitBands.size() == 1);

  const Measure::SetContact c = Contact(*housing, *pin);
  INFO(c.fitsRefused.size());
  CHECK(c.volume < 1e-9);
  CHECK_FALSE(c.overlaps);
  REQUIRE(c.pressFits.size() == 1);
  const auto& fit = c.pressFits.front();
  CHECK(fit.fit == "Ø8 H7 / p6");
  CHECK_THAT(fit.diameter, WithinAbs(8.0, 1e-12));
  CHECK_THAT(fit.interference, WithinAbs(0.024, 1e-9));
  CHECK_THAT(fit.maxInterference, WithinAbs(0.024, 1e-9));
  CHECK_THAT(fit.length, WithinAbs(10.0, 1e-9));
  // Exactly the annulus between the two over the block's 10 mm.
  CHECK_THAT(fit.volume, WithinAbs(Annulus(8.024, 8.0, 10.0), 1e-6));
  CHECK_THAT(c.pressFitVolume, WithinAbs(fit.volume, 1e-12));
  CHECK(c.fitsRefused.empty());

  // Whichever side is asked first.
  const Measure::SetContact reversed = Contact(*pin, *housing);
  CHECK(reversed.volume < 1e-9);
  CHECK(reversed.pressFits.size() == 1);

  // The whole-set volume answer agrees.
  const auto v = Measure::InterferenceVolume(
      housing->GetBodies(), glm::dmat4(1.0), pin->GetBodies(), glm::dmat4(1.0));
  REQUIRE(v);
  CHECK(*v < 1e-9);

  // Placed anywhere, the same fit: both parts turned and moved together.
  const glm::dmat4 xf =
      glm::translate(glm::dmat4(1.0), glm::dvec3(40.0, -7.0, 3.0)) *
      glm::rotate(glm::dmat4(1.0), 0.7, glm::normalize(glm::dvec3(1, 2, 3)));
  const auto moved = Measure::ContactBetween(housing->GetBodies(), xf,
                                             pin->GetBodies(), xf, false);
  REQUIRE(moved);
  CHECK(moved->volume < 1e-9);
  CHECK(moved->pressFits.size() == 1);
}

TEST_CASE("A pin past its band is a clash naming the excess",
          "[measure][tolerances][press_fit]") {
  // 0.01 mm larger than p6 allows over an H7 bore.
  auto housing = Housing(8.0);
  auto pin = Pin(8.034);
  Tolerate(*housing, 8.0, "H7");
  Tolerate(*pin, 8.034, "p6");
  const Measure::SetContact c = Contact(*housing, *pin);
  CHECK(c.pressFits.empty());
  // Nothing relieved: the whole annulus is the clash.
  CHECK_THAT(c.volume, WithinAbs(Annulus(8.034, 8.0, 10.0), 1e-6));
  REQUIRE(c.fitsRefused.size() == 1);
  const auto& why = c.fitsRefused.front();
  CHECK_THAT(why.excess, WithinAbs(0.010, 1e-9));
  CHECK_THAT(why.interference, WithinAbs(0.034, 1e-9));
  CHECK_THAT(why.maxInterference, WithinAbs(0.024, 1e-9));
  CHECK_THAT(why.reason, ContainsSubstring("past the band"));
  CHECK_THAT(why.reason, ContainsSubstring("0.0100 mm too much"));
  CHECK_THAT(why.fit, ContainsSubstring("H7 / p6"));
}

TEST_CASE("An overlap with no fit behind it is a clash",
          "[measure][tolerances][press_fit]") {
  auto housing = Housing(8.0);
  auto pin = Pin(8.024);

  // Neither face toleranced: a plain clash, and nothing said about fits.
  Measure::SetContact c = Contact(*housing, *pin);
  CHECK_THAT(c.volume, WithinAbs(Annulus(8.024, 8.0, 10.0), 1e-6));
  CHECK(c.pressFits.empty());
  CHECK(c.fitsRefused.empty());

  // The bore toleranced, the pin not: still a clash, and it says which side
  // carries no band.
  Tolerate(*housing, 8.0, "H7");
  c = Contact(*housing, *pin);
  CHECK(c.volume > 1.0);
  CHECK(c.pressFits.empty());
  REQUIRE(c.fitsRefused.size() == 1);
  CHECK_THAT(c.fitsRefused.front().reason,
             ContainsSubstring("shaft (Ø8.024 modelled) carries no size band"));

  // A clearance fit allows no interference at all.
  Tolerate(*pin, 8.024, "g6");
  c = Contact(*housing, *pin);
  CHECK(c.volume > 1.0);
  REQUIRE(c.fitsRefused.size() == 1);
  CHECK_THAT(c.fitsRefused.front().reason,
             ContainsSubstring("clearance fit allows no interference"));

  // A hole class on the pin is a band on the wrong side.
  Tolerate(*pin, 8.024, "N7");
  c = Contact(*housing, *pin);
  CHECK(c.volume > 1.0);
  REQUIRE(c.fitsRefused.size() == 1);
  CHECK_THAT(c.fitsRefused.front().reason,
             ContainsSubstring("the shaft carries a hole band (N7)"));

  // The fit again, and then the pin a micrometre off the bore's axis: two
  // faces that are not one fit.
  Tolerate(*pin, 8.024, "p6");
  CHECK(Contact(*housing, *pin).pressFits.size() == 1);
  c = Contact(*housing, *pin,
              glm::translate(glm::dmat4(1.0), glm::dvec3(0.001, 0, 0)));
  CHECK(c.volume > 1.0);
  CHECK(c.pressFits.empty());
  REQUIRE(c.fitsRefused.size() == 1);
  CHECK_THAT(c.fitsRefused.front().reason, ContainsSubstring("not coaxial"));

  // Clearing the tolerance takes the band off the body with it.
  housing->RemoveTolerance(housing->GetTolerances().front().id);
  CHECK(housing->GetBodies().front().fitBands.empty());
  CHECK(Contact(*housing, *pin).pressFits.empty());
}

TEST_CASE("A press fit relieves only its own annulus",
          "[measure][tolerances][press_fit]") {
  // The pin's fit is fine, but a collar on it runs into the block's top face
  // (the block is z 0..10, the collar 9..11): the fit is relieved and the
  // collar's overlap is still a clash.
  auto housing = Housing(8.0);
  auto pin = std::make_unique<Document>();
  pin->AddFeature(std::make_unique<CylinderFeature>(
      "Pin", glm::dvec3(0, 0, -5), glm::dvec3(0, 0, 1), 4.012, 14.0));
  pin->AddFeature(std::make_unique<CylinderFeature>(
      "Collar", glm::dvec3(0, 0, 9), glm::dvec3(0, 0, 1), 6.0, 2.0));
  pin->AddFeature(std::make_unique<BooleanFeature>(
      "Join", pin->GetFeature(0)->GetId(), pin->GetFeature(1)->GetId(),
      BooleanOp::Union));
  REQUIRE(pin->Regenerate());
  Tolerate(*housing, 8.0, "H7");
  Tolerate(*pin, 8.024, "p6");
  const Measure::SetContact c = Contact(*housing, *pin);
  REQUIRE(c.pressFits.size() == 1);
  CHECK_THAT(c.pressFits.front().length, WithinAbs(9.0, 1e-9));
  // The collar's 1 mm inside the block, around the bore — less the hair of
  // it inside the fit's own annulus just past the pin's end, which the fit
  // takes as its chamfer's (10 x the 12 um radial interference).
  CHECK_THAT(c.volume, WithinAbs(Annulus(12.0, 8.0, 1.0), 0.05));
  CHECK(c.overlaps);
}

TEST_CASE("A printed bore is read at the size it prints",
          "[measure][tolerances][press_fit]") {
  // PETG prints a bore 0.2 under its model (docs/tolerances/README.md
  // #printed-parts): a bore modelled Ø7.98 prints Ø7.78, so a steel Ø8.024
  // pin presses 0.244 into it, not the 0.044 the model shows.
  auto housing = Housing(7.98);
  housing->SetPrintProcess(printfit::Process{"fdm", "petg"});
  auto pin = Pin(8.024);
  Tolerate(*pin, 8.024, "p6");
  // Explicit deviations wide enough for a print: the fit allows 0.324.
  Deviate(*housing, 7.98, -0.3, 0.3);
  REQUIRE(housing->GetBodies().front().fitBands.size() == 1);
  CHECK_THAT(housing->GetBodies().front().fitBands.front().allowance,
             WithinAbs(0.2, 1e-12));
  Measure::SetContact c = Contact(*housing, *pin);
  CHECK(c.volume < 1e-9);
  REQUIRE(c.pressFits.size() == 1);
  CHECK_THAT(c.pressFits.front().interference, WithinAbs(0.244, 1e-9));
  CHECK_THAT(c.pressFits.front().maxInterference, WithinAbs(0.324, 1e-9));
  CHECK_THAT(c.pressFits.front().fit, ContainsSubstring("printed (fdm petg)"));

  // An H7 bore cannot take it: the same model is 0.22 past the band.
  Tolerate(*housing, 7.98, "H7");
  c = Contact(*housing, *pin);
  CHECK(c.volume > 1.0);
  REQUIRE(c.fitsRefused.size() == 1);
  CHECK_THAT(c.fitsRefused.front().excess, WithinAbs(0.244 - 0.024, 1e-9));
}

TEST_CASE("A bearing at its K7 seat's interference is a press fit",
          "[measure][tolerances][press_fit][bearing]") {
  // A 6005's outside is Ø47, 0/-11 um (ISO 492 normal); a Ø47 K7 housing is
  // +7/-18 um. At most 18 um of interference: the housing modelled at its
  // smallest, Ø46.982, against the ring at its largest.
  Document bearing;
  bearing.AddFeature(std::make_unique<BearingFeature>("B", "6005"));
  REQUIRE(bearing.GetBodies().size() == 1);
  // Its two ring faces carry their bands with no tolerance set.
  CHECK(bearing.GetBodies().front().fitBands.size() == 2);
  // An 80 mm block, 12 thick like the bearing, bored Ø46.982 through.
  auto housing = std::make_unique<Document>();
  housing->AddFeature(std::make_unique<BoxFeature>(
      "Block", glm::dvec3(-40, -40, 0), 80.0, 80.0, 12.0));
  housing->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 46.982 / 2.0, 14.0));
  housing->AddFeature(std::make_unique<BooleanFeature>(
      "Bore", housing->GetFeature(0)->GetId(), housing->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(housing->Regenerate());
  Tolerate(*housing, 46.982, "K7");

  const Measure::SetContact c = Contact(*housing, bearing);
  CHECK(c.volume < 1e-9);
  REQUIRE(c.pressFits.size() == 1);
  CHECK(c.pressFits.front().fit == "Ø47 K7 / 6005 outside (ISO 492 normal)");
  CHECK_THAT(c.pressFits.front().interference, WithinAbs(0.018, 1e-9));
  CHECK_THAT(c.pressFits.front().maxInterference, WithinAbs(0.018, 1e-9));

  // An H7 seat is a clearance fit for the same ring: the overlap is a clash.
  Tolerate(*housing, 46.982, "H7");
  const Measure::SetContact h7 = Contact(*housing, bearing);
  CHECK(h7.volume > 1.0);
  REQUIRE(h7.fitsRefused.size() == 1);
  CHECK_THAT(h7.fitsRefused.front().reason, ContainsSubstring("clearance"));
}

TEST_CASE("A relieved fit leaves no refusal from faces that only share its "
          "axis",
          "[measure][tolerances][press_fit][bearing]") {
  // The pan/tilt head's base plate: a round plate, its outside coaxial with
  // the 6005 pressed into its K7 pocket. The bearing's banded bore and the
  // plate's unbanded outside are coaxial and engaged, and the overlap lies
  // between their radii, but they are not a fit: once the outer ring's fit
  // is relieved nothing is left to explain, so nothing is refused.
  Document bearing;
  bearing.AddFeature(std::make_unique<BearingFeature>("B", "6005"));
  auto plate = std::make_unique<Document>();
  plate->AddFeature(std::make_unique<CylinderFeature>(
      "Disc", glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), 36.8, 12.0));
  plate->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 46.992 / 2.0, 14.0));
  plate->AddFeature(std::make_unique<BooleanFeature>(
      "Pocket", plate->GetFeature(0)->GetId(), plate->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(plate->Regenerate());
  Tolerate(*plate, 46.992, "K7");

  const Measure::SetContact c = Contact(*plate, bearing);
  CHECK(c.volume < 1e-9);
  REQUIRE(c.pressFits.size() == 1);
  CHECK(c.pressFits.front().fit == "Ø47 K7 / 6005 outside (ISO 492 normal)");
  CHECK_THAT(c.pressFits.front().interference, WithinAbs(0.008, 1e-9));
  CHECK(c.fitsRefused.empty());

  // An H7 pocket is a clearance fit for the ring: the clash stays, and the
  // fit says why.
  Tolerate(*plate, 46.992, "H7");
  const Measure::SetContact h7 = Contact(*plate, bearing);
  CHECK(h7.volume > 1.0);
  bool clearance = false;
  for (const auto& f : h7.fitsRefused)
    clearance |= f.reason.find("clearance") != std::string::npos;
  CHECK(clearance);
}

TEST_CASE("A motion result keeps the press fits it relieved",
          "[motion][press_fit][io]") {
  motion::Result r;
  r.samples.push_back({});
  r.checkedClashes = true;
  motion::PressFit f;
  f.a = 3;
  f.b = 7;
  f.bInner = 9;
  f.aName = "Housing";
  f.bName = "ISO 15 6005";
  f.fit = "Ø47 K7 / 6005 outside (ISO 492 normal)";
  f.interference = 0.018;
  f.maxInterference = 0.018;
  f.volume = 26.5;
  r.pressFits.push_back(f);
  const motion::Result back =
      DocumentIO::MotionResultFromJson(DocumentIO::MotionResultToJson(r));
  REQUIRE(back.pressFits.size() == 1);
  CHECK(back.pressFits[0].a == 3);
  CHECK(back.pressFits[0].b == 7);
  CHECK(back.pressFits[0].aInner == 0);
  CHECK(back.pressFits[0].bInner == 9);
  CHECK(back.pressFits[0].bName == "ISO 15 6005");
  CHECK(back.pressFits[0].fit == f.fit);
  CHECK(back.pressFits[0].interference == 0.018);
  CHECK(back.pressFits[0].volume == 26.5);
}

// The FROZEN v159 golden: a 6005 in a housing bored at its K7 seat's
// interference, an arm turning beside them, and an EXACT motion study. Run
// against the parts it names, the study is clean and its result carries the
// press fit (which a saved result keeps, v159).
TEST_CASE("Frozen v159 golden: an exact motion study relieves a press fit",
          "[motion][golden][press_fit]") {
  Document bearing;
  bearing.AddFeature(std::make_unique<BearingFeature>("B", "6005"));
  Document housing;
  housing.AddFeature(std::make_unique<BoxFeature>(
      "Block", glm::dvec3(-40, -40, 0), 80.0, 80.0, 12.0));
  housing.AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 46.982 / 2.0, 14.0));
  housing.AddFeature(std::make_unique<BooleanFeature>(
      "Bore", housing.GetFeature(0)->GetId(), housing.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(housing.Regenerate());
  Tolerate(housing, 46.982, "K7");
  Document arm;
  arm.AddFeature(std::make_unique<BoxFeature>("Arm", glm::dvec3(2, -2, 30),
                                              28.0, 4.0, 4.0));
  REQUIRE(arm.Regenerate());

  const std::filesystem::path golden = std::filesystem::path(TestSourceDir()) /
                                       "tests/golden/assembly/"
                                       "v159-press-fit.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->GetMotionStudies().size() == 1);
  const assembly::PartResolver parts = [&](uint64_t id,
                                           uint64_t) -> const Document* {
    return id == 1 ? &housing : id == 2 ? &bearing : id == 3 ? &arm : nullptr;
  };
  const motion::Result res =
      motion::Run(**g, parts, nullptr, (*g)->GetMotionStudies()[0]);
  REQUIRE(res.error.empty());
  CHECK(res.samples.size() == 4);
  CHECK(res.clean);
  REQUIRE(res.pressFits.size() == 1);
  CHECK(res.pressFits[0].fit == "Ø47 K7 / 6005 outside (ISO 492 normal)");
  CHECK(((res.pressFits[0].a == 1 && res.pressFits[0].b == 2) ||
         (res.pressFits[0].a == 2 && res.pressFits[0].b == 1)));
  CHECK_THAT(res.pressFits[0].interference, WithinAbs(0.018, 1e-9));

  // An H7 seat: the same overlap is a standing clash.
  Tolerate(housing, 46.982, "H7");
  const motion::Result h7 =
      motion::Run(**g, parts, nullptr, (*g)->GetMotionStudies()[0]);
  CHECK_FALSE(h7.clean);
  CHECK(h7.pressFits.empty());
}
