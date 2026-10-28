// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestPaths.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/drawing/DrawingValidate.h"
#include "core/drawing/ViewProjection.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
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
    const fs::path proj = fs::temp_directory_path() / "dwg_rt.lob";
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
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v30-drawing.json";
  REQUIRE(fs::exists(golden));

  std::string err;
  auto dwg = testio::LoadDrawing(golden.string());
  INFO(err);
  REQUIRE(dwg != nullptr);
  CheckSampleFields(*dwg);

  // Round-trip through the current writer (both forms), re-check semantics.
  const fs::path tmp = fs::temp_directory_path() / "dwg_golden_rt.lob";
  fs::remove_all(tmp);
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE((reloaded != nullptr && err.empty()));
  CheckSampleFields(*reloaded);
  fs::remove_all(tmp);
}

TEST_CASE("FormatLength/FormatAngle produce compact display text",
          "[drawing][format][units]") {
  const units::System mm; // the default: millimetres, degrees
  CHECK(core::FormatLength(40.0, mm) == "40");
  CHECK(core::FormatLength(8.5, mm) == "8.5");
  CHECK(core::FormatLength(12.344, mm) == "12.34");
  // Decimals, not significant figures: a 37.34 hole spacing read 37.3, and
  // a 1051 mm frame would have read "1.05e+03".
  CHECK(core::FormatLength(2.0 * 18.66762, mm) == "37.34");
  CHECK(core::FormatLength(1051.09, mm) == "1051.09");
  // A deviation is a decimal finer: a K7's +0.007 must not read 0.01.
  CHECK(core::FormatDeviation(0.007, mm) == "0.007");
  CHECK(core::FormatLength(-0.0, mm) == "0");
  CHECK(core::FormatLength(0.0, mm) == "0");
  CHECK(core::FormatAngle(0.0, mm) == "0.0\xC2\xB0");
  CHECK(core::FormatAngle(M_PI / 2, mm) == "90.0\xC2\xB0");
  CHECK(core::FormatAngle(M_PI / 4, mm) == "45.0\xC2\xB0");

  // The bug this signature exists for: these ignored the document entirely
  // and printed millimetres, so an inch part's drawing was dimensioned in mm
  // with nothing on the sheet to say so.
  units::System in;
  in.length = units::Length::Inch;
  CHECK(core::FormatLength(25.4, in) == "1");
  CHECK(core::FormatLength(50.8, in) == "2");
  CHECK(core::FormatLength(12.7, in) == "0.5");
  // Still no suffix — the title block states the unit once (see UnitsRow).
  CHECK(core::FormatLength(25.4, in).find("in") == std::string::npos);

  // A document that asked for radians gets radians, at a precision that
  // matches: a tenth of a radian is 5.7 degrees.
  units::System rad;
  rad.angle = units::Angle::Radian;
  CHECK(core::FormatAngle(M_PI / 2, rad) == "1.571 rad");
  CHECK(core::FormatAngle(0.0, rad) == "0.000 rad");
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

// An END is that end, even of a circle: a circle seen edge-on is drawn as a
// line, and its ends answered with the circle's centre -- the two ends of a
// turned chess piece's underside measured its radius. Without `end` a circle
// still means its centre.
TEST_CASE("A circle's end is its end, not its centre", "[drawing][dimension]") {
  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  drawing::SheetCurve underside; // a Phi 38 circle seen edge-on
  underside.edge = ElementRef{3, "under"};
  underside.sourceId = 1;
  underside.curveType = 1;
  underside.conicCenter = {40, 245};
  underside.conicRadius = 19.0;
  underside.points = {{40, 264}, {40, 245}, {40, 226}};
  underside.visible = true;
  // ...and the hidden half HLR returns for the same edge, back to the start:
  // an end is the reported (visible) segment's, not the joined ones'.
  drawing::SheetCurve hidden = underside;
  hidden.visible = false;
  hidden.points = {{40, 226}, {40, 245}, {40, 264}};
  view.curves.insert(view.curves.begin(), hidden);
  view.curves.push_back(underside);
  ProjRef a = EdgeRef(3, "under"), b = EdgeRef(3, "under");
  a.end = 1;
  b.end = 2;
  const drawing::ResolvedDim d =
      drawing::ResolveDim(MakeDim(DrawingDimKind::DistanceY, {a, b}), view);
  REQUIRE(d.resolved);
  CHECK_THAT(d.value, Catch::Matchers::WithinAbs(38.0, 1e-9));
  ProjRef whole = EdgeRef(3, "under");
  ProjRef top = EdgeRef(3, "under");
  top.end = 1;
  const drawing::ResolvedDim r = drawing::ResolveDim(
      MakeDim(DrawingDimKind::DistanceY, {whole, top}), view);
  REQUIRE(r.resolved);
  CHECK_THAT(r.value, Catch::Matchers::WithinAbs(19.0, 1e-9)); // centre to top
}

// The top of a knight's head is the interior of its outline curve: both ends
// sit on the base, and the midpoint is the bbox centre. `extreme` is the
// curve's point furthest toward a side of the sheet.
TEST_CASE("A ref's extreme is the curve's furthest point that way",
          "[drawing][dimension]") {
  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  drawing::SheetCurve base; // the base's top, a line at x = 19.4
  base.edge = ElementRef{2, "base"};
  base.sourceId = 1;
  base.points = {{19.4, -8}, {19.4, 8}};
  base.visible = true;
  drawing::SheetCurve head = base; // an arch out to x = 54.4 and back
  head.edge = ElementRef{2, "head"};
  head.curveType = 1;
  head.points = {{19.4, -5}, {40, -4}, {54.4, 1}, {45, 4}, {19.4, 5}};
  view.curves = {base, head};
  ProjRef from = EdgeRef(2, "base"), to = EdgeRef(2, "head");
  to.extreme = "right";
  const drawing::ResolvedDim d =
      drawing::ResolveDim(MakeDim(DrawingDimKind::DistanceX, {from, to}), view);
  REQUIRE(d.resolved);
  CHECK_THAT(d.value, Catch::Matchers::WithinAbs(35.0, 1e-9));
  to.extreme = "top";
  const drawing::ResolvedDim y = drawing::ResolveDim(
      MakeDim(DrawingDimKind::DistanceY, {EdgeRef(2, "base"), to}), view);
  REQUIRE(y.resolved);
  CHECK_THAT(y.value, Catch::Matchers::WithinAbs(5.0, 1e-9)); // 0 to 5
  CHECK(IsProjRefExtreme("bottom"));
  CHECK_FALSE(IsProjRefExtreme("up"));
}

TEST_CASE("A fastener callout names the standard part it points at",
          "[drawing][dimension][parts]") {
  // docs/parts/README.md#bom-and-drawings. The text is not composed here and
  // not typed by anyone: it is the occurrence's own Fastener feature's
  // designation, which is the same string the BOM and a purchase order carry.
  // A drawing that says "M6x50" while the BOM says "M6x45" is the exact
  // divergence a derived callout exists to prevent.
  drawing::SheetView view = DimTestView();
  view.curves[2].sourceId = 42; // the circle belongs to occurrence 42

  Document screwDoc;
  screwDoc.AddFeature(
      std::make_unique<FastenerFeature>("Screw", "iso4762", "M6", 50.0));
  Document plateDoc;
  plateDoc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0.0f), 10.0, 10.0, 10.0));

  drawing::DimContext ctx;
  ctx.occurrenceDoc = [&](uint64_t id) -> const Document* {
    return id == 42 ? &screwDoc : &plateDoc;
  };

  ProjRef ref = EdgeRef(2, "hole");
  ref.occurrenceId = 42;
  auto rd = drawing::ResolveDim(MakeDim(DrawingDimKind::FastenerCallout, {ref}),
                                view, ctx);
  REQUIRE(rd.resolved);
  CHECK(rd.leader);
  CHECK(rd.text == "ISO 4762 M6x50");
  // Anchored on the picked edge, whatever it is. A screw is a circle from
  // above and two lines from the side, so a callout that only worked on a
  // circle could not be placed on the section anybody actually draws.
  CHECK(rd.center.x == Catch::Approx(10.0));

  // Everything that could make it name the WRONG part leaves it dangling
  // instead, which is the hole callout's rule: a drawing with a visible gap
  // beats one that confidently labels a screw that is not there.
  SECTION("an occurrence that is not a bought fastener") {
    ProjRef plate = EdgeRef(2, "hole");
    plate.occurrenceId = 7; // resolves to the plate
    view.curves[2].sourceId = 7;
    CHECK_FALSE(
        drawing::ResolveDim(MakeDim(DrawingDimKind::FastenerCallout, {plate}),
                            view, ctx)
            .resolved);
  }
  SECTION("a PART view, which has no occurrence to name") {
    CHECK_FALSE(drawing::ResolveDim(MakeDim(DrawingDimKind::FastenerCallout,
                                            {EdgeRef(2, "hole")}),
                                    view, ctx)
                    .resolved);
  }
  SECTION("no resolver at all") {
    CHECK_FALSE(drawing::ResolveDim(
                    MakeDim(DrawingDimKind::FastenerCallout, {ref}), view)
                    .resolved);
  }
}

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

  // A dimension states the size of the PART, not of the paper it covers.
  // Distances and radii used to report sheet millimetres, which matched the
  // model only while every view sat at 1:1 (found by the 2026-08-13 clicks-
  // only drawing round, once views started auto-scaling).
  SECTION("values are model size, not sheet size, at a non-1:1 scale") {
    drawing::DimContext ctx;
    ctx.viewScale = 20.0; // 20:1 enlargement
    auto dx = drawing::ResolveDim(
        MakeDim(DrawingDimKind::DistanceX, {EdgeRef(1, "L"), EdgeRef(1, "R")}),
        view, ctx);
    REQUIRE(dx.resolved);
    CHECK(dx.value == Catch::Approx(1.5)); // 30 mm of paper / 20
    CHECK(dx.text == "1.5");
    auto dia = drawing::ResolveDim(
        MakeDim(DrawingDimKind::Diameter, {EdgeRef(2, "hole")}), view, ctx);
    REQUIRE(dia.resolved);
    CHECK(dia.value == Catch::Approx(0.4));
    CHECK(dia.radius == Catch::Approx(4.0)); // drawn at sheet size, though
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
  // Every dimension on a sheet is a bare number, so the sheet has to say what
  // they are in. It did not, and an inch part's drawing printed millimetres
  // over a title block of PROJECT / DOCUMENT / AUTHOR / DATE.
  CHECK(svg.find(">UNITS<") != std::string::npos);
  CHECK(svg.find(">mm<") != std::string::npos);
  fs::remove(out);

  units::System inches;
  inches.length = units::Length::Inch;
  const fs::path outIn = fs::temp_directory_path() / "drawing_export_in.svg";
  REQUIRE(DrawingExport::SaveSVG(doc, {view}, {}, outIn.string(), &err, {},
                                 inches));
  std::ifstream inFile(outIn);
  std::stringstream si;
  si << inFile.rdbuf();
  CHECK(si.str().find(">in<") != std::string::npos);
  fs::remove(outIn);

  // A sheet with notes and no dimensions still writes its notes: they sat
  // inside the dimension group, so a notes-only sheet exported none.
  drawing::ResolvedNote note;
  note.text = "DEBURR ALL EDGES";
  note.pos = {100, 100};
  const fs::path outNote = fs::temp_directory_path() / "drawing_note.svg";
  REQUIRE(
      DrawingExport::SaveSVG(doc, {view}, {}, outNote.string(), &err, {note}));
  std::ifstream noteFile(outNote);
  std::stringstream sn;
  sn << noteFile.rdbuf();
  CHECK(sn.str().find(">DEBURR ALL EDGES<") != std::string::npos);
  fs::remove(outNote);
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

  // Latin-1 letters and a dash: a Swedish caption must reach the PDF as the
  // WinAnsi bytes, not as '?' (it did, while the SVG was right).
  drawing::ResolvedDim text = dim;
  text.id = 2;
  text.text = "K\xC3\xA4llare \xE2\x80\x93 S\xC3\x96"
              "DER";
  text.labelPos = {48, 60};

  const fs::path out = fs::temp_directory_path() / "drawing_export.pdf";
  std::string err;
  REQUIRE(DrawingExport::SavePDF(doc, {view}, {dim, text}, out.string(), &err));
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
  CHECK(pdf.find("K\xE4llare \x96 S\xD6"
                 "DER") != std::string::npos);
  CHECK(pdf.find("K?llare") == std::string::npos);
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
      fs::path(TestSourceDir()) / "tests" / "golden" / "v17-full.json";
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
  // Title block pinned to the border's bottom-right corner, 160x48 (eight
  // rows since v147).
  CHECK(landscape.titleBlockMax.x == Catch::Approx(410.0));
  CHECK(landscape.titleBlockMin.x == Catch::Approx(410.0 - 160.0));
  CHECK(landscape.titleBlockMin.y == Catch::Approx(10.0));
  CHECK(landscape.titleBlockMax.y == Catch::Approx(58.0));

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

TEST_CASE("Callout provenance: MappedName parsing, hole text, pitch",
          "[drawing][callout]") {
  SECTION("MappedNameFeatureIds over the naming corpus") {
    using drawing::MappedNameFeatureIds;
    CHECK(MappedNameFeatureIds("14/face0") == std::vector<uint64_t>{14});
    CHECK(MappedNameFeatureIds("14/seam0") == std::vector<uint64_t>{14});
    CHECK(MappedNameFeatureIds("14/face0;U2") == std::vector<uint64_t>{14});
    CHECK(MappedNameFeatureIds("3/cap1;M14") == std::vector<uint64_t>({3, 14}));
    CHECK(MappedNameFeatureIds("2/cap0;G4.0") == std::vector<uint64_t>({2, 4}));
    CHECK(MappedNameFeatureIds("4/face1;U2;M4") ==
          std::vector<uint64_t>({4, 4}));
    CHECK(MappedNameFeatureIds("a;U0,b;U1;L").empty());
    CHECK(MappedNameFeatureIds("").empty());
    CHECK(MappedNameFeatureIds("garbage").empty());
  }

  SECTION("FormatHoleCallout for every kind") {
    HoleFeature simple("h", 1, HoleKind::Simple, 4.5, 6.0);
    CHECK(drawing::FormatHoleCallout(simple) == "\xC3\x98"
                                                "4.5 DEEP 6");
    simple.through = true;
    CHECK(drawing::FormatHoleCallout(simple) == "\xC3\x98"
                                                "4.5 THRU");
    HoleFeature cbore("h", 1, HoleKind::Counterbore, 4.5, 6.0);
    cbore.through = true;
    cbore.cbDiameter = 9.0;
    cbore.cbDepth = 3.0;
    CHECK(drawing::FormatHoleCallout(cbore) == "\xC3\x98"
                                               "4.5 THRU CBORE \xC3\x98"
                                               "9 DEEP 3");
    HoleFeature csk("h", 1, HoleKind::Countersink, 4.5, 6.0);
    csk.through = true;
    csk.csDiameter = 9.5;
    csk.csDepth = 2.5; // 2*atan2(2.5, 2.5) = 90 deg
    CHECK(drawing::FormatHoleCallout(csk) == "\xC3\x98"
                                             "4.5 THRU CSK \xC3\x98"
                                             "9.5 \xC3\x97 90.0\xC2\xB0");
  }

  // A hole that knows what it is FOR says so. Everything above is a hole
  // with no fit and must be untouched by this: a drawing of an old document
  // reads exactly as it did.
  SECTION("FormatHoleCallout states a recorded fit") {
    HoleFeature clearance("h", 1, HoleKind::Simple, 3.4, 6.0);
    clearance.through = true;
    clearance.fitThread = "M3";
    clearance.fitKind = "normal";
    CHECK(drawing::FormatHoleCallout(clearance) == "\xC3\x98"
                                                   "3.4 THRU, M3 NORMAL "
                                                   "CLEARANCE");
    // Named even when normal: "M3 clearance" conventionally IMPLIES normal,
    // and an implication is what recording the fit did away with.
    clearance.fitKind = "close";
    CHECK(drawing::FormatHoleCallout(clearance) == "\xC3\x98"
                                                   "3.4 THRU, M3 CLOSE "
                                                   "CLEARANCE");

    // The clause goes AFTER the recess, so the callout still reads as an
    // instruction first.
    HoleFeature cb("h", 1, HoleKind::Counterbore, 3.4, 6.0);
    cb.through = true;
    cb.cbDiameter = 6.5;
    cb.cbDepth = 3.3;
    cb.fitThread = "M3";
    cb.fitKind = "normal";
    CHECK(drawing::FormatHoleCallout(cb) == "\xC3\x98"
                                            "3.4 THRU CBORE \xC3\x98"
                                            "6.5 DEEP 3.3, M3 NORMAL "
                                            "CLEARANCE");

    // A TAPPED hole leads with its thread. Printing the tap drill would not
    // merely be unhelpful, it is the wrong instruction: nobody makes an M3
    // tapped hole by drilling 2.5 and stopping.
    HoleFeature tapped("h", 1, HoleKind::Simple, 2.5, 8.0);
    tapped.fitThread = "M3";
    tapped.fitKind = "tapped";
    CHECK(drawing::FormatHoleCallout(tapped) == "M3 DEEP 8");
    tapped.through = true;
    CHECK(drawing::FormatHoleCallout(tapped) == "M3 THRU");
    // A stated pitch is printed; the coarse one (0) is not, because "M3"
    // alone already means it and spelling it out invites the reader to ask
    // what the silent one meant.
    tapped.fitPitch = 0.35;
    CHECK(drawing::FormatHoleCallout(tapped) == "M3\xC3\x97"
                                                "0.35 THRU");

    // Half a fit is no fit: a thread with no kind cannot say what it is for.
    HoleFeature half("h", 1, HoleKind::Simple, 3.4, 6.0);
    half.through = true;
    half.fitThread = "M3";
    CHECK(drawing::FormatHoleCallout(half) == "\xC3\x98"
                                              "3.4 THRU");
  }

  // A dimension on a toleranced bore says so. This is provenance, not a new
  // authored field: the model already recorded the band, and the sheet had
  // gone on printing the nominal alone.
  SECTION("FormatToleranceSuffix") {
    Tolerance cls;
    cls.isoClass = "H7";
    CHECK(drawing::FormatToleranceSuffix(cls) == " H7");

    Tolerance band;
    band.lower = -0.02;
    band.upper = 0.05;
    // The leading + is not decoration: "0.05/-0.02" reads as a range.
    CHECK(drawing::FormatToleranceSuffix(band) == " +0.05/-0.02");

    Tolerance negBoth;
    negBoth.lower = -0.041;
    negBoth.upper = -0.02;
    CHECK(drawing::FormatToleranceSuffix(negBoth) == " -0.02/-0.041");

    // A distance is not a size, and never labels a circle.
    Tolerance dist;
    dist.ref = ElementRef{1, "a"};
    dist.ref2 = ElementRef{1, "b"};
    dist.lower = -0.1;
    dist.upper = 0.1;
    CHECK(drawing::FormatToleranceSuffix(dist).empty());
  }

  SECTION("UniformCollinearPitch") {
    using drawing::UniformCollinearPitch;
    const glm::dvec2 row[] = {{0, 0}, {10, 0}, {20, 0}};
    REQUIRE(UniformCollinearPitch(row).has_value());
    CHECK(*UniformCollinearPitch(row) == Catch::Approx(10.0));
    const glm::dvec2 uneven[] = {{0, 0}, {10, 0}, {25, 0}};
    CHECK_FALSE(UniformCollinearPitch(uneven).has_value());
    const glm::dvec2 triangle[] = {{0, 0}, {10, 0}, {5, 8}};
    CHECK_FALSE(UniformCollinearPitch(triangle).has_value());
    const glm::dvec2 pair[] = {{0, 0}, {0, 32}};
    CHECK(*UniformCollinearPitch(pair) == Catch::Approx(32.0));
    const glm::dvec2 lone[] = {{5, 5}};
    CHECK_FALSE(UniformCollinearPitch(lone).has_value());
  }
}

TEST_CASE("Hole rim element names carry the hole feature id",
          "[drawing][callout][integration]") {
  // The load-bearing assumption behind hole callouts, pinned in-tree: every
  // circular edge a HoleFeature makes mentions the hole's id in its
  // MappedName (prefix "<id>/..." or ";G<id>"/";M<id>" suffix).
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Base", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {40, 20});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", skId, 5.0, ExtrudeMode::NewBody));
  auto centers = std::make_unique<SketchFeature>(
      "Centers", SketchPlane(PlaneType::XY, glm::vec3(0, 0, 5)));
  centers->AddEntity(sketch::Point{{10, 10}});
  doc.AddFeature(std::move(centers));
  const uint64_t centersId = doc.GetFeature(2)->GetId();
  auto hole = std::make_unique<HoleFeature>("Drill", centersId,
                                            HoleKind::Countersink, 4.5, 5.0);
  hole->through = true;
  hole->csDiameter = 9.5;
  hole->csDepth = 2.5;
  doc.AddFeature(std::move(hole));
  const uint64_t holeId = doc.GetFeature(3)->GetId();

  REQUIRE(doc.GetBodies().size() == 1);
  const ElementMap& map = doc.GetBodies()[0].elementMap;
  int holeNamedEdges = 0;
  for (const auto& [index, name] : map.edgeNames) {
    const auto ids = drawing::MappedNameFeatureIds(name);
    if (std::find(ids.begin(), ids.end(), holeId) != ids.end())
      ++holeNamedEdges;
  }
  CHECK(holeNamedEdges >= 2); // bore rim(s) + countersink rim at least

  // And FindHoleForRef resolves one of those names with the bore radius.
  const HoleFeature* found = nullptr;
  for (const auto& [index, name] : map.edgeNames) {
    found = drawing::FindHoleForRef(doc, name, 4.5 / 2.0);
    if (found)
      break;
  }
  REQUIRE(found != nullptr);
  CHECK(found->GetId() == holeId);
  // A radius that matches nothing keeps missing (never mislabels).
  CHECK(drawing::FindHoleForRef(doc, "2/cap0;G" + std::to_string(holeId),
                                3.14159) == nullptr);
}

