// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Deep-groove ball bearings as generated standard parts
// (docs/parts/README.md#bearings): the ISO 15 table, the generated body, and
// placing, mating, fitting and checking one in an assembly.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/PrintAllowance.h"
#include "core/catalog/Catalog.h"
#include "core/features/BearingFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/FilletFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/BearingSeat.h"
#include "core/parts/BearingTable.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BearingActions.h"
#include "ui/DocumentSlots.h"
#include "ui/ToleranceActions.h"

#include <cmath>
#include <numbers>
#include <set>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

TEST_CASE("The bearing table is ISO 15's", "[parts][bearing]") {
  // Rows checked against ISO 15's boundary dimensions (and a supplier's
  // catalogue, which follows it): d x D x B, r_s min.
  struct Want {
    const char* designation;
    double d, D, B, r;
  };
  const Want wants[] = {
      {"6200", 10, 30, 9, 0.6},  {"608", 8, 22, 7, 0.3},
      {"6206", 30, 62, 16, 1.0}, {"6000", 10, 26, 8, 0.3},
      {"6006", 30, 55, 13, 1.0}, {"61804", 20, 32, 7, 0.3},
      {"61905", 25, 42, 9, 0.3}, {"6304", 20, 52, 15, 1.1},
      {"618/4", 4, 9, 2.5, 0.1}, {"623", 3, 10, 4, 0.15},
  };
  for (const Want& w : wants) {
    INFO(w.designation);
    const parts::BearingRow* row = parts::FindBearing(w.designation);
    REQUIRE(row != nullptr);
    CHECK(row->d == w.d);
    CHECK(row->D == w.D);
    CHECK(row->B == w.B);
    CHECK(row->rMin == w.r);
  }
  // Every row is a real ring, the scope is bores 3..30, and no designation
  // is listed twice.
  std::set<std::string> seen;
  for (const parts::BearingRow& r : parts::AllBearings()) {
    INFO(r.designation);
    CHECK(seen.insert(r.designation).second);
    CHECK(r.d >= 3.0);
    CHECK(r.d <= 30.0);
    CHECK(r.D > r.d);
    CHECK(r.rMin > 0.0);
    CHECK(r.rMin < r.B / 2.0);
  }
  // 6000..6006 and 6200..6206 are all there.
  for (int i = 0; i <= 6; i++) {
    CHECK(parts::FindBearing("600" + std::to_string(i)) != nullptr);
    CHECK(parts::FindBearing("620" + std::to_string(i)) != nullptr);
  }
  // What fits a Ø10 shaft: one per series.
  CHECK(parts::BearingsForBore(10.0).size() == 5);
  CHECK(parts::BearingsForOutside(30.0).size() == 2); // 61903 and 6200
}

TEST_CASE("The abutment limits are SKF's", "[parts][bearing]") {
  // Typed from SKF "Rolling bearings", PUB BU/P1 17000 EN (2018), table 1.1,
  // the "Abutment and fillet dimensions" columns of the 2Z row (the body is
  // drawn shielded): d_a min, d_a max, D_a max, r_a max.
  struct Want {
    const char* designation;
    double daMin, daMax, DaMax, raMax;
    const char* variant;
  };
  const Want wants[] = {
      {"6005", 28.2, 31.9, 43.8, 0.6, "2Z"},
      {"6002", 17, 20.4, 30, 0.3, "2Z"},
      {"608", 10, 12, 20, 0.3, "2Z"},
      {"609", 11, 14.3, 22, 0.3, "2Z"},
      {"6200", 14.2, 16.9, 25.8, 0.6, "2Z"},
      {"6306", 37, 44.5, 65, 1.0, "2Z"},
      // No 2Z listed: the sealed 2RS1 row.
      {"61804", 22, 23.6, 30, 0.3, "2RS1"},
  };
  for (const Want& w : wants) {
    INFO(w.designation);
    const parts::BearingAbutment* a = parts::AbutmentOf(w.designation);
    REQUIRE(a != nullptr);
    CHECK(a->daMin == w.daMin);
    CHECK(a->daMax == w.daMax);
    CHECK(a->DaMax == w.DaMax);
    CHECK(a->raMax == w.raMax);
    CHECK(std::string(a->variant) == w.variant);
  }
  // A limit SKF does not print is absent, not invented: no capped 618/4, so
  // no d_a max; and no row at all for the ISO 15 sizes SKF does not list.
  const parts::BearingAbutment* small = parts::AbutmentOf("618/4");
  REQUIRE(small != nullptr);
  CHECK_FALSE(parts::BearingAbutment::Has(small->daMax));
  CHECK(small->variant == nullptr);
  CHECK(small->daMin == 4.6);
  for (const char* none : {"618/3", "619/3", "605", "606"}) {
    INFO(none);
    CHECK(parts::FindBearing(none) != nullptr);
    CHECK(parts::AbutmentOf(none) == nullptr);
  }
  // Every carried row is a designation of the ISO 15 table, and its limits
  // lie between the rings: the shaft shoulder above the bore, the housing's
  // below the outside, a fillet no larger than the chamfer it clears.
  int rows = 0;
  for (const parts::BearingRow& r : parts::AllBearings()) {
    const parts::BearingAbutment* a = parts::AbutmentOf(r.designation);
    if (a == nullptr)
      continue;
    INFO(r.designation);
    rows++;
    CHECK(a->daMin > r.d);
    CHECK(a->DaMax < r.D);
    if (parts::BearingAbutment::Has(a->daMax))
      CHECK(a->daMax >= a->daMin);
    CHECK(a->raMax <= r.rMin + 1e-9);
  }
  CHECK(rows == static_cast<int>(parts::AllBearings().size()) - 4);
}

