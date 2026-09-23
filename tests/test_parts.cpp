#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/RuntimePaths.h"
#include "core/Document.h"
#include "core/features/FastenerFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/AxisStack.h"
#include "core/parts/FastenerTable.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

TEST_CASE("Axis traversal keeps material when the ray has one crossing",
          "[parts][axis]") {
  Body body;
  body.shape = CADShape(BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 20), 10, 10, 20));
  const std::vector<parts::PlacedBody> placed{{&body, glm::mat4(1), "plate"}};
  SECTION("The search limit is inside material") {
    const auto spans = parts::TraverseAxis(placed, {0, 0, 0}, {0, 0, 1}, 0, 30);
    REQUIRE(spans.size() == 1);
    CHECK_THAT(spans[0].enter, WithinAbs(20, 1e-6));
    CHECK_THAT(spans[0].exit, WithinAbs(30, 1e-6));
    CHECK(parts::JointEnd(spans, spans, 0).blind);
  }
  SECTION("A joint that ends before the obstruction is not blind") {
    // The far-end pick says where the joint stops. Reading `blind` off the
    // axis alone refused a nut on every through-bolt whose line happened to
    // carry on into another part — a quad's arm bolt with its nyloc on the
    // mid plate, refused because the top plate is 15 mm further up.
    const auto spans = parts::TraverseAxis(placed, {0, 0, 0}, {0, 0, 1}, 0, 30);
    REQUIRE(spans.size() == 1);
    const parts::JointExtent stops = parts::JointEnd(spans, spans, 12.0);
    CHECK_FALSE(stops.blind);
    CHECK_THAT(stops.end, WithinAbs(12.0, 1e-6));
    // ... but a bound PAST it does not reach past the material either.
    const parts::JointExtent past = parts::JointEnd(spans, spans, 40.0);
    CHECK(past.blind);
    CHECK_THAT(past.end, WithinAbs(20.0, 1e-6));
  }
  SECTION("The ray starts inside material") {
    const auto spans =
        parts::TraverseAxis(placed, {0, 0, 25}, {0, 0, 1}, 0, 50);
    REQUIRE(spans.size() == 1);
    CHECK_THAT(spans[0].enter, WithinAbs(0, 1e-6));
    CHECK_THAT(spans[0].exit, WithinAbs(15, 1e-6));
  }
}