TEST_CASE("a note's box is measured per line", "[drawing][dimension]") {
  // A note is one string that may carry newlines. Boxing the whole thing as
  // one line made a 200-character hole table 440 mm wide, so every
  // multi-line note on an A3 sheet reported as running off the border —
  // which is why a quadcopter's sheets emit one note per line.
  glm::dvec2 mn, mx, mn2, mx2;
  drawing::LabelBox({100.0, 100.0}, "ABCDEFGHIJ", mn, mx);
  drawing::LabelBox({100.0, 100.0}, "ABCD\nEFGH\nIJ", mn2, mx2);
  CHECK(mx2.x - mn2.x < mx.x - mn.x); // as wide as its widest line
  CHECK_THAT(mx2.x - mn2.x, Catch::Matchers::WithinAbs(4 * 2.2, 1e-9));
  CHECK_THAT(mx2.y - mn2.y,
             Catch::Matchers::WithinAbs(3 * drawing::kLabelLineMM, 1e-9));
  CHECK_THAT(mx2.y, Catch::Matchers::WithinAbs(mx.y, 1e-9)); // grows DOWNWARD
}

TEST_CASE("a callout is auto-placed clear of the view",
          "[drawing][callout][dimension]") {
  // Auto-placement re-resolved the dimension itself, with no DimContext —
  // and a callout needs one to resolve at all. Every hole callout therefore
  // fell through to the fallback offset and landed 12 mm from its own hole,
  // which is on the part. A quad's mid plate is where it showed: two bores,
  // one label on the plate. Now the caller resolves it and passes it in.
  drawing::SheetView view = DimTestView();
  view.bboxMin = {0.0, 0.0};
  view.bboxMax = {30.0, 20.0};
  for (int i = 1; i < 3; ++i) {
    drawing::SheetCurve c;
    c.edge = ElementRef{2, "hole" + std::to_string(i)};
    c.sourceId = 1;
    c.curveType = 1;
    c.conicCenter = {10.0 + 4.0 * i, 10.0};
    c.conicRadius = 4.0;
    c.closed = true;
    c.points = {{14.0 + 4.0 * i, 10}, {6.0 + 4.0 * i, 10}};
    view.curves.push_back(c);
  }
  DrawingDim dim =
      MakeDim(DrawingDimKind::PatternCallout,
              {EdgeRef(2, "hole"), EdgeRef(2, "hole1"), EdgeRef(2, "hole2")});
  drawing::DimPlaceContext ctx;
  ctx.borderMin = {-100.0, -100.0};
  ctx.borderMax = {200.0, 200.0};
  ctx.avoid.push_back({view.bboxMin, view.bboxMax});

  DrawingDim probe = dim;
  probe.labelOffset = {0.0, 0.0};
  const drawing::ResolvedDim rd = drawing::ResolveDim(probe, view);
  REQUIRE(rd.resolved);
  dim.labelOffset = drawing::PlaceDimOffset(dim, view, ctx, &rd);

  const drawing::ResolvedDim placed = drawing::ResolveDim(dim, view);
  REQUIRE(placed.resolved);
  glm::dvec2 mn, mx;
  drawing::LabelBox(placed.labelPos, placed.text, mn, mx);
  INFO("label at " << placed.labelPos.x << ", " << placed.labelPos.y);
  CHECK((mx.x < view.bboxMin.x || mn.x > view.bboxMax.x ||
         mx.y < view.bboxMin.y || mn.y > view.bboxMax.y));
}