TEST_CASE("An unknown bearing is refused naming what is carried",
          "[parts][bearing]") {
  std::string why;
  CHECK(parts::FindBearing("6207", &why) == nullptr);
  CHECK_THAT(why, ContainsSubstring("6206"));
  CHECK_THAT(why, ContainsSubstring("618, 619, 60, 62, 63"));
  // A suffix is a variant, and saying which bare designation it is beats a
  // list of sixty.
  CHECK(parts::FindBearing("6200-2Z", &why) == nullptr);
  CHECK_THAT(why, ContainsSubstring("\"6200\""));
  CHECK_THAT(why, ContainsSubstring("suffix"));
  CHECK(parts::FindBearing("608ZZ", &why) == nullptr);
  CHECK_THAT(why, ContainsSubstring("\"608\""));
  // A longer number is not a suffix.
  CHECK(parts::FindBearing("62001", &why) == nullptr);
  CHECK_THAT(why, !ContainsSubstring("suffix"));

  Document doc;
  doc.AddFeature(std::make_unique<BearingFeature>("B", "6207"));
  CHECK(doc.GetBodies().empty());
  CHECK_THAT(doc.GetFeature(0)->GetError(), ContainsSubstring("6206"));
}

TEST_CASE("The table answers the smallest bearing over a bore",
          "[parts][bearing]") {
  // A generator sizes a shaft from its load (13.9 mm for the pan/tilt
  // head's 3 g moment in PETG) and asks for the first 60-series row over it.
  std::string why;
  const auto rows = parts::BearingsFrom("60", 13.9, &why);
  REQUIRE_FALSE(rows.empty());
  CHECK(std::string(rows.front()->designation) == "6002");
  CHECK(rows.front()->d == 15.0);
  CHECK(rows.front()->D == 32.0);
  CHECK(rows.front()->B == 9.0);
  for (size_t i = 1; i < rows.size(); ++i) {
    CHECK(rows[i]->d >= rows[i - 1]->d);
    CHECK(std::string(rows[i]->series) == "60");
  }
  // A bore the table has is its own row, not the next one up.
  CHECK(
      std::string(parts::BearingsFrom("60", 8.0, &why).front()->designation) ==
      "608");
  // Every series when none is named.
  CHECK(parts::BearingsFrom("", 0.0, &why).size() ==
        parts::AllBearings().size());
  // Refused: a series the table lacks, and a bore past the largest.
  why.clear();
  CHECK(parts::BearingsFrom("64", 10.0, &why).empty());
  CHECK_THAT(why, ContainsSubstring("618, 619, 60, 62, 63"));
  why.clear();
  CHECK(parts::BearingsFrom("62", 31.0, &why).empty());
  CHECK_THAT(why, ContainsSubstring("largest bore carried is 30"));
}

TEST_CASE("ISO 492 normal-class ring tolerances", "[parts][bearing]") {
  CHECK_THAT(parts::BoreTolerance(10.0)->lower, WithinAbs(-0.008, 1e-12));
  CHECK_THAT(parts::BoreTolerance(20.0)->lower, WithinAbs(-0.010, 1e-12));
  CHECK_THAT(parts::BoreTolerance(30.0)->lower, WithinAbs(-0.010, 1e-12));
  CHECK_THAT(parts::OutsideTolerance(30.0)->lower, WithinAbs(-0.009, 1e-12));
  CHECK_THAT(parts::OutsideTolerance(62.0)->lower, WithinAbs(-0.013, 1e-12));
  CHECK(parts::BoreTolerance(10.0)->upper == 0.0);
  // Every row has a band.
  for (const parts::BearingRow& r : parts::AllBearings()) {
    INFO(r.designation);
    CHECK(parts::BoreTolerance(r.d));
    CHECK(parts::OutsideTolerance(r.D));
  }
}

namespace {

// Face `role` of the generated bearing body.
const Measure::FaceInfo FaceOf(const Body& body, const BearingFeature& f,
                               const char* role) {
  const auto index = body.elementMap.Resolve(f.FaceName(role));
  REQUIRE(index);
  const auto info = Measure::DescribeFaceAt(body, *index);
  REQUIRE(info);
  return *info;
}

} // namespace