// The vendored standard-parts tables (docs/parts/README.md#the-data).
//
// What these check is not the parser. It is that the DATA says what ISO says,
// because the vendored row is the specification a generated screw will be
// built from and checked against — the geometry is a means to it. A parser
// that reads a table with the wrong numbers in it passes every structural
// test and produces a screw that cannot be bought.
namespace {

// A table written to a temp dir, for the malformed cases. Real files cannot
// exercise them, and hand-editing the vendored ones would defeat the point of
// vendoring them unmodified.
struct TempTables {
  fs::path dir;
  TempTables() {
    // Unique per instance: two cases in one binary must not share a dir, and
    // `this` is the handiest unique number that needs no header.
    dir =
        fs::temp_directory_path() /
        ("designer_parts_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(dir);
    parts::SetDataDirForTesting(dir.string());
  }
  ~TempTables() {
    parts::SetDataDirForTesting("");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void Write(const std::string& name, const std::string& text) const {
    std::ofstream(dir / (name + ".csv")) << text;
  }
};

// The vendored tables, read from the SOURCE tree.
//
// `RuntimePaths::DataDir()` looks beside the running executable, and this
// binary lives in build/tests/ rather than beside `designer` — so it would
// resolve to build/share/designer/parts and find nothing. test_ibl.cpp reaches
// the HDRI the same way, for the same reason. Staging is checked separately
// below, because "the content is right" and "the build put it where the app
// looks" are different claims and only one of them is about the data.
struct SourceTables {
  SourceTables() {
    parts::SetDataDirForTesting(std::string(DESIGNER_SOURCE_DIR) +
                                "/assets/parts");
  }
  ~SourceTables() { parts::SetDataDirForTesting(""); }
};

} // namespace

TEST_CASE("parts: the vendored table is the specification", "[parts]") {
  const SourceTables tables;
  // ISO 4762 M6, read off the standard: 10 mm head diameter, 6 mm head
  // height, 5 mm hex socket, 1 mm pitch. If a future re-vendor moves a column
  // or shifts a row, this is what says so — and every generated screw is only
  // as right as these numbers.
  std::string error;
  const auto m6 = parts::Dimensions("iso4762def", "M6", &error);
  INFO(error);
  REQUIRE(m6.has_value());
  CHECK_THAT(m6->at("P"), WithinAbs(1.0, 1e-9));       // coarse pitch
  CHECK_THAT(m6->at("dk_max"), WithinAbs(10.0, 1e-9)); // head diameter
  CHECK_THAT(m6->at("k"), WithinAbs(6.0, 1e-9));       // head height
  // s_MEAN, not the nominal: ISO 4762 gives the socket a tolerance band and
  // this column is its middle, so an M6's 5 mm hex reads 5.06. Asserting 5.0
  // here failed, correctly — the column means what it says and the test did
  // not. Pinned at the real value so a re-vendor that shifts the column shows.
  CHECK_THAT(m6->at("s_mean"), WithinAbs(5.06, 1e-9));

  // And a second size, because one row agreeing could be luck: M10 is 16 mm
  // across the head, 10 high, an 8 mm socket, 1.5 mm pitch.
  const auto m10 = parts::Dimensions("iso4762def", "M10", &error);
  REQUIRE(m10.has_value());
  CHECK_THAT(m10->at("P"), WithinAbs(1.5, 1e-9));
  CHECK_THAT(m10->at("dk_max"), WithinAbs(16.0, 1e-9));
  CHECK_THAT(m10->at("k"), WithinAbs(10.0, 1e-9));
  CHECK_THAT(m10->at("s_mean"), WithinAbs(8.07, 1e-9)); // 8 mm hex, mean

  // A nut and a washer from their own standards, so the three shipped tables
  // are each pinned: ISO 4032 M6 is 5.2 thick across 10 mm flats; ISO 7089 M6
  // is 12 mm across and 1.6 thick.
  const auto nut = parts::Dimensions("iso4032def", "M6", &error);
  REQUIRE(nut.has_value());
  CHECK_THAT(nut->at("m"), WithinAbs(5.2, 1e-9));
  CHECK_THAT(nut->at("s_nom"), WithinAbs(10.0, 1e-9));

  const auto washer = parts::Dimensions("iso7089def", "M6", &error);
  REQUIRE(washer.has_value());
  CHECK_THAT(washer->at("d2_max"), WithinAbs(12.0, 1e-9));
  CHECK_THAT(washer->at("h"), WithinAbs(1.6, 1e-9));
}

TEST_CASE("parts: an unstocked size is refused, naming what is stocked",
          "[parts]") {
  const SourceTables tables;
  // The rule the whole interface rests on. M7 exists as a thread and is not
  // in ISO 4762; interpolating one from M6 and M8 would produce a screw that
  // looks perfect in the viewport and cannot be bought, which is the
  // plausible-and-wrong answer this codebase refuses everywhere.
  std::string error;
  const auto m7 = parts::Dimensions("iso4762def", "M7", &error);
  CHECK_FALSE(m7.has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("M7"));
  CHECK_THAT(error, ContainsSubstring("does not stock"));
  // The refusal has to say what TO ask for, not just what failed.
  CHECK_THAT(error, ContainsSubstring("M6"));
  CHECK_THAT(error, ContainsSubstring("M8"));

  // An unknown standard names the path, so a build whose data did not stage
  // is distinguishable from a typo.
  error.clear();
  CHECK_FALSE(parts::Dimensions("iso9999def", "M6", &error).has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("iso9999def"));
  CHECK_THAT(error, ContainsSubstring("DESIGNER_DATA_DIR"));
}

TEST_CASE("parts: a derived length snaps to a size that is made", "[parts]") {
  const SourceTables tables;
  // A derived grip is not a screw. 22.7 mm of stack needs the next nominal
  // length up, because 20, 25 and 30 are what exist — a part list asking for
  // 22.7 cannot be ordered, and a screw drawn at 22.7 looks right.
  std::string error;
  auto snapped = parts::SnapLength("iso4762def", "M6", 22.7, &error);
  INFO(error);
  REQUIRE(snapped.has_value());
  CHECK_THAT(*snapped, WithinAbs(25.0, 1e-9));

  // Exactly on a nominal length stays there rather than jumping up.
  snapped = parts::SnapLength("iso4762def", "M6", 20.0, &error);
  REQUIRE(snapped.has_value());
  CHECK_THAT(*snapped, WithinAbs(20.0, 1e-9));

  // Past the end of the series is a refusal naming the range, not the longest
  // one available — silently handing back a screw 200 mm shorter than asked
  // is the substitution, and it would assemble.
  error.clear();
  CHECK_FALSE(
      parts::SnapLength("iso4762def", "M6", 5000.0, &error).has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("no nominal length"));
}

TEST_CASE("parts: a malformed table refuses rather than half-loading",
          "[parts]") {
  // A table that half loads is worse than one that refuses: the sizes it did
  // not read look exactly like sizes the standard does not define, so the
  // refusal above would start lying about what is stocked.
  const TempTables tables;
  std::string error;

  tables.Write("ragged", "\"Ragged\"\n\"Dia\",\"P\",\"k\"\n\"M6\",1.0\n");
  CHECK(parts::Load("ragged", &error) == nullptr);
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("2 fields against 3 columns"));

  error.clear();
  tables.Write("dup", "\"Dup\"\n\"Dia\",\"k\"\n\"M6\",6.0\n\"M6\",7.0\n");
  CHECK(parts::Load("dup", &error) == nullptr);
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("twice"));

  // A field that is a NAME rather than a dimension is not malformed, and this
  // is a deliberate loosening (phase 4): iso14579def.csv names the driver each
  // size takes ("M6","T30",2.3) in the middle of a dimension table, and
  // refusing that file would have meant no hexalobular drive at all. The
  // guarantee that matters is kept — the field is ABSENT from the numbers, so
  // nothing reads a driver name as a millimetre — and it is readable as text.
  error.clear();
  tables.Write("named", "\"Named\"\n\"Dia\",\"k\",\"Drive\"\n\"M6\",6.0,T30\n");
  REQUIRE(parts::Load("named", &error) != nullptr);
  const auto named = parts::Dimensions("named", "M6", &error);
  REQUIRE(named.has_value());
  CHECK(named->at("k") == 6.0);
  CHECK(named->count("Drive") == 0);
  CHECK(parts::TextField("named", "M6", "Drive", &error) == "T30");