TEST_CASE("ResolveDim callout kinds", "[drawing][callout][dimension]") {
  drawing::SheetView view = DimTestView();
  // Three equal circles in a row for the pattern (r=4, pitch 12).
  for (int i = 1; i < 3; ++i) {
    drawing::SheetCurve c;
    c.edge = ElementRef{2, "hole" + std::to_string(i)};
    c.sourceId = 1;
    c.curveType = 1;
    c.conicCenter = {10.0 + 12.0 * i, 10.0};
    c.conicRadius = 4.0;
    c.closed = true;
    c.points = {{14.0 + 12.0 * i, 10}, {6.0 + 12.0 * i, 10}};
    view.curves.push_back(c);
  }

  SECTION("hole callout without a source document dangles") {
    const auto rd = drawing::ResolveDim(
        MakeDim(DrawingDimKind::HoleCallout, {EdgeRef(2, "hole")}), view);
    CHECK_FALSE(rd.resolved);
  }

  SECTION("pattern callout: N x diameter + uniform pitch, leader laid out") {
    DrawingDim d =
        MakeDim(DrawingDimKind::PatternCallout,
                {EdgeRef(2, "hole"), EdgeRef(2, "hole1"), EdgeRef(2, "hole2")});
    d.labelOffset = {0.0, 14.0};
    const auto rd = drawing::ResolveDim(d, view);
    REQUIRE(rd.resolved);
    CHECK(rd.leader);
    CHECK(rd.text == "3\xC3\x97 \xC3\x98"
                     "8 PITCH 12");
    CHECK(rd.value == Catch::Approx(12.0));
  }

  SECTION("pattern callout with mixed radii dangles") {
    view.curves[2].conicRadius = 3.0; // the first 'hole' curve
    const auto rd =
        drawing::ResolveDim(MakeDim(DrawingDimKind::PatternCallout,
                                    {EdgeRef(2, "hole"), EdgeRef(2, "hole1")}),
                            view);
    CHECK_FALSE(rd.resolved);
  }
}

