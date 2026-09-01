#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/RuntimePaths.h"
#include "core/Document.h"
#include "core/features/FastenerFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/FastenerTable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

// The vendored standard-parts tables (docs/parts/plan.md, phase 1).
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

  error.clear();
  tables.Write("nan", "\"Nan\"\n\"Dia\",\"k\"\n\"M6\",six\n");
  CHECK(parts::Load("nan", &error) == nullptr);
  INFO(error);
  CHECK_THAT(error, ContainsSubstring("is not a number"));

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
  for (const char* standard : {"iso4762def", "iso4032def", "iso7089def",
                               "iso888length", "iso4757def", "iso10664def"}) {
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
  for (const char* standard : {"iso4762def", "iso4032def", "iso7089def",
                               "iso888length", "iso4757def", "iso10664def"}) {
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
             ContainsSubstring("is not generated yet"));

  // A standard that describes something else entirely: ISO 7089 is a washer
  // and has no head or socket to build a screw from.
  Document washer;
  washer.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso7089def", "M6", 20.0));
  CHECK(washer.GetBodies().empty());
  INFO(washer.GetFeature(0)->GetError());
  CHECK_THAT(washer.GetFeature(0)->GetError(),
             ContainsSubstring("does not describe one"));
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