  error.clear();
  tables.Write("empty", "\"Empty\"\n\"Dia\",\"k\"\n");
  CHECK(parts::Load("empty", &error) == nullptr);
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("no table with any rows"));

  // A blank cell is ABSENT, not zero — zero is a real dimension and "this
  // standard does not define that here" is not the same statement.
  error.clear();
  tables.Write("sparse", "\"Sparse\"\n\"Dia\",\"k\",\"t\"\n\"M6\",6.0,\n");
  const parts::TableFile* sparse = parts::Load("sparse", &error);
  INFO(error);
  REQUIRE(sparse != nullptr);
  const auto& row = sparse->tables.front().rows.at("M6");
  CHECK(row.count("k") == 1);
  CHECK(row.count("t") == 0);
}

TEST_CASE("parts: sizes read back in the order a person expects", "[parts]") {
  const SourceTables tables;
  // M10 sorts before M2 lexicographically, so a std::map's own order would
  // make every refusal message read as though the table were shuffled.
  std::string error;
  const parts::TableFile* file = parts::Load("iso4762def", &error);
  INFO(error);
  REQUIRE(file != nullptr);
  const std::vector<std::string> sizes = file->tables.front().order;
  REQUIRE(sizes.size() > 10);
  CHECK(sizes.front() == "M1.6");
  const auto m2 = std::find(sizes.begin(), sizes.end(), "M2");
  const auto m10 = std::find(sizes.begin(), sizes.end(), "M10");
  REQUIRE(m2 != sizes.end());
  REQUIRE(m10 != sizes.end());
  CHECK(m2 < m10);
}

TEST_CASE("parts: the shipped tables all load", "[parts]") {
  const SourceTables tables;
  // Cheap, and it is the check that fails when a re-vendor drops a file or a
  // packaging change forgets to stage the directory.
  std::string error;
  for (const char* standard :
       {"iso4762def", "iso4032def", "iso7089def", "iso888length", "iso4757def",
        "iso10664def", "iso14579def", "iso4017head", "iso10642def",
        "iso7380def", "clearance_hole_sizes", "tap_hole_sizes"}) {
    INFO(standard << ": " << error);
    CHECK(parts::Load(standard, &error) != nullptr);
  }
}

TEST_CASE("parts: the build stages the tables where the app looks", "[parts]") {
  // The other cases read the source tree, so none of them would notice a
  // packaging change that stopped copying assets/parts. This is the one that
  // does: the app resolves DataDir() beside its own executable, and the tables
  // have to be there or every fastener refuses at runtime with a message about
  // a path nobody edited.
  const fs::path staged =
      RuntimePaths::ExecutableDir().parent_path() / "assets" / "parts";
  INFO("expected the build to stage " << staged);
  REQUIRE(fs::is_directory(staged));
  for (const char* standard :
       {"iso4762def", "iso4032def", "iso7089def", "iso888length", "iso4757def",
        "iso10664def", "iso14579def", "iso4017head", "iso10642def",
        "iso7380def", "clearance_hole_sizes", "tap_hole_sizes"}) {
    INFO(standard);
    CHECK(fs::exists(staged / (std::string(standard) + ".csv")));
  }
}

// --- the generated body ------------------------------------------------------

TEST_CASE("parts: a generated screw measures what the standard says",
          "[parts]") {
  // The check that matters, and the reason the tables are vendored at all: the
  // body is compared against the ROW it was built from, so this is a test
  // against ISO 4762 rather than against itself. Every number below is read
  // out of the table at runtime — hard-coding them here would test that the
  // generator agrees with a copy of the data, which is a weaker claim.
  const SourceTables tables;
  std::string error;
  const auto dims = parts::Dimensions("iso4762def", "M6", &error);
  INFO(error);
  REQUIRE(dims.has_value());
  const double dk = dims->at("dk_max"), k = dims->at("k");
  const double s = dims->at("s_mean"), t = dims->at("t");
  const double length = 20.0, nominal = 6.0;

  Document doc;
  doc.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso4762def", "M6", length));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& screw = doc.GetBodies()[0];
  REQUIRE(screw.shape.IsValid());

  // The head is dk across and k tall, and the shank runs `length` from under
  // it: a body that is the right volume but the wrong shape would pass a mass
  // check and fail this.
  glm::vec3 lo(0.0f), hi(0.0f);
  screw.shape.GetBoundingBox(lo, hi);
  CHECK_THAT(hi.x - lo.x, WithinAbs(dk, 1e-3));
  CHECK_THAT(hi.y - lo.y, WithinAbs(dk, 1e-3));
  CHECK_THAT(hi.z - lo.z, WithinAbs(length + k, 1e-3));
  // Built head-down: the rim of the head is z = 0, which is the face that
  // lands on a picked rim in phase 3.
  CHECK_THAT(lo.z, WithinAbs(-k, 1e-3));

  // And the volume, analytically: a head cylinder plus a shank cylinder minus
  // a hexagonal socket of `t` deep. A half-scale profile, a radius used as a
  // diameter or a socket cut to the wrong depth each move this.
  constexpr double kPi = 3.14159265358979323846;
  const double head = kPi * (dk / 2) * (dk / 2) * k;
  const double shank = kPi * (nominal / 2) * (nominal / 2) * length;
  const double socket = s * s * std::sqrt(3.0) / 2.0 * t; // regular hexagon
  CHECK_THAT(screw.shape.Volume(), WithinAbs(head + shank - socket, 0.5));

  // The designation is what the BOM and a purchase order say.
  const auto* feature = dynamic_cast<const FastenerFeature*>(doc.GetFeature(0));
  REQUIRE(feature != nullptr);
  CHECK(feature->Designation() == "ISO 4762 M6x20");
}