TEST_CASE("A generated bearing has the table's dimensions and named faces",
          "[parts][bearing]") {
  for (const char* designation : {"6200", "608", "6206", "618/4"}) {
    INFO(designation);
    const parts::BearingRow& row = *parts::FindBearing(designation);
    Document doc;
    doc.AddFeature(std::make_unique<BearingFeature>("B", designation));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies().front();
    const auto& f = static_cast<const BearingFeature&>(*doc.GetFeature(0));
    CHECK(body.material == "steel");

    const auto bore = FaceOf(body, f, BearingFeature::kBore);
    CHECK(bore.kind == "cylinder");
    CHECK_THAT(bore.radius * 2.0, WithinAbs(row.d, 1e-9));
    const auto outside = FaceOf(body, f, BearingFeature::kOutside);
    CHECK_THAT(outside.radius * 2.0, WithinAbs(row.D, 1e-9));
    // Side A on z = 0, side B on z = B, for both rings.
    CHECK_THAT(FaceOf(body, f, BearingFeature::kInnerSideA).centroid.z,
               WithinAbs(0.0, 1e-9));
    CHECK_THAT(FaceOf(body, f, BearingFeature::kOuterSideA).centroid.z,
               WithinAbs(0.0, 1e-9));
    CHECK_THAT(FaceOf(body, f, BearingFeature::kInnerSideB).centroid.z,
               WithinAbs(row.B, 1e-9));
    CHECK_THAT(FaceOf(body, f, BearingFeature::kOuterSideB).centroid.z,
               WithinAbs(row.B, 1e-9));
    CHECK(FaceOf(body, f, BearingFeature::kShieldA).kind == "plane");
    CHECK(FaceOf(body, f, BearingFeature::kShieldB).kind == "plane");

    // The volume, by Pappus from the section the feature draws (lands 0.3 of
    // the section height, shields recessed 0.05 B, a ball space of 0.6
    // min(section, B)) — the OCC revolve and boolean against arithmetic.
    const double ri0 = row.d / 2, ro0 = row.D / 2, B = row.B, c = row.rMin;
    const double h = ro0 - ri0, ri = ri0 + 0.3 * h, ro = ro0 - 0.3 * h;
    const double e = 0.05 * B;
    const std::vector<std::pair<double, double>> pts = {
        {ri0 + c, 0}, {ri, 0},      {ri, e},      {ro, e},
        {ro, 0},      {ro0 - c, 0}, {ro0, c},     {ro0, B - c},
        {ro0 - c, B}, {ro, B},      {ro, B - e},  {ri, B - e},
        {ri, B},      {ri0 + c, B}, {ri0, B - c}, {ri0, c}};
    double area = 0.0, moment = 0.0;
    for (size_t i = 0; i < pts.size(); i++) {
      const auto [x0, y0] = pts[i];
      const auto [x1, y1] = pts[(i + 1) % pts.size()];
      const double cross = x0 * y1 - x1 * y0;
      area += cross / 2.0;
      moment += (x0 + x1) * cross / 6.0;
    }
    const double pi = std::numbers::pi;
    const double ring = 2.0 * pi * std::abs(moment);
    const double a = 0.6 * std::min(h, B) / 2.0;
    const double torus = 2.0 * pi * pi * ((ri0 + ro0) / 2.0) * a * a;
    CHECK_THAT(body.shape.Volume(), WithinAbs(ring - torus, 1e-3));
    (void)area;
  }
  // The masses the header quotes, for a steel bearing (7.85 g/cm3).
  Document d608;
  d608.AddFeature(std::make_unique<BearingFeature>("B", "608"));
  CHECK_THAT(d608.GetBodies().front().shape.Volume() * 7.85e-3,
             WithinAbs(12.2, 0.1));
  Document d6200;
  d6200.AddFeature(std::make_unique<BearingFeature>("B", "6200"));
  CHECK_THAT(d6200.GetBodies().front().shape.Volume() * 7.85e-3,
             WithinAbs(31.0, 0.1));
}

TEST_CASE("A bearing feature round-trips", "[parts][bearing]") {
  Document doc;
  doc.AddFeature(std::make_unique<BearingFeature>("B", "6202"));
  const nlohmann::json j = DocumentIO::FeatureToJson(*doc.GetFeature(0));
  CHECK(j.at("type") == "Bearing");
  CHECK(j.at("designation") == "6202");
  Document reloaded;
  reloaded.AddFeature(DocumentIO::FeatureFromJson(j));
  REQUIRE(reloaded.GetBodies().size() == 1);
  CHECK_THAT(reloaded.GetBodies()[0].shape.Volume(),
             WithinAbs(doc.GetBodies()[0].shape.Volume(), 1e-9));
}

namespace {

struct Rig {
  ui::AppState state;
  size_t shaftTab = 0;
  uint64_t shaftOcc = 0;
  ElementRef journal; // the Ø10 cylinder
  ElementRef rim;     // where it meets the shoulder
  uint64_t shaftFeature = 0;
};

// A stepped shaft: a Ø20 x 10 collar at z 0..10 and a Ø10 x 40 journal on
// it (other radii when given), optionally with a fillet of `fillet` where
// they meet. Placed once in an assembly "Rig", which is left active.
void BuildRig(Rig& rig, double fillet = 0.0, double collarRadius = 10.0,
              double journalRadius = 5.0) {
  ui::AppState& state = rig.state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  rig.shaftTab = ui::AddPart(state, "Shaft");
  ui::SetActivePart(state, rig.shaftTab);
  Document& doc = *state.document;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Collar", glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), collarRadius, 10.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::dvec3(0, 0, 10), glm::dvec3(0, 0, 1), journalRadius,
      40.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Join", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Union));
  REQUIRE(doc.Regenerate());
  const auto findRim = [&]() {
    const Body& body = doc.GetBodies().front();
    for (const auto& [index, mapped] : body.elementMap.edgeNames) {
      const auto edge = Measure::DescribeEdgeAt(body, index);
      if (edge && edge->kind == "circle" &&
          std::abs(edge->radius - journalRadius) < 1e-6 &&
          std::abs(edge->midpoint.z - 10.0) < 1e-6)
        return ElementRef{body.createdBy, mapped};
    }
    return ElementRef{};
  };
  if (fillet > 0.0) {
    const ElementRef edge = findRim();
    REQUIRE(edge.IsSet());
    doc.AddFeature(std::make_unique<FilletFeature>(
        "Fillet", std::vector<ElementRef>{edge}, fillet));
    REQUIRE(doc.Regenerate());
  }
  const Body& body = doc.GetBodies().front();
  rig.shaftFeature = body.createdBy;
  for (const auto& [index, mapped] : body.elementMap.names) {
    const auto face = Measure::DescribeFaceAt(body, index);
    if (face && face->kind == "cylinder" &&
        std::abs(face->radius - journalRadius) < 1e-6)
      rig.journal = ElementRef{body.createdBy, mapped};
  }
  REQUIRE(rig.journal.IsSet());
  if (fillet == 0.0) {
    rig.rim = findRim();
    REQUIRE(rig.rim.IsSet());
  }
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  Occurrence occ;
  occ.partId = state.parts[rig.shaftTab].id;
  occ.name = "Shaft";
  rig.shaftOcc = state.assembly->AddOccurrence(occ);
}

} // namespace

