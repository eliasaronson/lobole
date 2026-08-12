#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestIO.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingRender.h"
#include "core/drawing/DrawingValidate.h"
#include "core/io/Format.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
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

} // namespace

TEST_CASE("Drawing model: ids are stable and never reused",
          "[drawing][model]") {
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
  other.viewId = v + 100; // some other (absent) view
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
    REQUIRE(testio::SaveDrawing(dwg, tmp.string()));
    auto reloaded = testio::LoadDrawing(tmp.string());
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
    REQUIRE(testio::SaveDrawing(dwg, proj.string()));
    CHECK(fs::exists(proj / "index.json"));
    CHECK(fs::exists(proj / "views" / "1-front.json"));
    CHECK(fs::exists(proj / "dims" / "3-diameter.json"));
    auto reloaded = testio::LoadDrawing(proj.string());
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
  auto dwg = testio::LoadDrawing(golden.string());
  INFO(err);
  REQUIRE(dwg != nullptr);
  CheckSampleFields(*dwg);

  // Round-trip through the current writer (both forms), re-check semantics.
  const fs::path tmp = fs::temp_directory_path() / "dwg_golden_rt.dsgn";
  fs::remove_all(tmp);
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
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
  hole.curveType = 1; // GeomAbs_Circle
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

} // namespace

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
    REQUIRE(rd.resolved); // both midpoints at y=10 -> pure horizontal 30
    CHECK(rd.value == Catch::Approx(30.0));
  }

  SECTION("diameter of the circular edge") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Diameter, {EdgeRef(2, "hole")}), view);
    REQUIRE(rd.resolved);
    CHECK(rd.radial);
    CHECK(rd.value == Catch::Approx(8.0));
    CHECK(rd.center.x == Catch::Approx(10.0));
    CHECK(rd.text == "\xC3\x98"
                     "8"); // Ø8
  }

  SECTION("radius of the circular edge") {
    auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Radius, {EdgeRef(2, "hole")}), view);
    REQUIRE(rd.resolved);
    CHECK(rd.value == Catch::Approx(4.0));
    CHECK(rd.text == "R4");
  }

  SECTION("a ref that matches no curve is dangling, never rebound") {
    auto rd =
        drawing::ResolveDim(MakeDim(DrawingDimKind::DistanceX,
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
  doc.GetSheet().landscape = false; // A4 portrait -> viewBox 210 x 297
  doc.GetSheet().titleBlock.project = "Widget & Co <v2>"; // XML-escaping

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
    while ((pos = svg.find(needle, pos)) != std::string::npos) {
      ++n;
      ++pos;
    }
    return n;
  };

  CHECK(svg.rfind("<?xml", 0) == 0);
  CHECK(svg.find("viewBox=\"0 0 210 297\"") != std::string::npos);
  CHECK(count("<polyline") == 2);
  CHECK(svg.find("stroke-dasharray") != std::string::npos); // the hidden curve
  // Title-block text is XML-escaped, never raw.
  CHECK(svg.find("Widget &amp; Co &lt;v2&gt;") != std::string::npos);
  CHECK(svg.find("Widget & Co <v2>") == std::string::npos);
  CHECK(svg.rfind("</svg>") != std::string::npos);
  fs::remove(out);
}

TEST_CASE("PDF export writes a valid single-page document",
          "[drawing][export]") {
  DrawingDocument doc;
  doc.GetSheet().size = SheetSize::A3;
  doc.GetSheet().landscape = true;
  doc.GetSheet().titleBlock.project = "Gearbox";

  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  drawing::SheetCurve c;
  c.visible = true;
  c.points = {{20, 20}, {60, 20}, {60, 50}, {20, 50}, {20, 20}};
  view.curves.push_back(c);

  drawing::ResolvedDim dim;
  dim.id = 1;
  dim.kind = DrawingDimKind::Diameter;
  dim.resolved = true;
  dim.radial = true;
  dim.center = {40, 35};
  dim.radius = 6;
  dim.rim = {44, 39};
  dim.value = 12;
  dim.text = "\xC3\x98"
             "12"; // Ø12 — exercises the WinAnsi mapping
  dim.labelPos = {48, 42};

  const fs::path out = fs::temp_directory_path() / "drawing_export.pdf";
  std::string err;
  REQUIRE(DrawingExport::SavePDF(doc, {view}, {dim}, out.string(), &err));
  INFO(err);

  std::ifstream in(out, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string pdf = ss.str();

  CHECK(pdf.rfind("%PDF-1.", 0) == 0);
  CHECK(pdf.find("/Type /Catalog") != std::string::npos);
  CHECK(pdf.find("/MediaBox [0 0 ") != std::string::npos);
  CHECK(pdf.find("/BaseFont /Helvetica") != std::string::npos);
  CHECK(pdf.find("stream") != std::string::npos);
  CHECK(pdf.find("startxref") != std::string::npos);
  // The A3 landscape long edge is 420 mm -> ~1190.55 pt.
  CHECK(pdf.find("1190.55") != std::string::npos);
  // Ø became the WinAnsi byte 0xD8, not the raw UTF-8 pair.
  CHECK(pdf.find("\xD8"
                 "12") != std::string::npos);
  CHECK(pdf.find("\xC3\x98") == std::string::npos);
  CHECK(pdf.rfind("%%EOF") != std::string::npos);
  fs::remove(out);
}

TEST_CASE("Drawing loaders reject the wrong document kind",
          "[drawing][serialization]") {
  // A drawing saved and read back by the part loader must be refused, and
  // vice versa — the kind discriminator is load-bearing.
  const DrawingDocument dwg = SampleDrawing();
  const fs::path tmp = fs::temp_directory_path() / "dwg_kind.json";
  REQUIRE(testio::SaveDrawing(dwg, tmp.string()));

  const auto asPart = DocumentIO::Load(tmp.string());
  CHECK_FALSE(asPart.has_value());
  CHECK(asPart.error().find("drawing") != std::string::npos);
  fs::remove(tmp);

  // A part file is not a drawing.
  const fs::path partGolden =
      fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" / "v17-full.json";
  if (fs::exists(partGolden)) {
    const auto asDrawing = DocumentIO::LoadDrawing(partGolden.string());
    CHECK_FALSE(asDrawing.has_value());
    CHECK_FALSE(asDrawing.error().empty());
  }
}

TEST_CASE("SheetChrome: one source for extent, border, and title block",
          "[drawing][chrome]") {
  Sheet sheet;
  sheet.size = SheetSize::A3;
  sheet.landscape = false;
  const SheetChrome portrait = ChromeOf(sheet);
  CHECK(portrait.extent == glm::dvec2{297.0, 420.0});
  CHECK(portrait.borderMin == glm::dvec2{10.0, 10.0});
  CHECK(portrait.borderMax == glm::dvec2{287.0, 410.0});

  sheet.landscape = true;
  const SheetChrome landscape = ChromeOf(sheet);
  CHECK(landscape.extent == glm::dvec2{420.0, 297.0});
  // Title block pinned to the border's bottom-right corner, 160x32.
  CHECK(landscape.titleBlockMax.x == Catch::Approx(410.0));
  CHECK(landscape.titleBlockMin.x == Catch::Approx(410.0 - 160.0));
  CHECK(landscape.titleBlockMin.y == Catch::Approx(10.0));
  CHECK(landscape.titleBlockMax.y == Catch::Approx(42.0));

  // A sheet narrower than the block clamps the width to the border span.
  Sheet letter;
  letter.size = SheetSize::Letter;
  letter.landscape = false; // 215.9 wide; border span 195.9 > 160 - no clamp
  CHECK(ChromeOf(letter).titleBlockMax.x - ChromeOf(letter).titleBlockMin.x ==
        Catch::Approx(160.0));
}

TEST_CASE("ValidateDrawing flags layout problems and stays quiet when clean",
          "[drawing][validate]") {
  DrawingDocument dwg;
  dwg.GetSheet().size = SheetSize::A3;
  dwg.GetSheet().landscape = true; // extent 420 x 297, border 10..410/287

  DrawingView a;
  a.name = "front";
  const uint64_t aId = dwg.AddView(a);
  DrawingView b;
  b.name = "top";
  const uint64_t bId = dwg.AddView(b);

  const auto mkView = [](uint64_t id, glm::dvec2 lo, glm::dvec2 hi) {
    drawing::SheetView sv;
    sv.viewId = id;
    sv.bboxMin = lo;
    sv.bboxMax = hi;
    sv.hasGeometry = true;
    sv.solvedSourceVersion = 1;
    return sv;
  };
  const auto kinds = [](const std::vector<drawing::DrawingIssue>& issues) {
    std::vector<std::string> k;
    for (const auto& i : issues)
      k.push_back(i.kind);
    return k;
  };

  SECTION("clean layout: no issues") {
    std::map<uint64_t, drawing::SheetView> renders;
    renders[aId] = mkView(aId, {50, 150}, {200, 250});
    renders[bId] = mkView(bId, {50, 60}, {200, 140});
    const auto issues = drawing::ValidateDrawing(dwg, renders, {}, {});
    CHECK(issues.empty());
  }

  SECTION("off-border, overlap, and title-block collisions are reported") {
    std::map<uint64_t, drawing::SheetView> renders;
    renders[aId] = mkView(aId, {300, 5}, {430, 100});  // off right + bottom
    renders[bId] = mkView(bId, {290, 20}, {405, 120}); // overlaps a + block
    const auto issues = drawing::ValidateDrawing(dwg, renders, {}, {});
    const auto k = kinds(issues);
    CHECK(std::count(k.begin(), k.end(), "view_outside_border") >= 1);
    CHECK(std::count(k.begin(), k.end(), "view_overlap") == 1);
    CHECK(std::count(k.begin(), k.end(), "view_overlaps_title_block") == 2);
  }

  SECTION("touching bboxes are not an overlap") {
    std::map<uint64_t, drawing::SheetView> renders;
    renders[aId] = mkView(aId, {50, 150}, {200, 250});
    renders[bId] = mkView(bId, {200, 150}, {280, 250}); // shares x=200 edge
    const auto issues = drawing::ValidateDrawing(dwg, renders, {}, {});
    CHECK(issues.empty());
  }

  SECTION("unprojected and stale views are reported") {
    std::map<uint64_t, drawing::SheetView> renders;
    renders[aId] = mkView(aId, {50, 150}, {200, 250});
    // bId missing from renders entirely.
    const auto issues =
        drawing::ValidateDrawing(dwg, renders, {}, {{aId, true}});
    const auto k = kinds(issues);
    CHECK(std::count(k.begin(), k.end(), "unprojected_view") == 1);
    CHECK(std::count(k.begin(), k.end(), "stale_view") == 1);
  }

  SECTION("dim issues: dangling and off-paper") {
    std::map<uint64_t, drawing::SheetView> renders;
    renders[aId] = mkView(aId, {50, 150}, {200, 250});
    renders[bId] = mkView(bId, {50, 60}, {200, 140});
    drawing::ResolvedDim dangling;
    dangling.id = 7;
    dangling.resolved = false;
    drawing::ResolvedDim off;
    off.id = 8;
    off.resolved = true;
    off.kind = DrawingDimKind::Distance;
    off.dim1 = {50, 5}; // below the border
    off.dim2 = {200, 5};
    off.labelPos = {125, 5};
    off.text = "150";
    const auto issues =
        drawing::ValidateDrawing(dwg, renders, {dangling, off}, {});
    const auto k = kinds(issues);
    CHECK(std::count(k.begin(), k.end(), "dangling_dim") == 1);
    CHECK(std::count(k.begin(), k.end(), "dim_outside_border") == 1);
  }
}

TEST_CASE("DefaultDimOffset places dimensions clear of the view",
          "[drawing][dimension][placement]") {
  drawing::SheetView view = DimTestView();
  view.bboxMin = {0.0, 0.0};
  view.bboxMax = {30.0, 20.0};

  const auto place = [&](DrawingDim d) {
    d.labelOffset = drawing::DefaultDimOffset(d, view);
    return drawing::ResolveDim(d, view);
  };

  SECTION("distance_x lands a gap below the bbox") {
    const auto rd = place(
        MakeDim(DrawingDimKind::DistanceX, {EdgeRef(1, "L"), EdgeRef(1, "R")}));
    REQUIRE(rd.resolved);
    CHECK(rd.dim1.y ==
          Catch::Approx(view.bboxMin.y - drawing::kDimOffsetGapMM));
    CHECK(rd.dim2.y == rd.dim1.y);
    CHECK(rd.labelPos.y < view.bboxMin.y);
  }

  SECTION("distance lands its whole line below the bbox") {
    const auto rd = place(
        MakeDim(DrawingDimKind::Distance, {EdgeRef(1, "L"), EdgeRef(1, "R")}));
    REQUIRE(rd.resolved);
    CHECK(std::max(rd.dim1.y, rd.dim2.y) <=
          view.bboxMin.y - drawing::kDimOffsetGapMM + 1e-9);
  }

  SECTION("distance_y lands a gap left of the bbox") {
    const auto rd = place(
        MakeDim(DrawingDimKind::DistanceY, {EdgeRef(1, "L"), EdgeRef(1, "R")}));
    REQUIRE(rd.resolved);
    CHECK(rd.dim1.x ==
          Catch::Approx(view.bboxMin.x - drawing::kDimOffsetGapMM));
  }

  SECTION("diameter leader points away from the view centre") {
    const auto rd =
        place(MakeDim(DrawingDimKind::Diameter, {EdgeRef(2, "hole")}));
    REQUIRE(rd.resolved);
    // Circle centre (10,10) is left of the bbox centre (15,10): the leader
    // must run further left, label beyond the rim.
    CHECK(rd.rim.x < rd.center.x);
    CHECK(rd.labelPos.x < rd.rim.x);
  }

  SECTION("unresolvable dims fall back to the historical default") {
    DrawingDim d = MakeDim(DrawingDimKind::DistanceX,
                           {EdgeRef(1, "L"), EdgeRef(9, "gone")});
    const glm::dvec2 off = drawing::DefaultDimOffset(d, view);
    CHECK(off.x == 0.0);
    CHECK(off.y == Catch::Approx(-12.0));
  }
}