TEST_CASE("parts: a screw the standard does not stock builds nothing, loudly",
          "[parts]") {
  // The refusal reaching a feature. A fastener that quietly came back empty
  // would leave an assembly a screw short with nothing to read, which is the
  // shape of every bug this project's conventions exist to prevent.
  const SourceTables tables;
  Document doc;
  doc.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso4762def", "M7", 20.0));
  CHECK(doc.GetBodies().empty());
  REQUIRE(doc.FeatureCount() == 1);
  INFO(doc.GetFeature(0)->GetError());
  CHECK_THAT(doc.GetFeature(0)->GetError(),
             ContainsSubstring("nearest stocked are M6 and M8"));

  // A length nobody would make, and a drive this build does not generate:
  // each refuses by name rather than producing something plausible.
  Document zero;
  zero.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso4762def", "M6", 0.0));
  CHECK(zero.GetBodies().empty());
  CHECK_THAT(zero.GetFeature(0)->GetError(), ContainsSubstring("needs a "
                                                               "length"));

  Document drive;
  auto phillips =
      std::make_unique<FastenerFeature>("Screw", "iso4762def", "M6", 20.0);
  phillips->drive = "ph2";
  drive.AddFeature(std::move(phillips));
  CHECK(drive.GetBodies().empty());
  INFO(drive.GetFeature(0)->GetError());
  CHECK_THAT(drive.GetFeature(0)->GetError(),
             ContainsSubstring("is not one this build cuts"));

  // A drive this build DOES cut, on a standard that is not made with it. The
  // refusal names the standard that is, because a torx socket cap screw is an
  // ISO 14579 — it is a different catalogue item, not a configuration of this
  // one, and that is the correction the caller needs.
  Document wrongDrive;
  auto torx = std::make_unique<FastenerFeature>("Screw", "iso4762", "M6", 20.0);
  torx->drive = "torx";
  wrongDrive.AddFeature(std::move(torx));
  CHECK(wrongDrive.GetBodies().empty());
  INFO(wrongDrive.GetFeature(0)->GetError());
  CHECK_THAT(wrongDrive.GetFeature(0)->GetError(),
             ContainsSubstring("iso14579"));

  // A standard nothing in this build makes.
  Document unknown;
  unknown.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "din912", "M6", 20.0));
  CHECK(unknown.GetBodies().empty());
  INFO(unknown.GetFeature(0)->GetError());
  CHECK_THAT(unknown.GetFeature(0)->GetError(),
             ContainsSubstring("socket head cap screw"));

  // A standard that describes something else entirely: ISO 7089 is a washer
  // and has no length to be 20 mm of.
  Document washer;
  washer.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso7089def", "M6", 20.0));
  CHECK(washer.GetBodies().empty());
  INFO(washer.GetFeature(0)->GetError());
  CHECK_THAT(washer.GetFeature(0)->GetError(),
             ContainsSubstring("has no length"));
}

namespace {

// The volume of a solid built from one FastenerFeature, or 0 with the refusal
// in `error`. Every case below measures the body rather than trusting that it
// was built, because a feature that refuses returns no body at all.
double BuildVolume(const char* standard, const char* size, double length,
                   Document& doc) {
  doc.AddFeature(
      std::make_unique<FastenerFeature>("F", standard, size, length));
  if (doc.GetBodies().empty())
    return 0.0;
  return doc.GetBodies()[0].shape.Volume();
}

constexpr double kPi = 3.14159265358979323846;

double HexArea(double acrossFlats) {
  return acrossFlats * acrossFlats * std::sqrt(3.0) / 2.0;
}

// How much of a disc of radius `rho` a regular hexagon of that width covers,
// both centred on the axis. Whole disc while it fits inside the flats, whole
// hexagon once the disc clears the corners, and in between the disc less the
// six circular segments that stick out past the flats.
double DiscInsideHex(double rho, double acrossFlats) {
  const double flat = acrossFlats * 0.5;
  const double corner = acrossFlats / std::sqrt(3.0);
  if (rho <= flat)
    return kPi * rho * rho;
  if (rho >= corner)
    return HexArea(acrossFlats);
  return kPi * rho * rho - 6.0 * (rho * rho * std::acos(flat / rho) -
                                  flat * std::sqrt(rho * rho - flat * flat));
}

// A button head with its socket, by integrating the dome's cross-section and
// taking the hexagon out of the slices the socket reaches.
//
// Needed because the socket is WIDER than the dome near the crown, so it does
// not remove a clean prism: it opens through the top of the dome, which is
// what a real button head cap screw looks like from above and is why this
// head is the one that cannot be checked with area x depth.
double ButtonHeadVolume(double dk, double k, double s, double t) {
  const double r = dk * 0.5;
  const double sphere = (r * r + k * k) / (2.0 * k);
  const double centre = -k + sphere;
  constexpr int kSlices = 20000;
  const double dz = k / kSlices;
  double volume = 0.0;
  for (int i = 0; i < kSlices; i++) {
    const double z = -k + (i + 0.5) * dz;
    const double rho =
        std::sqrt(std::max(0.0, sphere * sphere - (z - centre) * (z - centre)));
    double area = kPi * rho * rho;
    if (z <= -k + t)
      area -= DiscInsideHex(rho, s);
    volume += area * dz;
  }
  return volume;
}

// The cross-sectional area of a hexalobular recess, by counting: a central
// disc of diameter B unioned with six discs of radius Re on a circle of radius
// A/2 - Re. Integrated numerically rather than derived, because the closed
// form is six circle-circle lens areas and getting THAT wrong would make this
// test agree with a bug in the generator for the same reason.
double TorxArea(double A, double B, double Re) {
  const double ring = A * 0.5 - Re;
  const double half = A * 0.5 + 1e-9;
  constexpr int kSteps = 1200;
  const double step = 2.0 * half / kSteps;
  double inside = 0.0;
  for (int ix = 0; ix < kSteps; ix++) {
    const double x = -half + (ix + 0.5) * step;
    for (int iy = 0; iy < kSteps; iy++) {
      const double y = -half + (iy + 0.5) * step;
      bool in = x * x + y * y <= (B * 0.5) * (B * 0.5);
      for (int i = 0; i < 6 && !in; i++) {
        const double a = i * kPi / 3.0;
        const double dx = x - ring * std::cos(a), dy = y - ring * std::sin(a);
        in = dx * dx + dy * dy <= Re * Re;
      }
      if (in)
        inside += step * step;
    }
  }
  return inside;
}

} // namespace

