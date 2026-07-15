#include <catch2/catch_test_macros.hpp>

#include "core/DrawingDocument.h"
#include "core/Serialization.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace {

// A representative drawing exercising every serialized field: an A3 landscape
// sheet with a title block, a base view + a folded child, and a diameter
// dimension referencing a silhouette.
DrawingDocument SampleDrawing() {
  DrawingDocument dwg;
  dwg.GetSheet().size = SheetSize::A3;
  dwg.GetSheet().landscape = true;
  dwg.GetSheet().titleBlock.project = "Gearbox";
  dwg.GetSheet().titleBlock.document = "Bracket rev B";
  dwg.GetSheet().titleBlock.author = "E. Aronsson";
  dwg.GetSheet().titleBlock.date = "2026-07-14";

  DrawingView front;
  front.name = "Front";
  front.sourceDocId = 3;
  front.orientation = ViewOrientation::Front;
  front.scale = 0.5;
  front.position = {80.0, 150.0};
  front.showHidden = true;
  const uint64_t frontId = dwg.AddView(front);

  DrawingView top;
  top.name = "Top";
  top.sourceDocId = 3;
  top.orientation = ViewOrientation::Top;
  top.baseViewId = frontId;
  top.scale = 0.5;
  top.position = {80.0, 90.0};
  top.showHidden = false;
  dwg.AddView(top);

  DrawingDim dim;
  dim.kind = DrawingDimKind::Diameter;
  dim.viewId = frontId;
  ProjRef ref;
  ref.viewId = frontId;
  ref.silFace = ElementRef{5, "4/g7"};
  dim.refs.push_back(ref);
  dim.labelOffset = {12.0, -6.0};
  dwg.AddDim(dim);
  return dwg;
}

void CheckSampleFields(const DrawingDocument& dwg) {
  CHECK(dwg.GetSheet().size == SheetSize::A3);
  CHECK(dwg.GetSheet().landscape);
  CHECK(dwg.GetSheet().titleBlock.project == "Gearbox");
  CHECK(dwg.GetSheet().titleBlock.author == "E. Aronsson");

  REQUIRE(dwg.ViewCount() == 2);
  const DrawingView& front = dwg.GetViews()[0];
  CHECK(front.name == "Front");
  CHECK(front.sourceDocId == 3);
  CHECK(front.orientation == ViewOrientation::Front);
  CHECK(front.scale == 0.5);
  CHECK(front.position.x == 80.0);
  CHECK(front.position.y == 150.0);
  CHECK(front.showHidden);

  const DrawingView& top = dwg.GetViews()[1];
  CHECK(top.orientation == ViewOrientation::Top);
  CHECK(top.baseViewId == front.id);
  CHECK_FALSE(top.showHidden);

  REQUIRE(dwg.DimCount() == 1);
  const DrawingDim& dim = dwg.GetDims()[0];
  CHECK(dim.kind == DrawingDimKind::Diameter);
  CHECK(dim.viewId == front.id);
  REQUIRE(dim.refs.size() == 1);
  CHECK(dim.refs[0].silFace.name == "4/g7");
  CHECK(dim.refs[0].silFace.featureId == 5);
  CHECK(dim.labelOffset.x == 12.0);
}

}  // namespace

TEST_CASE("Drawing model: ids are stable and never reused", "[drawing][model]") {
  DrawingDocument dwg;
  const uint64_t v1 = dwg.AddView({});
  const uint64_t v2 = dwg.AddView({});
  DrawingDim d;
  d.viewId = v1;
  const uint64_t d1 = dwg.AddDim(d);
  // Views and dims share one counter — ids are unique across the document.
  CHECK(v1 == 1);
  CHECK(v2 == 2);
  CHECK(d1 == 3);

  REQUIRE(dwg.RemoveView(v1));
  // The next add takes a fresh id, never a recycled one.
  CHECK(dwg.AddView({}) == 4);
}