TEST_CASE("A bearing is placed on a picked journal and mated there",
          "[actions][parts][bearing]") {
  Rig rig;
  BuildRig(rig);
  ui::AppState& state = rig.state;

  // A Ø10 journal takes five bearings, and none is picked for you.
  ui::BearingRequest dry;
  dry.dryRun = true;
  ui::BearingFit fit =
      ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, dry);
  CHECK_FALSE(fit.ok);
  CHECK_THAT(fit.why, ContainsSubstring("61800"));
  CHECK_THAT(fit.why, ContainsSubstring("6200 (10x30x9)"));
  CHECK_THAT(fit.why, ContainsSubstring("Say which"));
  CHECK(fit.candidates.size() == 5);

  // A series narrows it to one.
  dry.series = "62";
  fit = ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, dry);
  REQUIRE(fit.ok);
  CHECK(fit.designation == "6200");
  CHECK(fit.shaft);
  CHECK(fit.shoulder);
  // Side A on the shoulder plane, the axis up the journal.
  CHECK_THAT(fit.position.z, WithinAbs(10.0, 1e-5));
  CHECK_THAT((fit.orientation * glm::dvec3(0, 0, 1)).z, WithinAbs(1.0, 1e-5));
  CHECK_THAT(fit.summary, ContainsSubstring("Ø20 shoulder"));
  CHECK(state.assembly->GetOccurrences().size() ==
        1); // a dry run places nothing

  // A designation of another size is refused naming the ones that fit.
  dry.series.clear();
  dry.designation = "6201";
  fit = ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, dry);
  CHECK_FALSE(fit.ok);
  CHECK_THAT(fit.why, ContainsSubstring("Ø12 bore"));
  CHECK_THAT(fit.why, ContainsSubstring("6000 (10x26x8)"));

  // The rim says the same thing the face did.
  dry.designation = "6200";
  fit = ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.rim}, dry);
  REQUIRE(fit.ok);
  CHECK_THAT(fit.position.z, WithinAbs(10.0, 1e-5));

  // Placed: an occurrence of "ISO 15 6200", fastened to the journal by its
  // bore, and the solve moves nothing.
  ui::BearingRequest place;
  place.designation = "6200";
  fit = ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
  REQUIRE(fit.ok);
  REQUIRE(fit.occurrence != 0);
  REQUIRE(fit.mate != 0);
  const Occurrence* bearing = state.assembly->FindById(fit.occurrence);
  REQUIRE(bearing != nullptr);
  CHECK(bearing->name == "ISO 15 6200");
  const Mate* mate = nullptr;
  for (const Mate& m : state.assembly->GetMates())
    if (m.id == fit.mate)
      mate = &m;
  REQUIRE(mate != nullptr);
  CHECK(mate->type == MateType::Fastened);
  CHECK(mate->a.anchor.face.name == rig.journal.name);
  CHECK(BearingFeature::RoleOf(mate->b.anchor.face.name) == "bore");
  const glm::dvec3 before = bearing->position;
  CHECK(ui::SolveActiveAssembly(state) == 0);
  bearing = state.assembly->FindById(fit.occurrence);
  CHECK(glm::length(bearing->position - before) < 1e-4);

  // The generated part is filed with the standard parts.
  bool filed = false;
  for (const ui::PartSlot& slot : state.parts)
    if (slot.name == "ISO 15 6200")
      filed = slot.folder == ui::kStandardPartsFolder;
  CHECK(filed);
}

TEST_CASE("A bearing is refused on a part tab and by an unknown name",
          "[actions][parts][bearing]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  std::string error;
  CHECK(ui::AddBearing(state, "6200", {}, {1, 0, 0, 0}, &error) == 0);
  CHECK_THAT(error, ContainsSubstring("ASSEMBLY"));
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  const size_t tabs = state.parts.size();
  CHECK(ui::AddBearing(state, "6200-2RS", {}, {1, 0, 0, 0}, &error) == 0);
  CHECK_THAT(error, ContainsSubstring("\"6200\""));
  CHECK(state.parts.size() == tabs); // no tab left behind by a refusal
}

TEST_CASE("Bearings on the BOM are one row per designation",
          "[actions][parts][bearing]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  std::string error;
  REQUIRE(ui::AddBearing(state, "608", {}, {1, 0, 0, 0}, &error) != 0);
  REQUIRE(ui::AddBearing(state, "608", {0, 0, 20}, {1, 0, 0, 0}, &error) != 0);
  REQUIRE(ui::AddBearing(state, "6200", {0, 0, 40}, {1, 0, 0, 0}, &error) != 0);
  const ui::BomTable bom = ui::ComputeBom(state);
  REQUIRE(bom.rows.size() == 2);
  for (const ui::BomRow& row : bom.rows) {
    INFO(row.partName);
    CHECK(row.standard == "iso15");
    CHECK(row.bought());
    if (row.designation == "608") {
      CHECK(row.quantity == 2);
      CHECK_THAT(row.unitMassG, WithinAbs(12.2, 0.2));
    } else {
      CHECK(row.designation == "6200");
      CHECK(row.quantity == 1);
    }
  }
}