TEST_CASE("Drawing golden loads and round-trips (v53 callouts)",
          "[drawing][serialization][golden]") {
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v53-callouts.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->DimCount() == 2);
  CHECK(dwg->GetDims()[0].kind == DrawingDimKind::HoleCallout);
  CHECK(dwg->GetDims()[1].kind == DrawingDimKind::PatternCallout);
  CHECK(dwg->GetDims()[1].refs.size() == 3);

  const fs::path tmp = fs::temp_directory_path() / "dwg_v53_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->GetDims()[0].kind == DrawingDimKind::HoleCallout);
  CHECK(reloaded->GetDims()[1].kind == DrawingDimKind::PatternCallout);
  fs::remove(tmp);
}

TEST_CASE("Drawing golden loads and round-trips (v89 view quality)",
          "[drawing][serialization][golden]") {
  // v89 added a view's `quality`. The golden carries all three cases that
  // matter: one view saying "exact", one saying "fast", and one saying
  // NOTHING — a view written before v89, which must read as exact rather
  // than as an empty string that matches neither branch.
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v89-view-quality.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->ViewCount() == 3);
  CHECK(dwg->GetViews()[0].quality == "exact");
  CHECK(dwg->GetViews()[1].quality == "fast");
  CHECK(dwg->GetViews()[2].quality == "exact"); // absent = the default

  const fs::path tmp = fs::temp_directory_path() / "dwg_v89_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->GetViews()[1].quality == "fast");
  // And the round trip WRITES the default rather than dropping it, so a file
  // saved once says outright which projection made it.
  CHECK(reloaded->GetViews()[2].quality == "exact");
  fs::remove(tmp);
}

