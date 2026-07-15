#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/DrawingDimension.h"
#include "core/DrawingDocument.h"
#include "core/DrawingExport.h"
#include "core/DrawingRender.h"
#include "core/Format.h"
#include "core/Serialization.h"

#include <filesystem>
#include <fstream>
#include <sstream>

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

TEST_CASE("FormatLength/FormatAngle produce compact display text",
          "[drawing][format]") {
  CHECK(core::FormatLength(40.0) == "40");
  CHECK(core::FormatLength(8.5) == "8.5");
  CHECK(core::FormatLength(12.345) == "12.3");
  CHECK(core::FormatLength(-0.0) == "0");
  CHECK(core::FormatLength(0.0) == "0");
  CHECK(core::FormatAngle(0.0) == "0.0\xC2\xB0");
  CHECK(core::FormatAngle(M_PI / 2) == "90.0\xC2\xB0");
  CHECK(core::FormatAngle(M_PI / 4) == "45.0\xC2\xB0");
}

namespace {

// A projected view with the geometry a dimension test needs: two vertical
// edges of a 30 mm-wide box (left "L", right "R"), and a circle "hole" of
// radius 4 centred at (10, 10). All in sheet mm.
drawing::SheetView DimTestView() {
  drawing::SheetView v;
  v.viewId = 1;
  v.hasGeometry = true;

  drawing::SheetCurve left;
  left.edge = ElementRef{1, "L"};
  left.sourceId = 1;
  left.points = {{0, 0}, {0, 20}};
  v.curves.push_back(left);

  drawing::SheetCurve right;
  right.edge = ElementRef{1, "R"};
  right.sourceId = 1;
  right.points = {{30, 0}, {30, 20}};
  v.curves.push_back(right);

  drawing::SheetCurve hole;
  hole.edge = ElementRef{2, "hole"};
  hole.sourceId = 1;
  hole.curveType = 1;  // GeomAbs_Circle
  hole.conicCenter = {10, 10};
  hole.conicRadius = 4.0;
  hole.points = {{14, 10}, {10, 14}, {6, 10}, {10, 6}, {14, 10}};
  v.curves.push_back(hole);
  return v;
}

DrawingDim MakeDim(DrawingDimKind kind, std::vector<ProjRef> refs) {
  DrawingDim d;
  d.id = 7;
  d.kind = kind;
  d.viewId = 1;
  d.refs = std::move(refs);
  return d;
}

ProjRef EdgeRef(uint64_t fid, const std::string& name) {
  ProjRef r;
  r.viewId = 1;
  r.edge = ElementRef{fid, name};
  return r;
}

}  // namespace

TEST_CASE("ResolveDim measures projected geometry", "[drawing][dimension]") {
  const drawing::SheetView view = DimTestView();

  SECTION("distance-X between two edges is the box width") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::DistanceX, {EdgeRef(1, "L"), EdgeRef(1, "R")}),
        view);
    REQUIRE(rd.resolved);
    CHECK(rd.value == Catch::Approx(30.0));
    CHECK(rd.text == "30");
  }

  SECTION("plain distance between the edge midpoints") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Distance, {EdgeRef(1, "L"), EdgeRef(1, "R")}),
        view);
    REQUIRE(rd.resolved);  // both midpoints at y=10 -> pure horizontal 30
    CHECK(rd.value == Catch::Approx(30.0));
  }

  SECTION("diameter of the circular edge") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Diameter, {EdgeRef(2, "hole")}), view);
    REQUIRE(rd.resolved);
    CHECK(rd.radial);
    CHECK(rd.value == Catch::Approx(8.0));
    CHECK(rd.center.x == Catch::Approx(10.0));
    CHECK(rd.text == "\xC3\x98" "8");  // Ø8
  }

  SECTION("radius of the circular edge") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Radius, {EdgeRef(2, "hole")}), view);
    REQUIRE(rd.resolved);
    CHECK(rd.value == Catch::Approx(4.0));
    CHECK(rd.text == "R4");
  }

  SECTION("a ref that matches no curve is dangling, never rebound") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::DistanceX,
                {EdgeRef(1, "L"), EdgeRef(9, "gone")}),
        view);
    CHECK_FALSE(rd.resolved);
    CHECK(rd.value == 0.0);
  }
}

TEST_CASE("SVG export writes the sheet, views, and title block",
          "[drawing][export]") {
  // A tiny sheet with one view of two hand-built curves — a visible triangle
  // and a hidden segment — plus a title block. The writer is GPU-free, so this
  // exercises the whole export path headlessly.
  DrawingDocument doc;
  doc.GetSheet().size = SheetSize::A4;
  doc.GetSheet().landscape = false;  // A4 portrait -> viewBox 210 x 297
  doc.GetSheet().titleBlock.project = "Widget & Co <v2>";  // XML-escaping

  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  drawing::SheetCurve solid;
  solid.visible = true;
  solid.points = {{20, 20}, {60, 20}, {40, 50}, {20, 20}};
  view.curves.push_back(solid);
  drawing::SheetCurve dashed;
  dashed.visible = false;
  dashed.points = {{20, 20}, {60, 20}};
  view.curves.push_back(dashed);

  const fs::path out = fs::temp_directory_path() / "drawing_export.svg";
  std::string err;
  REQUIRE(DrawingExport::SaveSVG(doc, {view}, {}, out.string(), &err));
  INFO(err);
  REQUIRE(fs::exists(out));

  std::ifstream in(out);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string svg = ss.str();

  auto count = [&](const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = svg.find(needle, pos)) != std::string::npos) { ++n; ++pos; }
    return n;
  };

  CHECK(svg.rfind("<?xml", 0) == 0);
  CHECK(svg.find("viewBox=\"0 0 210 297\"") != std::string::npos);
  CHECK(count("<polyline") == 2);
  CHECK(svg.find("stroke-dasharray") != std::string::npos);  // the hidden curve
  // Title-block text is XML-escaped, never raw.
  CHECK(svg.find("Widget &amp; Co &lt;v2&gt;") != std::string::npos);
  CHECK(svg.find("Widget & Co <v2>") == std::string::npos);
  CHECK(svg.rfind("</svg>") != std::string::npos);
  fs::remove(out);
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