TEST_CASE("check_fits reads a bearing seat with the ring's own band",
          "[actions][parts][bearing][tolerance]") {
  Rig rig;
  BuildRig(rig);
  ui::AppState& state = rig.state;
  ui::BearingRequest place;
  place.designation = "6200";
  const ui::BearingFit fit =
      ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
  REQUIRE(fit.ok);

  // No tolerance on the journal: unverified, and says where to put one.
  std::vector<ui::FitCheck> fits = ui::CheckFits(state);
  REQUIRE(fits.size() == 1);
  CHECK(fits[0].status == "unverified");
  CHECK_THAT(fits[0].reason, ContainsSubstring("carries no tolerance"));

  // k5 on the journal. In an H7 hole that is a transition fit; on a bearing
  // bore, made SMALL (0/-8 um), it is interference — which is why a bearing
  // seat is read with the ring's band and not as a hole class.
  Document* shaft = ui::PartDocById(state, state.parts[rig.shaftTab].id);
  REQUIRE(shaft != nullptr);
  Tolerance k5;
  k5.ref = rig.journal;
  k5.isoClass = "k5";
  shaft->SetTolerance(k5);
  fits = ui::CheckFits(state);
  REQUIRE(fits.size() == 1);
  INFO(fits[0].Sentence());
  CHECK(fits[0].computed);
  CHECK(fits[0].status == "reported");
  CHECK(fits[0].kind == "interference");
  CHECK_THAT(fits[0].holeClass, ContainsSubstring("6200 bore"));
  CHECK(fits[0].shaftClass == "k5");
  CHECK_THAT(fits[0].holeMin, WithinAbs(9.992, 1e-9));
  CHECK_THAT(fits[0].holeMax, WithinAbs(10.0, 1e-9));
  // k5 at 10 mm: +1..+7 um.
  CHECK_THAT(fits[0].minClearance, WithinAbs(-0.015, 1e-9));
  CHECK_THAT(fits[0].maxClearance, WithinAbs(-0.001, 1e-9));

  // A hole class on a journal the bearing's bore goes on is refused.
  Tolerance h7 = k5;
  h7.isoClass = "H7";
  shaft->SetTolerance(h7);
  fits = ui::CheckFits(state);
  REQUIRE(fits.size() == 1);
  CHECK(fits[0].status == "unverified");
  CHECK_THAT(fits[0].reason, ContainsSubstring("needs a shaft class"));

  // A mate on a FLAT face is not a fit at all: the interface a bolt clamps,
  // a ring's side on its shoulder. It used to be listed as an unverified fit
  // ("carries no tolerance"), and on the pan/tilt head eighteen such rows
  // buried the eight bearing seats.
  const Document* shaftDoc = shaft;
  ElementRef shoulder;
  for (const auto& [index, mapped] :
       shaftDoc->GetBodies().front().elementMap.names) {
    const auto face =
        Measure::DescribeFaceAt(shaftDoc->GetBodies().front(), index);
    if (face && face->kind == "plane" &&
        std::abs(face->centroid.z - 10.0) < 1e-6)
      shoulder = ElementRef{shaftDoc->GetBodies().front().createdBy, mapped};
  }
  REQUIRE(shoulder.IsSet());
  Mate flat;
  flat.type = MateType::Fastened;
  flat.name = "Ring on shoulder";
  flat.a.occurrenceId = rig.shaftOcc;
  flat.a.anchor.face = shoulder;
  flat.b.occurrenceId = fit.occurrence;
  flat.b.anchor.face = rig.journal; // any face of the other part; a is flat
  state.assembly->AddMate(flat);
  fits = ui::CheckFits(state);
  CHECK(fits.size() == 1);
}