TEST_CASE("Drawing golden loads and round-trips (v115 flat pattern sheet)",
          "[drawing][serialization][golden]") {
  // v115 added a view's `configuration` and a note's `table`. The golden has
  // a view of the part as it is (no key: whatever it has active) beside a
  // view of its Flat configuration, a bend table on that view, and a plain
  // note that says nothing about tables.
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v115-flat-pattern-sheet.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->ViewCount() == 2);
  CHECK(dwg->GetViews()[0].configuration == 0);
  CHECK(dwg->GetViews()[1].configuration == 12);
  REQUIRE(dwg->NoteCount() == 2);
  CHECK(dwg->GetNotes()[0].table == NoteTable::Bends);
  CHECK(dwg->GetNotes()[0].viewId == 2);
  CHECK(dwg->GetNotes()[1].table == NoteTable::None);

  const fs::path tmp = fs::temp_directory_path() / "dwg_v115_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->GetViews()[1].configuration == 12);
  CHECK(reloaded->GetNotes()[0].table == NoteTable::Bends);
  fs::remove(tmp);

  // A table kind this build does not know is refused, not read as a note.
  std::ifstream in(golden);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  const std::string from = "\"table\": \"bends\"";
  text.replace(text.find(from), from.size(), "\"table\": \"holes\"");
  const fs::path bad = fs::temp_directory_path() / "dwg_v115_bad.json";
  std::ofstream(bad) << text;
  CHECK(testio::LoadDrawing(bad.string()) == nullptr);
  fs::remove(bad);
}