TEST_CASE("parts: a nut and a washer are their standards' own numbers",
          "[parts]") {
  // Phase 4's first claim: the joint can be completed. A nut and a washer are
  // the two things every assembly we have built is missing on the far side of
  // its holes, and both are checked against the row they were built from.
  const SourceTables tables;
  std::string error;

  const auto nutDims = parts::Dimensions("iso4032", "M6", &error);
  INFO(error);
  REQUIRE(nutDims.has_value());
  const double acrossFlats = nutDims->at("s_nom"), m = nutDims->at("m");

  Document nutDoc;
  const double nut = BuildVolume("iso4032", "M6", 0.0, nutDoc);
  REQUIRE(nutDoc.GetBodies().size() == 1);
  glm::vec3 lo(0.0f), hi(0.0f);
  nutDoc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  // A hexagon measures `s` across the flats and 2/sqrt(3) times that across
  // the corners, so its bounding box has one of each — which of the two lands
  // on X depends on the prism's phase and is not a promise worth making.
  // Checking BOTH is the stronger statement anyway: a nut built to the wrong
  // one of those two numbers does not fit the spanner it is named for, and it
  // would pass a check against either alone.
  CHECK_THAT(std::min(hi.x - lo.x, hi.y - lo.y), WithinAbs(acrossFlats, 1e-3));
  CHECK_THAT(std::max(hi.x - lo.x, hi.y - lo.y),
             WithinAbs(acrossFlats * 2.0 / std::sqrt(3.0), 1e-3));
  CHECK_THAT(hi.z - lo.z, WithinAbs(m, 1e-3));
  // Bearing face on z = 0, body below — the convention every fastener shares,
  // so the placement layer can point one down an axis without knowing which
  // of the three shapes it is holding.
  CHECK_THAT(hi.z, WithinAbs(0.0, 1e-6));
  CHECK_THAT(nut, WithinAbs(HexArea(acrossFlats) * m - kPi * 9.0 * m, 0.5));

  const auto washerDims = parts::Dimensions("iso7089", "M6", &error);
  REQUIRE(washerDims.has_value());
  const double outer = washerDims->at("d2_max");
  const double bore = washerDims->at("d1_min"), h = washerDims->at("h");

  Document washerDoc;
  const double washer = BuildVolume("iso7089", "M6", 0.0, washerDoc);
  REQUIRE(washerDoc.GetBodies().size() == 1);
  washerDoc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  CHECK_THAT(hi.x - lo.x, WithinAbs(outer, 1e-3));
  CHECK_THAT(hi.z - lo.z, WithinAbs(h, 1e-3));
  CHECK_THAT(hi.z, WithinAbs(0.0, 1e-6));
  CHECK_THAT(washer,
             WithinAbs(kPi * (outer * outer - bore * bore) / 4.0 * h, 0.5));

  // Neither carries a designation with a length in it: "ISO 4032 M6x0" is not
  // a thing to order, and a BOM line reading it would be nonsense.
  CHECK(dynamic_cast<const FastenerFeature*>(nutDoc.GetFeature(0))
            ->Designation() == "ISO 4032 M6");
  CHECK(dynamic_cast<const FastenerFeature*>(washerDoc.GetFeature(0))
            ->Designation() == "ISO 7089 M6");
}