TEST_CASE("check_seat reads a placed bearing and the seat it is on",
          "[actions][parts][bearing]") {
  SECTION("A square shoulder") {
    // Ø16: inside SKF's d_a 14.2 to 16.9 for a 6200-2Z.
    Rig rig;
    BuildRig(rig, 0.0, /*collarRadius=*/8.0);
    ui::AppState& state = rig.state;
    ui::BearingRequest place;
    place.designation = "6200";
    const ui::BearingFit fit =
        ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
    INFO(r.dump(2));
    CHECK(r.at("d") == 10.0);
    CHECK(r.at("D") == 30.0);
    CHECK(r.at("r_min") == 0.6);
    REQUIRE(r.at("seats").size() == 1);
    CHECK(r.at("seats")[0].at("ring") == "inner");
    CHECK_THAT(r.at("seats")[0].at("shoulder_diameter").get<double>(),
               WithinAbs(16.0, 1e-6));
    const auto status = [&](const std::string& what) {
      for (const auto& c : r.at("checks"))
        if (c.at("characteristic") == what)
          return c.at("status").get<std::string>();
      return std::string("absent");
    };
    CHECK(status("shaft_diameter") == "pass");
    CHECK(status("inner_ring_seated") == "pass");
    CHECK(status("inner_fillet_radius") == "pass");
    // How far a shoulder may reach is not ISO 15's to say; SKF's abutment
    // table says it.
    CHECK(status("inner_shoulder_diameter") == "pass");
    CHECK(r.at("abutment").at("d_a_min") == 14.2);
    CHECK(r.at("abutment").at("d_a_max") == 16.9);
    CHECK(r.at("abutment").at("variant") == "2Z");
    CHECK_THAT(r.at("abutment").at("source").get<std::string>(),
               ContainsSubstring("17000"));
    // And the outer ring is in no housing yet: never a pass.
    CHECK(status("housing_diameter") == "unverified");
    CHECK(r.at("status") == "unverified");
    CHECK_THAT(r.at("measurements").at("shaft_diameter").get<double>(),
               WithinAbs(10.0, 1e-9));

    // The catalog path: the supplier's abutment limits, judged against the
    // same measurements.
    catalog::Json options = {{"id", "demo"},     {"revision", "1"},
                             {"source", "test"}, {"license", "CC0-1.0"},
                             {"units", "mm"},    {"storage", "reference"}};
    const catalog::Json entry = catalog::Preview(
        "designation,bore_diameter,outer_diameter,width,shaft_min,shaft_max,"
        "shoulder_min,shoulder_max,fillet_radius_max\n"
        "6200,10,30,9,9.99,10.01,12,15,0.6\n",
        "csv", options);
    const nlohmann::json withCatalog =
        ui::CheckSeat(state, fit.occurrence, &entry, "");
    INFO(withCatalog.dump(2));
    // The Ø16 shoulder is past this supplier's 15 mm maximum.
    CHECK(withCatalog.at("catalog").at("status") == "fail");
    CHECK(withCatalog.at("status") == "fail");

    // Moved off its shoulder, the ring is not seated.
    state.assembly->FindById(fit.occurrence)->position.z += 1.0;
    const nlohmann::json off =
        ui::CheckSeat(state, fit.occurrence, nullptr, "");
    for (const auto& c : off.at("checks"))
      if (c.at("characteristic") == "inner_ring_seated")
        CHECK(c.at("status") == "fail");
    CHECK(off.at("status") == "fail");
  }
  SECTION("A fillet bigger than the ring's chamfer") {
    Rig rig;
    BuildRig(rig, /*fillet=*/1.0);
    ui::AppState& state = rig.state;
    ui::BearingRequest place;
    place.designation = "6200";
    const ui::BearingFit fit =
        ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    // Past the fillet, on the shoulder plane.
    CHECK_THAT(fit.position.z, WithinAbs(10.0, 1e-5));
    CHECK_THAT(fit.summary, ContainsSubstring("fillet R1"));
    const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
    INFO(r.dump(2));
    bool found = false;
    for (const auto& c : r.at("checks"))
      if (c.at("characteristic") == "inner_fillet_radius") {
        found = true;
        CHECK(c.at("status") == "fail");
        CHECK_THAT(c.at("observed").get<double>(), WithinAbs(1.0, 1e-6));
        // Judged against the supplier's r_a max, not the ring's chamfer.
        CHECK(c.at("limit") == "r_a max");
        CHECK(c.at("maximum") == 0.6);
      }
    CHECK(found);
    CHECK(r.at("status") == "fail");
  }
  // SKF's abutment limits for a 6200-2Z: d_a 14.2 to 16.9 (PUB BU/P1 17000
  // EN, table 1.1). Each shoulder reaches past the ring's chamfer (Ø11.2),
  // so it is the supplier's limit that judges it, and is named.
  const auto shoulder = [](double collarRadius) {
    Rig rig;
    BuildRig(rig, 0.0, collarRadius);
    ui::BearingRequest place;
    place.designation = "6200";
    const ui::BearingFit fit = ui::FitBearing(
        rig.state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    const nlohmann::json r =
        ui::CheckSeat(rig.state, fit.occurrence, nullptr, "");
    for (const auto& c : r.at("checks"))
      if (c.at("characteristic") == "inner_shoulder_diameter")
        return c;
    return nlohmann::json();
  };
  SECTION("A shaft shoulder below d_a min leaves the ring unsupported") {
    const nlohmann::json c = shoulder(6.5); // Ø13
    INFO(c.dump(2));
    CHECK(c.at("status") == "fail");
    CHECK(c.at("limit") == "d_a min");
    CHECK(c.at("minimum") == 14.2);
    CHECK_THAT(c.at("reason").get<std::string>(), ContainsSubstring("too low"));
  }
  SECTION("A shaft shoulder above d_a max reaches the shield") {
    const nlohmann::json c = shoulder(10.0); // Ø20
    INFO(c.dump(2));
    CHECK(c.at("status") == "fail");
    CHECK(c.at("limit") == "d_a max");
    CHECK(c.at("maximum") == 16.9);
    CHECK(c.at("variant") == "6200-2Z");
    CHECK_THAT(c.at("reason").get<std::string>(),
               ContainsSubstring("too high"));
  }
  SECTION("A bearing the catalogue does not list stays unverified") {
    // ISO 15 has a 606 (6x17x6); SKF's catalogue lists none.
    Rig rig;
    BuildRig(rig, 0.0, /*collarRadius=*/5.0, /*journalRadius=*/3.0);
    ui::BearingRequest place;
    place.designation = "606";
    const ui::BearingFit fit = ui::FitBearing(
        rig.state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    const nlohmann::json r =
        ui::CheckSeat(rig.state, fit.occurrence, nullptr, "");
    INFO(r.dump(2));
    CHECK(r.at("abutment").is_null());
    bool found = false;
    for (const auto& c : r.at("checks"))
      if (c.at("characteristic") == "inner_shoulder_diameter") {
        found = true;
        CHECK(c.at("status") == "unverified");
        CHECK_THAT(c.at("reason").get<std::string>(),
                   ContainsSubstring("lists no 606"));
      }
    CHECK(found);
  }
  SECTION("Not a bearing") {
    Rig rig;
    BuildRig(rig);
    CHECK_THROWS_WITH(ui::CheckSeat(rig.state, rig.shaftOcc, nullptr, ""),
                      ContainsSubstring("not a placed bearing"));
  }
}

TEST_CASE("A bearing goes in a housing bore by its outside",
          "[actions][parts][bearing]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  const size_t tab = ui::AddPart(state, "Housing");
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  // A 20 mm plate, a Ø20 through hole and a Ø30 counterbore 9 deep from the
  // top: the seat for a 6200's outer ring, the step its shoulder.
  doc.AddFeature(std::make_unique<BoxFeature>("Plate", glm::dvec3(-30, -30, 0),
                                              60.0, 60.0, 20.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Through", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 10.0, 22.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  const uint64_t drilled = doc.GetFeatures().back()->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Counterbore", glm::dvec3(0, 0, 11), glm::dvec3(0, 0, 1), 15.0, 10.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Bore", drilled, doc.GetFeatures().back()->GetId(), BooleanOp::Subtract));
  REQUIRE(doc.Regenerate());
  const Body& body = doc.GetBodies().front();
  ElementRef seat;
  for (const auto& [index, mapped] : body.elementMap.names) {
    const auto face = Measure::DescribeFaceAt(body, index);
    if (face && face->kind == "cylinder" &&
        std::abs(face->radius - 15.0) < 1e-6)
      seat = ElementRef{body.createdBy, mapped};
  }
  REQUIRE(seat.IsSet());
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  Occurrence occ;
  occ.partId = state.parts[tab].id;
  occ.name = "Housing";
  const uint64_t housing = state.assembly->AddOccurrence(occ);

  ui::BearingRequest request;
  request.dryRun = true;
  ui::BearingFit fit =
      ui::FitBearing(state, ui::RimPick{housing, seat}, request);
  CHECK_FALSE(fit.ok);
  CHECK_THAT(fit.why, ContainsSubstring("Ø30 housing bore"));
  CHECK_THAT(fit.why, ContainsSubstring("61903"));
  request.designation = "6200";
  request.dryRun = false;
  fit = ui::FitBearing(state, ui::RimPick{housing, seat}, request);
  REQUIRE(fit.ok);
  CHECK_FALSE(fit.shaft);
  // On the step at z = 11, facing out of the bore.
  CHECK_THAT(fit.position.z, WithinAbs(11.0, 1e-5));
  CHECK_THAT((fit.orientation * glm::dvec3(0, 0, 1)).z, WithinAbs(1.0, 1e-5));
  CHECK(fit.overhang == 0.0);
  const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
  INFO(r.dump(2));
  REQUIRE(r.at("seats").size() == 1);
  CHECK(r.at("seats")[0].at("ring") == "outer");
  CHECK_THAT(r.at("seats")[0].at("shoulder_diameter").get<double>(),
             WithinAbs(20.0, 1e-6));
  for (const auto& c : r.at("checks")) {
    if (c.at("characteristic") == "housing_diameter")
      CHECK(c.at("status") == "pass");
    if (c.at("characteristic") == "outer_ring_seated")
      CHECK(c.at("status") == "pass");
    // A Ø20 step is inside SKF's D_a max of 25.8 for a 6200-2Z.
    if (c.at("characteristic") == "outer_shoulder_diameter")
      CHECK(c.at("status") == "pass");
  }

  // Opened to Ø27 under the ring, the step is past D_a max: the outer
  // ring's side face is left unsupported.
  Document* housingDoc = state.parts[tab].doc.get();
  REQUIRE(housingDoc != nullptr);
  auto* through = dynamic_cast<CylinderFeature*>(housingDoc->GetFeature(1));
  REQUIRE(through != nullptr);
  through->radius = 13.5;
  REQUIRE(housingDoc->Regenerate());
  const nlohmann::json wide = ui::CheckSeat(state, fit.occurrence, nullptr, "");
  INFO(wide.dump(2));
  bool judged = false;
  for (const auto& c : wide.at("checks"))
    if (c.at("characteristic") == "outer_shoulder_diameter") {
      judged = true;
      CHECK(c.at("status") == "fail");
      CHECK(c.at("limit") == "D_a max");
      CHECK(c.at("maximum") == 25.8);
      CHECK_THAT(c.at("observed").get<double>(), WithinAbs(27.0, 1e-6));
    }
  CHECK(judged);
  CHECK(wide.at("status") == "fail");
}

// A seat on a printed part is modelled at its size plus the process
// allowance (docs/tolerances/README.md#printed-parts): the pan/tilt head's
// PETG yoke bores for a 6002 are Ø32.3. fit_bearing refused them as "a
// Ø32.3 housing bore takes no bearing", and check_seat would have failed the
// diameter, though the part was modelled exactly as the docs say.
TEST_CASE("A printed housing is fitted and checked at what it prints",
          "[actions][parts][bearing]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  const size_t tab = ui::AddPart(state, "Printed housing");
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  const double model =
      *printfit::ForBasic({"fdm", "petg"}, printfit::Printed::Hole, 30.0,
                          nullptr)
           .transform([](const printfit::Allowance& a) { return a.modelMm; });
  REQUIRE(model > 30.0);
  doc.AddFeature(std::make_unique<BoxFeature>("Plate", glm::dvec3(-30, -30, 0),
                                              60.0, 60.0, 20.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Through", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 10.0, 22.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  const uint64_t drilled = doc.GetFeatures().back()->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Counterbore", glm::dvec3(0, 0, 11), glm::dvec3(0, 0, 1), model / 2.0,
      10.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Bore", drilled, doc.GetFeatures().back()->GetId(), BooleanOp::Subtract));
  REQUIRE(doc.Regenerate());
  const Body& body = doc.GetBodies().front();
  ElementRef seat;
  for (const auto& [index, mapped] : body.elementMap.names) {
    const auto face = Measure::DescribeFaceAt(body, index);
    if (face && face->kind == "cylinder" &&
        std::abs(face->radius - model / 2.0) < 1e-6)
      seat = ElementRef{body.createdBy, mapped};
  }
  REQUIRE(seat.IsSet());
  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  Occurrence occ;
  occ.partId = state.parts[tab].id;
  occ.name = "Housing";
  const uint64_t housing = state.assembly->AddOccurrence(occ);
  ui::BearingRequest request;
  request.designation = "6200";

  // Not printed, the same bore is a size no bearing has.
  request.dryRun = true;
  CHECK_FALSE(ui::FitBearing(state, ui::RimPick{housing, seat}, request).ok);

  // The housing's tab is stashed while the assembly is active.
  REQUIRE(state.parts[tab].doc != nullptr);
  state.parts[tab].doc->SetPrintProcess(printfit::Process{"fdm", "petg"});
  request.dryRun = false;
  const ui::BearingFit fit =
      ui::FitBearing(state, ui::RimPick{housing, seat}, request);
  INFO(fit.why);
  REQUIRE(fit.ok);
  CHECK(fit.designation == "6200");
  CHECK_THAT(fit.seatDiameter, WithinAbs(30.0, 1e-9));
  CHECK_THAT(fit.summary, ContainsSubstring("prints"));
  const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
  INFO(r.dump(2));
  bool checked = false;
  for (const auto& c : r.at("checks")) {
    if (c.at("characteristic") == "housing_diameter") {
      CHECK(c.at("status") == "pass");
      CHECK(c.at("observed") == 30.0);
      checked = true;
    }
  }
  CHECK(checked);
  CHECK_THAT(r.at("seats")[0].at("modelled_diameter").get<double>(),
             WithinAbs(model, 1e-9));
}

// Both rings (docs/parts/README.md#both-rings): a one-solid bearing in a
// mechanism is mated to ONE seat, since fastening it to the housing and
// the shaft would lock the shaft. Its other ring is still on a seat, and
// check_seat reads it through that seat (the coaxial cylinder under it)
// rather than calling it unverified. fit_bearing reports that ring, and
// mates it (revolute) only when asked.
TEST_CASE("check_seat reads the ring no mate reaches through its seat",
          "[actions][parts][bearing]") {
  Rig rig;
  BuildRig(rig);
  ui::AppState& state = rig.state;
  const int asmTab = state.activePart;
  // A plate with a Ø30 bore through it, around the journal: the 6200's
  // outer ring sits in it at z 10..19 once the plate is placed at z 5.
  const size_t tab = ui::AddPart(state, "Housing");
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  doc.AddFeature(std::make_unique<BoxFeature>("Plate", glm::dvec3(-30, -30, 0),
                                              60.0, 60.0, 20.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), 15.0, 22.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(doc.Regenerate());
  ui::SetActivePart(state, static_cast<size_t>(asmTab));
  REQUIRE(state.assembly != nullptr);
  Occurrence occ;
  occ.partId = state.parts[tab].id;
  occ.name = "Housing";
  occ.position = glm::dvec3(0, 0, 5);
  const uint64_t housing = state.assembly->AddOccurrence(occ);

  const auto status = [](const nlohmann::json& r, const std::string& what) {
    for (const auto& c : r.at("checks"))
      if (c.at("characteristic") == what)
        return c.at("status").get<std::string>();
    return std::string("absent");
  };
  ui::BearingRequest place;
  place.designation = "6200";

  SECTION("check_seat finds the housing under the outer ring") {
    const ui::BearingFit fit =
        ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    CHECK(fit.otherMate == 0); // not asked for
    INFO(fit.otherRing.dump(2));
    REQUIRE(fit.otherRing.contains("seat"));
    CHECK(fit.otherRing.at("seat").at("found") == "coaxial");
    CHECK(fit.otherRing.at("diameter_check").at("status") == "pass");

    const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
    INFO(r.dump(2));
    REQUIRE(r.at("seats").size() == 2);
    const nlohmann::json* outer = nullptr;
    for (const auto& s : r.at("seats"))
      if (s.at("ring") == "outer")
        outer = &s;
    REQUIRE(outer != nullptr);
    CHECK(outer->at("found") == "coaxial");
    CHECK(outer->at("mate").is_null());
    CHECK(outer->at("occurrence") == housing);
    CHECK(status(r, "shaft_diameter") == "pass");
    CHECK(status(r, "housing_diameter") == "pass");
    // A plain through bore stops nothing: said, not passed.
    CHECK(status(r, "outer_ring_located") == "unverified");

    // A bore of another size under the ring FAILS the diameter, rather than
    // being passed over as "no seat".
    Document* housingDoc = state.parts[tab].doc.get();
    REQUIRE(housingDoc != nullptr);
    auto* bore = dynamic_cast<CylinderFeature*>(housingDoc->GetFeature(1));
    REQUIRE(bore != nullptr);
    bore->radius = 15.05;
    REQUIRE(housingDoc->Regenerate());
    const nlohmann::json wide =
        ui::CheckSeat(state, fit.occurrence, nullptr, "");
    INFO(wide.dump(2));
    CHECK(status(wide, "housing_diameter") == "fail");
  }
  SECTION("fit_bearing mates the other ring when asked, and moves nothing") {
    place.mateOther = true;
    const ui::BearingFit fit =
        ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    INFO(fit.summary);
    REQUIRE(fit.otherMate != 0);
    const Mate* mate = nullptr;
    for (const Mate& m : state.assembly->GetMates())
      if (m.id == fit.otherMate)
        mate = &m;
    REQUIRE(mate != nullptr);
    CHECK(mate->type == MateType::Revolute);
    CHECK(mate->a.occurrenceId == housing);
    CHECK(BearingFeature::RoleOf(mate->b.anchor.face.name) == "outside");
    const glm::dvec3 before =
        state.assembly->FindById(fit.occurrence)->position;
    const glm::dvec3 housingBefore =
        state.assembly->FindById(housing)->position;
    CHECK(ui::SolveActiveAssembly(state) == 0);
    CHECK(glm::length(state.assembly->FindById(fit.occurrence)->position -
                      before) < 1e-4);
    CHECK(glm::length(state.assembly->FindById(housing)->position -
                      housingBefore) < 1e-4);
    const nlohmann::json r = ui::CheckSeat(state, fit.occurrence, nullptr, "");
    INFO(r.dump(2));
    for (const auto& s : r.at("seats"))
      if (s.at("ring") == "outer")
        CHECK(s.at("mate") == fit.otherMate); // read through the mate now
    CHECK(status(r, "housing_diameter") == "pass");
  }
  SECTION("asked to mate a ring nothing sits under, it says so") {
    REQUIRE(state.assembly->RemoveOccurrence(housing));
    place.mateOther = true;
    const ui::BearingFit fit =
        ui::FitBearing(state, ui::RimPick{rig.shaftOcc, rig.journal}, place);
    REQUIRE(fit.ok);
    CHECK(fit.otherMate == 0);
    CHECK_FALSE(fit.otherRing.contains("seat"));
    CHECK_THAT(fit.summary, ContainsSubstring("outer ring is NOT mated"));
  }
}