TEST_CASE("Drawing golden loads and round-trips (v134 view rotation)",
          "[drawing][serialization][golden]") {
  // v134 added a view's `rotation_deg`: a piece modelled along X drawn
  // upright. The second view has no key, which is 0.
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v134-view-rotation.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->ViewCount() == 2);
  CHECK(dwg->GetViews()[0].rotationDeg == 90.0);
  CHECK(dwg->GetViews()[1].rotationDeg == 0.0);
  // ...and a dimension ref's `extreme`, beside one that has none.
  REQUIRE(dwg->DimCount() == 1);
  CHECK(dwg->GetDims()[0].refs[0].extreme.empty());
  CHECK(dwg->GetDims()[0].refs[1].extreme == "top");

  const fs::path tmp = fs::temp_directory_path() / "dwg_v134_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->GetViews()[0].rotationDeg == 90.0);
  CHECK(reloaded->GetViews()[1].rotationDeg == 0.0);
  CHECK(reloaded->GetDims()[0].refs[1].extreme == "top");
  fs::remove(tmp);

  // An extreme this build does not know is refused, not read as none.
  std::ifstream in(golden);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  const std::string from = "\"extreme\": \"top\"";
  text.replace(text.find(from), from.size(), "\"extreme\": \"up\"");
  const fs::path bad = fs::temp_directory_path() / "dwg_v134_bad.json";
  std::ofstream(bad) << text;
  CHECK(testio::LoadDrawing(bad.string()) == nullptr);
  fs::remove(bad);
}