TEST_CASE("parts: the four head shapes each measure their standard",
          "[parts]") {
  // One case per head, each against its own table's spelling of the same
  // dimension — `dk_max` in ISO 4762, `dk` in ISO 7380, `csh_diam` in
  // ISO 10642, `s` in ISO 4017. Normalising those away would have ended the
  // ability to diff the vendored files against upstream, so they are listed.
  const SourceTables tables;
  std::string error;
  const double length = 20.0, shank = kPi * 9.0 * length;
  glm::vec3 lo(0.0f), hi(0.0f);

  SECTION("a hex head is turned from the outside and has no recess") {
    const auto dims = parts::Dimensions("iso4017", "M6", &error);
    INFO(error);
    REQUIRE(dims.has_value());
    const double acrossFlats = dims->at("s"), k = dims->at("k");
    Document doc;
    const double volume = BuildVolume("iso4017", "M6", length, doc);
    REQUIRE(doc.GetBodies().size() == 1);
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(std::min(hi.x - lo.x, hi.y - lo.y),
               WithinAbs(acrossFlats, 1e-3));
    CHECK_THAT(std::max(hi.x - lo.x, hi.y - lo.y),
               WithinAbs(acrossFlats * 2.0 / std::sqrt(3.0), 1e-3));
    CHECK_THAT(lo.z, WithinAbs(-k, 1e-3));
    CHECK_THAT(hi.z - lo.z, WithinAbs(length + k, 1e-3));
    // No socket subtracted, because there is nothing to put a driver in: the
    // whole point of a hex head is that it is turned by a spanner.
    CHECK_THAT(volume, WithinAbs(HexArea(acrossFlats) * k + shank, 0.5));
  }

  SECTION("a button head is a spherical cap of the standard's height") {
    const auto dims = parts::Dimensions("iso7380", "M6", &error);
    INFO(error);
    REQUIRE(dims.has_value());
    const double dk = dims->at("dk"), k = dims->at("k");
    const double s = dims->at("s_mean"), t = dims->at("t_min");
    Document doc;
    const double volume = BuildVolume("iso7380", "M6", length, doc);
    REQUIRE(doc.GetBodies().size() == 1);
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(hi.x - lo.x, WithinAbs(dk, 1e-3));
    CHECK_THAT(hi.z, WithinAbs(length, 1e-3));
    // NOT -k, and this is the finding rather than a tolerance: the socket is
    // wider than the dome near the crown, so it opens THROUGH the top and the
    // apex is not there to measure. What is left is the dome down to where it
    // is as wide as the socket is across its flats — which is what a button
    // head cap screw looks like from above, and is 0.36 mm short of the
    // standard's head height for reasons that are geometry, not error.
    // A cap k tall on a base dk across is a sphere of (r^2 + k^2) / 2k; a
    // hemisphere, which is what a domed head is easy to draw as, would be
    // 5.25 mm tall here against the standard's 3.3.
    const double r = dk * 0.5;
    const double sphere = (r * r + k * k) / (2.0 * k);
    const double crown =
        (-k + sphere) - std::sqrt(sphere * sphere - s * s / 4.0);
    CHECK(crown > -k);
    CHECK_THAT(lo.z, WithinAbs(crown, 1e-3));
    CHECK_THAT(volume, WithinAbs(ButtonHeadVolume(dk, k, s, t) + shank, 0.5));
  }

  SECTION("a countersunk head is a cone, and the length is OVER it") {
    const auto dims = parts::Dimensions("iso10642", "M6", &error);
    INFO(error);
    REQUIRE(dims.has_value());
    const double dk = dims->at("csh_diam"), k = dims->at("csh_height");
    const double s = dims->at("s_mean"), t = dims->at("t");
    Document doc;
    const double volume = BuildVolume("iso10642", "M6", length, doc);
    REQUIRE(doc.GetBodies().size() == 1);
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(hi.x - lo.x, WithinAbs(dk, 1e-3));
    // The one head of the four that does NOT stand proud: it is sunk into the
    // part, so the whole screw lies at +z and an M6x20 is 20 mm OVERALL. Read
    // as 20 mm under the head it would be one head-height too long, which
    // looks perfectly right in a viewport.
    CHECK_THAT(lo.z, WithinAbs(0.0, 1e-6));
    CHECK_THAT(hi.z - lo.z, WithinAbs(length, 1e-3));
    const double cone = kPi * k / 3.0 * (dk * dk / 4.0 + dk * 3.0 / 2.0 + 9.0);
    CHECK_THAT(
        volume,
        WithinAbs(cone + kPi * 9.0 * (length - k) - HexArea(s) * t, 0.5));
  }

  SECTION("a hexalobular screw borrows ISO 4762's head, by name") {
    // ISO 14579 IS ISO 4762 with a different recess, and its vendored file
    // says so by carrying only the recess — no dk, no k. The borrowing is
    // declared in the profile table rather than inferred, and this is what
    // checks the declaration against the two files.
    const auto head = parts::Dimensions("iso4762", "M6", &error);
    const auto socket = parts::Dimensions("iso14579", "M6", &error);
    INFO(error);
    REQUIRE(head.has_value());
    REQUIRE(socket.has_value());
    CHECK(socket->count("dk_max") == 0);
    const double dk = head->at("dk_max"), k = head->at("k");
    const double A = socket->at("A"), depth = socket->at("t_mean");

    const auto driver =
        parts::TextField("iso14579def", "M6", "DriveSize", &error);
    REQUIRE(driver.has_value());
    CHECK(*driver == "T30");
    const auto lobe = parts::Dimensions("iso10664def", *driver, &error);
    REQUIRE(lobe.has_value());

    Document doc;
    const double volume = BuildVolume("iso14579", "M6", length, doc);
    REQUIRE(doc.GetBodies().size() == 1);
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(hi.x - lo.x, WithinAbs(dk, 1e-3));
    CHECK_THAT(lo.z, WithinAbs(-k, 1e-3));
    // And the recess is the lobed one, not a hexagon: an M6's torx socket is
    // 5.6 across the lobes against a 5.06 hexagon, so a substituted hex would
    // land within a few percent on volume and be the wrong shape entirely.
    const double lobed = TorxArea(A, lobe->at("B"), lobe->at("Re")) * depth;
    CHECK_THAT(volume, WithinAbs(kPi * dk * dk / 4.0 * k + shank - lobed, 0.6));
    CHECK(dynamic_cast<const FastenerFeature*>(doc.GetFeature(0))
              ->Designation() == "ISO 14579 M6x20");
  }
}