TEST_CASE("Drawing model: removing a view cascades to its dims",
          "[drawing][model]") {
  DrawingDocument dwg;
  const uint64_t v = dwg.AddView({});
  DrawingDim onView;
  onView.viewId = v;
  dwg.AddDim(onView);
  DrawingDim other;
  other.viewId = v + 100;  // some other (absent) view
  const uint64_t keep = dwg.AddDim(other);

  REQUIRE(dwg.DimCount() == 2);
  REQUIRE(dwg.RemoveView(v));
  // The dim measuring the removed view is gone; the unrelated one survives.
  CHECK(dwg.DimCount() == 1);
  CHECK(dwg.FindDim(keep) != nullptr);
}

TEST_CASE("Drawing round-trips through single-file and directory formats",
          "[drawing][serialization]") {
  const DrawingDocument dwg = SampleDrawing();

  SECTION("single JSON file") {
    const fs::path tmp = fs::temp_directory_path() / "dwg_rt.json";
    std::string err;
    REQUIRE(DocumentIO::SaveDrawing(dwg, tmp.string(), &err));
    auto reloaded = DocumentIO::LoadDrawing(tmp.string(), &err);
    INFO(err);
    REQUIRE(reloaded != nullptr);
    CheckSampleFields(*reloaded);
    // The id counter is pinned past the restored ids.
    CHECK(reloaded->NextId() == 4);
    fs::remove(tmp);
  }

  SECTION("directory format, one file per view/dim") {
    const fs::path proj = fs::temp_directory_path() / "dwg_rt.dsgn";
    fs::remove_all(proj);
    std::string err;
    REQUIRE(DocumentIO::SaveDrawing(dwg, proj.string(), &err));
    CHECK(fs::exists(proj / "index.json"));
    CHECK(fs::exists(proj / "views" / "1-front.json"));
    CHECK(fs::exists(proj / "dims" / "3-diameter.json"));
    auto reloaded = DocumentIO::LoadDrawing(proj.string(), &err);
    INFO(err);
    REQUIRE(reloaded != nullptr);
    CheckSampleFields(*reloaded);
    fs::remove_all(proj);
  }
}

TEST_CASE("Drawing golden loads and round-trips (v30)",
          "[drawing][serialization][golden]") {
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "drawing" / "v30-drawing.json";
  REQUIRE(fs::exists(golden));

  std::string err;
  auto dwg = DocumentIO::LoadDrawing(golden.string(), &err);
  INFO(err);
  REQUIRE(dwg != nullptr);
  CheckSampleFields(*dwg);

  // Round-trip through the current writer (both forms), re-check semantics.
  const fs::path tmp = fs::temp_directory_path() / "dwg_golden_rt.dsgn";
  fs::remove_all(tmp);
  REQUIRE(DocumentIO::SaveDrawing(*dwg, tmp.string(), &err));
  auto reloaded = DocumentIO::LoadDrawing(tmp.string(), &err);
  REQUIRE((reloaded != nullptr && err.empty()));
  CheckSampleFields(*reloaded);
  fs::remove_all(tmp);
}

TEST_CASE("Drawing loaders reject the wrong document kind",
          "[drawing][serialization]") {
  // A drawing saved and read back by the part loader must be refused, and
  // vice versa — the kind discriminator is load-bearing.
  const DrawingDocument dwg = SampleDrawing();
  const fs::path tmp = fs::temp_directory_path() / "dwg_kind.json";
  std::string err;
  REQUIRE(DocumentIO::SaveDrawing(dwg, tmp.string(), &err));

  auto asPart = DocumentIO::Load(tmp.string(), &err);
  CHECK(asPart == nullptr);
  CHECK(err.find("drawing") != std::string::npos);
  fs::remove(tmp);

  // A part file is not a drawing.
  const fs::path partGolden = fs::path(DESIGNER_SOURCE_DIR) / "tests" /
                              "golden" / "v17-full.json";
  if (fs::exists(partGolden)) {
    err.clear();
    auto asDrawing = DocumentIO::LoadDrawing(partGolden.string(), &err);
    CHECK(asDrawing == nullptr);
    CHECK_FALSE(err.empty());
  }
}