TEST_CASE("A view's rotation turns what it draws, not where it looks",
          "[drawing]") {
  DrawingView v;
  v.orientation = ViewOrientation::Front;
  glm::dvec3 dir0, up0, dir, up;
  ViewAxes(v, dir0, up0);
  v.rotationDeg = 90.0;
  ViewAxes(v, dir, up);
  CHECK(glm::length(dir - dir0) < 1e-12); // still looking the same way
  // Turned counter-clockwise by 90, what was drawn to the right is drawn up:
  // the world's right-hand vector is the new up.
  const glm::dvec3 right0 = glm::normalize(glm::cross(dir0, up0));
  CHECK(glm::length(up - right0) < 1e-12);
  // A point to the right of the origin lands above it on the sheet.
  const glm::dvec2 p = drawing::ProjectPoint(right0 * 10.0, dir, up);
  CHECK(std::abs(p.x) < 1e-9);
  CHECK(std::abs(p.y - 10.0) < 1e-9);
}

TEST_CASE("Drawing golden loads and round-trips (v90 hidden occurrences)",
          "[drawing][serialization][golden]") {
  // v90 added a view's `hidden`. The golden carries a view that drops two
  // occurrences and two that drop none, because "absent" and "empty" must
  // both read as draw-everything.
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v90-view-hidden.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->ViewCount() == 3);
  CHECK(dwg->GetViews()[0].hidden.empty());
  REQUIRE(dwg->GetViews()[1].hidden.size() == 2);
  CHECK(dwg->GetViews()[1].hidden[0] == 7);
  CHECK(dwg->GetViews()[1].hidden[1] == 9);
  CHECK(dwg->GetViews()[2].hidden.empty()); // absent = draw everything

  const fs::path tmp = fs::temp_directory_path() / "dwg_v90_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  REQUIRE(reloaded->GetViews()[1].hidden.size() == 2);
  CHECK(reloaded->GetViews()[0].hidden.empty());
  fs::remove(tmp);
}

TEST_CASE("a drawing refuses an unknown sheet size or view orientation",
          "[drawing][enums]") {
  // Verified against a running instance before the fix: an A2 drawing whose
  // size string did not parse loaded as A4 (297x210 instead of 594x420) with
  // no error, so every view's placement and scale was silently relative to
  // different paper. Driven through the real load entry point, which is the
  // path a hand-edited or merged file actually takes.
  const fs::path dir = fs::temp_directory_path() / "designer_enum_load";
  fs::create_directories(dir);
  const std::string ver = std::to_string(DocumentIO::DocumentFormatVersion());
  auto write = [&](const std::string& name, const std::string& body) {
    const fs::path p = dir / name;
    std::ofstream(p) << body;
    return p.string();
  };
  auto loadErr = [&](const std::string& path) {
    auto r = DocumentIO::LoadDrawing(path);
    REQUIRE_FALSE(r.has_value());
    return r.error();
  };

  const std::string head = R"({"format":"designer-document","version":)" + ver +
                           R"(,"kind":"drawing","dims":[],)";

  // A legitimate A2 sheet still loads.
  REQUIRE(
      testio::LoadDrawing(write(
          "good.json",
          head + R"("sheet":{"size":"a2","landscape":true},"views":[]})")) !=
      nullptr);

  const std::string sizeErr = loadErr(
      write("badsize.json", head + R"("sheet":{"size":"a2plus"},"views":[]})"));
  INFO("sheet error: " << sizeErr);
  CHECK(sizeErr.find("a2plus") != std::string::npos);
  CHECK(sizeErr.find("a2") != std::string::npos); // names what IS accepted

  const std::string orientErr = loadErr(write(
      "badorient.json",
      head + R"("sheet":{"size":"a3"},)"
             R"("views":[{"id":1,"name":"V","orientation":"frontish"}]})"));
  INFO("orientation error: " << orientErr);
  CHECK(orientErr.find("frontish") != std::string::npos);

  fs::remove_all(dir);
}

TEST_CASE("drawing notes resolve expressions and report stale references",
          "[drawing]") {
  DrawingNote n;
  n.text = "Hole {{bore}}; pitch {{pitch*2}}";
  auto r = drawing::ResolveNote(n, nullptr, {{"bore", 6}, {"pitch", 30}});
  REQUIRE(r.resolved);
  REQUIRE(r.text == "Hole 6; pitch 60");
  r = drawing::ResolveNote(n, nullptr, {{"bore", 8}, {"pitch", 32}});
  REQUIRE(r.text == "Hole 8; pitch 64");
  REQUIRE_FALSE(drawing::ResolveNote(n, nullptr).resolved);
  n.text = "{{1/0}}";
  REQUIRE_FALSE(drawing::ResolveNote(n, nullptr).resolved);
  n.text = "{{bore";
  REQUIRE_FALSE(drawing::ResolveNote(n, nullptr).resolved);
}