TEST_CASE("parts: a grip a hair over a nominal length snaps to it", "[parts]") {
  // Found on a real model: two 10 mm bars, an M8 through both, and the answer
  // was an M8x25. A grip is a sum of boolean intersection distances, so the
  // stack measured 20.0000005 and a bare `<` rejected the 20 mm screw for
  // half a micron — five millimetres of error out of numeric noise, on the
  // most ordinary joint there is.
  const SourceTables tables;
  std::string error;
  CHECK(parts::SnapLength("iso4762", "M8", 20.0000005, &error) == 20.0);
  CHECK(parts::SnapLength("iso4762", "M8", 20.0, &error) == 20.0);
  // And a grip that is genuinely longer still goes up: the tolerance absorbs
  // noise, not a millimetre of real material.
  CHECK(parts::SnapLength("iso4762", "M8", 20.5, &error) == 25.0);

  // The blind-hole direction is bounded from ABOVE and gets the same
  // treatment, for the same reason and with the same limit.
  CHECK(parts::SnapLengthDown("iso4762", "M8", 19.9999995, &error) == 20.0);
  CHECK(parts::SnapLengthDown("iso4762", "M8", 19.5, &error) == 16.0);
}

TEST_CASE("parts: an ambiguous clearance diameter is refused, not guessed",
          "[parts]") {
  // Found building the single-cylinder engine
  // (generators/README.md#single-cylinder-and-air-cooled-engines). 46 of the
  // clearance table's 111 diameters are a clearance for two different threads,
  // and the backward lookup returned whichever came first in table order —
  // always the SMALLER thread on the LOOSER fit.
  const SourceTables tables;
  std::string error;

  // Unambiguous: 6.6 is an M6 Normal and nothing else.
  const auto six = parts::ThreadForClearance(6.6, &error);
  REQUIRE(six);
  CHECK(six->first == "M6");
  CHECK(six->second == "Normal");

  // 11.0 is an M9 Loose AND an M10 Normal. It used to answer M9 — a size
  // ISO 4762 does not stock — so an ordinary M10 clearance hole came back as
  // a refusal about M9.
  error.clear();
  CHECK_FALSE(parts::ThreadForClearance(11.0, &error).has_value());
  INFO(error);
  CHECK(error.find("M9") != std::string::npos);
  CHECK(error.find("M10") != std::string::npos);
  // And it says the way through rather than just saying no.
  CHECK(error.find("size") != std::string::npos);

  // 10.0 is the same shape: M8 Loose and M9 Normal.
  error.clear();
  CHECK_FALSE(parts::ThreadForClearance(10.0, &error).has_value());

  // A diameter that is nobody's clearance is still refused, with the older
  // message — the two refusals are for different reasons and say so.
  error.clear();
  CHECK_FALSE(parts::ThreadForClearance(6.5, &error).has_value());
  INFO(error);
  CHECK(error.find("more than one") == std::string::npos);
}

TEST_CASE("parts: a hole knows what it is for", "[parts]") {
  // docs/modeling/holes-and-threads.md#hole. The other direction from
  // ThreadForClearance: given the intent, the number — so "a clearance hole
  // for an M6, normal fit" stops being 6.6 that somebody had to know, look up
  // and type.
  const SourceTables tables;
  std::string error;

  CHECK(parts::HoleDiameter("M6", parts::Fit::Close, 0.0, &error) == 6.4);
  CHECK(parts::HoleDiameter("M6", parts::Fit::Normal, 0.0, &error) == 6.6);
  CHECK(parts::HoleDiameter("M6", parts::Fit::Loose, 0.0, &error) == 7.0);

  // Tapped is a different table and a different question: the drill depends
  // on the PITCH as well as the size, and the two are one key ("M6-1").
  CHECK(parts::HoleDiameter("M6", parts::Fit::Tapped, 0.0, &error) == 5.0);
  CHECK(parts::HoleDiameter("M6", parts::Fit::Tapped, 0.75, &error) == 5.25);
  // Which is why the pitch is not assumed: a fine M6 taps to a quarter of a
  // millimetre bigger, and getting it wrong taps BADLY rather than looking
  // wrong.
  CHECK(parts::HoleDiameter("M6", parts::Fit::Tapped, 0.0, &error) !=
        parts::HoleDiameter("M6", parts::Fit::Tapped, 0.75, &error));

  // The coarse pitch comes from the standard already vendored, not from a
  // second table of the same numbers that could disagree with it.
  CHECK(parts::CoarsePitch("M6", &error) == 1.0);
  CHECK(parts::CoarsePitch("M8", &error) == 1.25);

  // And a tapped hole is nothing like the clearance hole for the same thread
  // — 5.0 against 6.4/6.6/7.0 — which is what lets a check tell a tapped
  // blind hole from a clearance one drilled to the same depth.
  CHECK(*parts::HoleDiameter("M6", parts::Fit::Tapped, 0.0, &error) <
        *parts::HoleDiameter("M6", parts::Fit::Close, 0.0, &error) - 1.0);

  // Refused, never rounded to a neighbour.
  error.clear();
  CHECK_FALSE(
      parts::HoleDiameter("M7.3", parts::Fit::Normal, 0.0, &error).has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("nearest stocked"));
  error.clear();
  CHECK_FALSE(
      parts::HoleDiameter("M6", parts::Fit::Tapped, 0.9, &error).has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("M6-0.9"));

  CHECK(parts::FitFromName("tapped") == parts::Fit::Tapped);
  CHECK_FALSE(parts::FitFromName("snug").has_value());
  CHECK(parts::FitName(parts::Fit::Loose) == "loose");
}