TEST_CASE("drawing validator checks note bounds and overlaps", "[drawing]") {
  DrawingDocument doc;
  const auto chrome = ChromeOf(doc.GetSheet());
  DrawingNote n;
  n.text = "Note";
  n.pos = (chrome.titleBlockMin + chrome.titleBlockMax) / 2.0;
  doc.AddNote(n);
  n.pos = {-10, -10};
  doc.AddNote(n);
  doc.AddNote(n);
  const auto issues = drawing::ValidateDrawing(doc, {}, {}, {});
  for (const auto* kind :
       {"note_over_title_block", "note_outside_border", "notes_overlap"}) {
    REQUIRE(std::any_of(issues.begin(), issues.end(), [&](const auto& i) {
      return i.kind == kind && i.noteId != 0;
    }));
  }
}

// v162 (docs/drawings/README.md#saved-projections): a drawing's directory
// form carries each view's last projection beside it. Loaded, every one is a
// CANDIDATE -- the loader cannot hash the source, so a foreign input hash is
// read like any other and left to the projecting job to discard; one that
// does not read at all is dropped with a warning, never a refusal. The
// single-file form (an autosave's) carries none.
TEST_CASE("Drawing golden carries its views' saved projections (v162)",
          "[drawing][serialization][golden]") {
  const fs::path golden =
      fs::path(TestSourceDir()) / "tests" / "golden" / "drawing";
  nlohmann::json expect;
  std::ifstream(golden / "v162-stored-projection.expect.json") >> expect;
  const fs::path dir = golden / expect.at("directory").get<std::string>();
  auto dwg = testio::LoadDrawing(dir.string());
  REQUIRE(dwg != nullptr);
  CHECK(dwg->ViewCount() == expect.at("views").get<size_t>());
  const auto check = [&](const DrawingDocument& d) {
    for (const auto& [id, e] : expect.at("saved_projections").items()) {
      INFO("view " << id);
      const auto p = d.SavedProjection(std::stoull(id));
      REQUIRE(p != nullptr);
      CHECK(inputhash::Hex(p->inputHash) ==
            e.at("input_hash").get<std::string>());
      CHECK(p->result.curves.size() == e.at("curves").get<size_t>());
      for (const auto& c : p->result.curves)
        CHECK(c.points.size() >= 2);
    }
  };
  check(*dwg);

  // Written back in the directory form: the same projections reopen.
  const fs::path tmp = fs::temp_directory_path() / "dwg_v162_rt.lob";
  fs::remove_all(tmp);
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto again = testio::LoadDrawing(tmp.string());
  REQUIRE(again != nullptr);
  check(*again);
  // A projection whose bin is torn is dropped, and the drawing still loads.
  for (const auto& e : fs::recursive_directory_iterator(tmp))
    if (e.path().filename() == "1-front.projection.bin")
      fs::resize_file(e.path(), 3);
  auto torn = testio::LoadDrawing(tmp.string());
  REQUIRE(torn != nullptr);
  CHECK(torn->ViewCount() == expect.at("views").get<size_t>());
  CHECK(torn->SavedProjection(1) == nullptr);
  CHECK(torn->SavedProjection(2) != nullptr);
  fs::remove_all(tmp);

  // The single-file form carries none, and saving to it writes none.
  auto single =
      testio::LoadDrawing((golden / "v162-stored-projection.json").string());
  REQUIRE(single != nullptr);
  CHECK(single->ViewCount() == expect.at("views").get<size_t>());
  CHECK(single->SavedProjections().size() ==
        expect.at("single_file_saved_projections").get<size_t>());
  const fs::path file = fs::temp_directory_path() / "dwg_v162_single.json";
  REQUIRE(testio::SaveDrawing(*dwg, file.string()));
  auto fromFile = testio::LoadDrawing(file.string());
  REQUIRE(fromFile != nullptr);
  CHECK(fromFile->SavedProjections().empty());
  fs::remove(file);
}

// v203 (docs/drawings/README.md#exploded-views): an assembly view may draw
// one of its source's exploded views. A view without the key is as
// assembled; an id that is not one is refused, never read as none.
TEST_CASE("Drawing golden loads and round-trips (v203 exploded view)",
          "[drawing][serialization][golden]") {
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v203-exploded-view.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  REQUIRE(dwg->ViewCount() == 2);
  CHECK(dwg->GetViews()[0].explodedView == 0);
  CHECK(dwg->GetViews()[1].explodedView == 57);

  const fs::path tmp = fs::temp_directory_path() / "dwg_exploded_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  CHECK(reloaded->GetViews()[0].explodedView == 0);
  CHECK(reloaded->GetViews()[1].explodedView == 57);
  fs::remove(tmp);

  std::ifstream in(golden);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const std::string from = "\"exploded_view\": 57";
  for (const char* bad :
       {"\"exploded_view\": 0", "\"exploded_view\": -3",
        "\"exploded_view\": 2.5", "\"exploded_view\": \"drawing\""}) {
    INFO(bad);
    std::string t = text;
    t.replace(t.find(from), from.size(), bad);
    const fs::path p = fs::temp_directory_path() / "dwg_exploded_bad.json";
    std::ofstream(p) << t;
    CHECK(testio::LoadDrawing(p.string()) == nullptr);
    fs::remove(p);
  }
}