TEST_CASE("parts: a non-preferred size is a size, not a refusal", "[parts]") {
  // ISO marks the sizes it would rather you avoided with parentheses —
  // "(M3.5)" — and that is a recommendation about which sizes to design
  // around, not a different part number. Asking for M3.5 used to be refused
  // with "the nearest stocked are M3 and M4", which is a lie about a size
  // sitting in the table.
  const SourceTables tables;
  std::string error;
  const auto dims = parts::Dimensions("iso4032", "M3.5", &error);
  INFO(error);
  REQUIRE(dims.has_value());
  CHECK(dims->at("s_nom") == 6.0);
  // Quoted the way the file quotes it, too, so echoing a key back out of a
  // listing is not refused for carrying the marker it was given.
  CHECK(parts::Dimensions("iso4032", "(M3.5)", &error).has_value());
  // And it is offered as a neighbour, which is the reason this matters: the
  // refusal for a size that really is absent has to name the one next to it.
  CHECK_FALSE(parts::Dimensions("iso4032", "M3.6", &error).has_value());
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("(M3.5)"));
}

TEST_CASE("parts: a fastener round-trips through the document format",
          "[parts][serialization]") {
  const SourceTables tables;
  Document doc;
  auto screw =
      std::make_unique<FastenerFeature>("Screw", "iso4762def", "M8", 30.0);
  screw->lengthExpr = "plate * 2";
  doc.AddFeature(std::move(screw));
  REQUIRE(doc.GetBodies().size() == 1);
  const double volume = doc.GetBodies()[0].shape.Volume();

  const nlohmann::json j = DocumentIO::FeatureToJson(*doc.GetFeature(0));
  CHECK(j.at("type") == "Fastener");
  CHECK(j.at("standard") == "iso4762def");
  CHECK(j.at("size") == "M8");
  CHECK(j.at("length_expr") == "plate * 2");
  // The default drive is omitted rather than written, like every other
  // defaulted field in this format.
  CHECK_FALSE(j.contains("drive"));

  Document reloaded;
  reloaded.AddFeature(DocumentIO::FeatureFromJson(j));
  REQUIRE(reloaded.GetBodies().size() == 1);
  CHECK_THAT(reloaded.GetBodies()[0].shape.Volume(), WithinAbs(volume, 1e-6));
}

TEST_CASE("Axis traversal ignores the bodies its ray cannot reach",
          "[parts][axis]") {
  // `placed` is every body of every leaf in the assembly — 300-odd on the
  // OM615 — and a bolt's axis crosses three or four. Without a box test each
  // fit paid four ray casts AND a solid classifier for every body in the
  // model: measured A/B on that engine's build, the prune is 82.1 s -> 74.9
  // and fit_fastener 528 ms a call -> 369
  // (docs/parts/README.md#fitting-a-fastener-what-the-axis-crosses).
  //
  // What must not change is the ANSWER.
  Body plate;
  plate.shape = CADShape(BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 20), 10, 10, 20));
  std::vector<parts::PlacedBody> placed{{&plate, glm::mat4(1), "plate"}};
  const auto alone = parts::TraverseAxis(placed, {0, 0, 0}, {0, 0, 1}, 0, 60);
  REQUIRE(alone.size() == 1);

  // Thirty bodies the axis comes nowhere near, on both sides and beyond the
  // reach, plus one squarely ON the axis past the plate — that one MUST
  // still be found.
  std::vector<Body> far;
  far.reserve(32);
  for (int i = 0; i < 30; ++i) {
    Body b;
    const double x = 100.0 + 20.0 * i;
    b.shape = CADShape(BRepPrimAPI_MakeBox(gp_Pnt(x, -5, 0), 10, 10, 10));
    far.push_back(std::move(b));
  }
  Body beyond;
  beyond.shape = CADShape(BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 45), 10, 10, 10));
  for (Body& b : far)
    placed.push_back({&b, glm::mat4(1), "far"});
  placed.push_back({&beyond, glm::mat4(1), "beyond"});

  const auto crowded = parts::TraverseAxis(placed, {0, 0, 0}, {0, 0, 1}, 0, 60);
  // The plate reads exactly as it did alone, and the body ON the axis is
  // found: a prune that loses it would silently shorten every bolt.
  REQUIRE(crowded.size() == 2);
  CHECK(crowded[0].part == "plate");
  CHECK_THAT(crowded[0].enter, WithinAbs(alone[0].enter, 1e-9));
  CHECK_THAT(crowded[0].exit, WithinAbs(alone[0].exit, 1e-9));
  CHECK(crowded[1].part == "beyond");
  CHECK_THAT(crowded[1].enter, WithinAbs(45.0, 1e-6));
  CHECK_THAT(crowded[1].exit, WithinAbs(55.0, 1e-6));
}
