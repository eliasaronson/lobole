// Sheet furniture (v147): section views, parts lists, balloons, datum
// symbols, the title block's derived rows, cosmetic threads in callouts, and
// what the pan/tilt base plate's drawing found wrong on the way
// (docs/drawings/README.md#section-views).
#include "TestPaths.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/drawing/DrawingSection.h"
#include "core/drawing/DrawingValidate.h"
#include "core/features/HoleFeature.h"
#include "core/io/Format.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using Catch::Approx;

namespace {

// A 40 x 20 x 10 block with a Ø8 bore along Z at (20, 10).
TopoDS_Shape BoredBlock() {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 20.0, 10.0).Shape();
  const TopoDS_Shape bore =
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 10, -1), gp_Dir(0, 0, 1)), 4.0,
                               12.0)
          .Shape();
  return BRepAlgoAPI_Cut(box, bore).Shape();
}

int CylinderFace(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i)
    if (BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() ==
        GeomAbs_Cylinder)
      return i - 1;
  return -1;
}

} // namespace

TEST_CASE("A section cut names what it left by what it was",
          "[drawing][section]") {
  const TopoDS_Shape block = BoredBlock();
  TopTools_IndexedMapOfShape before;
  TopExp::MapShapes(block, TopAbs_EDGE, before);
  const int bore = CylinderFace(block);
  REQUIRE(bore >= 0);

  // Through the bore's axis, looking along +Y: the half nearer the reader
  // (y < 10) goes.
  std::string error;
  const auto cut =
      drawing::CutBody(block, {20.0, 10.0, 0.0}, {0.0, 1.0, 0.0}, &error);
  REQUIRE(cut.has_value());
  REQUIRE_FALSE(cut->shape.IsNull());

  // The source is untouched: the boolean ran non-destructive.
  TopTools_IndexedMapOfShape after;
  TopExp::MapShapes(block, TopAbs_EDGE, after);
  CHECK(after.Extent() == before.Extent());

  // Every edge is a source edge or a section of a source face. The plane
  // cuts the bore along two walls: one the bore's face made, the other the
  // bore's own seam, which lies in this plane (the projection names a seam
  // seen edge-on by its face too, so both walls are the bore's).
  TopTools_IndexedMapOfShape srcFaces;
  TopExp::MapShapes(block, TopAbs_FACE, srcFaces);
  const TopoDS_Face boreFace = TopoDS::Face(srcFaces(bore + 1));
  int fromBore = 0, seams = 0;
  for (size_t e = 0; e < cut->edgeOrigin.size(); ++e) {
    const bool named = cut->edgeOrigin[e] >= 0 || cut->edgeFromFace[e] >= 0;
    CHECK(named);
    if (cut->edgeFromFace[e] == bore)
      fromBore++;
    if (cut->edgeOrigin[e] >= 0 &&
        BRep_Tool::IsClosed(TopoDS::Edge(before(cut->edgeOrigin[e] + 1)),
                            boreFace))
      seams++;
  }
  CHECK(fromBore + seams == 2);
  CHECK(fromBore >= 1);

  // The exposed face is two 16 x 10 rectangles either side of the bore.
  REQUIRE(cut->cutLoops.size() == 2);
  for (const auto& loop : cut->cutLoops)
    for (const glm::dvec3& p : loop)
      CHECK(p.y == Approx(10.0).margin(1e-6));

  // Hatched in the plane's own coordinates (x, z), the lines cover the
  // area: 2 x 160 mm2 at 1 mm spacing is 320 mm of line.
  std::vector<std::vector<glm::dvec2>> loops;
  for (const auto& loop : cut->cutLoops) {
    std::vector<glm::dvec2> l;
    for (const glm::dvec3& p : loop)
      l.push_back({p.x, p.z});
    loops.push_back(l);
  }
  double length = 0.0;
  for (const auto& [a, b] : drawing::HatchRegion(loops, 1.0, 0.0))
    length += glm::length(b - a);
  CHECK(length == Approx(320.0).margin(1e-6));
}

TEST_CASE("Hatching is even-odd: a hole stays clear", "[drawing][section]") {
  const std::vector<std::vector<glm::dvec2>> loops = {
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{3, 3}, {7, 3}, {7, 7}, {3, 7}}};
  const auto lines = drawing::HatchRegion(loops, 1.0, 0.0);
  double length = 0.0;
  for (const auto& [a, b] : lines) {
    length += glm::length(b - a);
    // No line runs through the hole's interior.
    const double y = a.y;
    if (y > 3.0 && y < 7.0)
      CHECK_FALSE((std::min(a.x, b.x) < 5.0 && std::max(a.x, b.x) > 5.0));
  }
  CHECK(length == Approx(84.0));
  // At 45 degrees the area is the same, the lines just run diagonally.
  double diag = 0.0;
  for (const auto& [a, b] :
       drawing::HatchRegion(loops, 0.5, 3.14159265358979 / 4.0))
    diag += glm::length(b - a);
  CHECK(diag == Approx(84.0 / 0.5).epsilon(0.03));
}

TEST_CASE("A seam is the face's silhouette or nothing, never an edge",
          "[drawing][hlr]") {
  // A plain cylinder along Z: its one seam is a model edge.
  drawing::InputBody body;
  body.shape = BRepPrimAPI_MakeCylinder(10.0, 30.0).Shape();
  body.sourceId = 1;
  drawing::ProjectionOptions opts;
  std::string error;
  // From the side, the two side lines are ONE face's two silhouettes, so a
  // Ø across them resolves whichever side the seam happens to lie on.
  const auto side =
      drawing::ProjectView({body}, {0, 1, 0}, {0, 0, 1}, opts, &error);
  REQUIRE(side.has_value());
  int outlines = 0;
  for (const auto& c : side->curves) {
    CHECK(c.kind != drawing::CurveKind::Sewn);
    if (c.kind == drawing::CurveKind::Outline && c.visible)
      outlines++;
  }
  CHECK(outlines >= 2);
  // Down the axis, the seam is a radial line across the end face: drawn,
  // it was a tick on the circle. Now nothing but the circles.
  const auto end =
      drawing::ProjectView({body}, {0, 0, -1}, {0, 1, 0}, opts, &error);
  REQUIRE(end.has_value());
  for (const auto& c : end->curves)
    CHECK(c.curveType == 1); // circles only
}

TEST_CASE("Balloons, datums and tables draw from one description",
          "[drawing][notes]") {
  drawing::ResolvedNote balloon;
  balloon.text = "7";
  balloon.pos = {100, 100};
  balloon.style = NoteStyle::Balloon;
  balloon.hasLeader = true;
  balloon.anchor = {130, 100};
  const auto g = drawing::NoteGraphics(balloon);
  REQUIRE(g.texts.size() == 1);
  CHECK(g.texts[0].text == "7");
  REQUIRE(g.fills.size() == 1); // the dot on the part
  glm::dvec2 dot(0.0);
  for (const glm::dvec2& p : g.fills[0])
    dot += p / static_cast<double>(g.fills[0].size());
  CHECK(glm::length(dot - balloon.anchor) < 1e-6);
  glm::dvec2 mn, mx;
  drawing::NoteBox(balloon, mn, mx);
  CHECK(mx.x - mn.x == Approx(8.0)); // a one-digit balloon is 8 mm across

  drawing::ResolvedNote datum = balloon;
  datum.text = "A";
  datum.style = NoteStyle::Datum;
  const auto d = drawing::NoteGraphics(datum);
  REQUIRE(d.fills.size() == 1);
  REQUIRE(d.fills[0].size() == 3); // the triangle, its base on the feature
  glm::dvec2 baseMid = 0.5 * (d.fills[0][0] + d.fills[0][2]);
  CHECK(glm::length(baseMid - datum.anchor) < 1e-6);

  drawing::ResolvedNote table;
  table.text = "CAPTION\nITEM\tPART\n1\tbase_plate";
  table.pos = {410, 58}; // its bottom-right corner
  CHECK(drawing::IsTableText(table.text));
  drawing::NoteBox(table, mn, mx);
  CHECK(mx.x == Approx(410.0));
  CHECK(mn.y == Approx(58.0));
  CHECK(mx.y - mn.y == Approx(18.0)); // three 6 mm rows
  const auto t = drawing::NoteGraphics(table);
  CHECK(t.texts.size() == 5);

  drawing::ResolvedNote plain;
  plain.text = "BREAK ALL EDGES";
  CHECK(drawing::NoteGraphics(plain).empty());
}

TEST_CASE("A leader stops at the side of its words", "[drawing][notes]") {
  const glm::dvec2 at{100, 100};
  const glm::dvec2 left = drawing::LeaderEnd({50, 100}, at, "ABC");
  CHECK(left.x < 100.0);
  CHECK(left.x > 90.0);
  CHECK(left.y == Approx(100.0));
  const glm::dvec2 below = drawing::LeaderEnd({100, 50}, at, "ABC");
  CHECK(below.x == Approx(100.0));
  CHECK(below.y < 100.0);
}

TEST_CASE("A tapped hole says its thread, its depth and its drill",
          "[drawing][callout]") {
  HoleFeature hole("Tripod", 1, HoleKind::Simple, 7.9375, 12.7);
  hole.fitThread = "3/8-16 UNC";
  hole.fitKind = "tapped";
  ThreadMark mark;
  mark.designation = "3/8-16 UNC";
  mark.threadClass = "2B";
  CHECK(drawing::FormatHoleCallout(hole, {}, &mark) ==
        "3/8-16 UNC-2B DEEP 12.7, DRILL \xC3\x98"
        "7.94 DEEP 12.7");
  // A thread shorter than the drill says both depths.
  mark.length = 9.5;
  CHECK(drawing::FormatHoleCallout(hole, {}, &mark) ==
        "3/8-16 UNC-2B DEEP 9.5, DRILL \xC3\x98"
        "7.94 DEEP 12.7");
  // Without a thread the callout reads as it always did.
  CHECK(drawing::FormatHoleCallout(hole) == "3/8-16 UNC DEEP 12.7");
}

TEST_CASE("A toleranced class states its limits", "[drawing][tolerance]") {
  Tolerance k7;
  k7.isoClass = "K7";
  CHECK(drawing::FormatToleranceSuffix(k7, {}, 32.0) == " K7 (+0.007/-0.018)");
  Tolerance h7;
  h7.isoClass = "H7";
  CHECK(drawing::FormatToleranceSuffix(h7, {}, 20.0) == " H7 (+0.021/0)");
  // No nominal: the class alone, as before.
  CHECK(drawing::FormatToleranceSuffix(h7) == " H7");
}

TEST_CASE("A diameter across two walls is linear and carries its Ø",
          "[drawing][dimension]") {
  // A bore Ø16 seen in section at 2:1: two walls 32 sheet mm apart, named
  // by the bore's face.
  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  for (double x : {84.0, 116.0}) {
    drawing::SheetCurve wall;
    wall.silFace = ElementRef{1, "bore"};
    wall.sourceId = 1;
    wall.curveType = 0;
    wall.points = {{x, 50}, {x, 70}};
    view.curves.push_back(wall);
  }
  DrawingDim dim;
  dim.kind = DrawingDimKind::Diameter;
  dim.viewId = 1;
  ProjRef r;
  r.viewId = 1;
  r.silFace = ElementRef{1, "bore"};
  dim.refs = {r};
  dim.labelOffset = {0, 20}; // 20 mm up from the walls' middle
  drawing::DimContext ctx;
  ctx.viewScale = 2.0;
  const auto rd = drawing::ResolveDim(dim, view, ctx);
  REQUIRE(rd.resolved);
  CHECK_FALSE(rd.radial);
  CHECK(rd.value == Approx(16.0));
  CHECK(rd.text == "\xC3\x98"
                   "16");
  CHECK(rd.dim1.y == Approx(80.0));
  CHECK(rd.dim2.y == Approx(80.0));
  CHECK(std::abs(rd.dim2.x - rd.dim1.x) == Approx(32.0));
}

TEST_CASE("An extreme asked of a circle is that extreme, not its centre",
          "[drawing][notes]") {
  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  drawing::SheetCurve ring;
  ring.edge = ElementRef{2, "ring"};
  ring.sourceId = 5;
  ring.curveType = 1;
  ring.conicCenter = {50, 50};
  ring.conicRadius = 10.0;
  ring.closed = true;
  ring.points = {{60, 50}, {50, 60}, {40, 50}, {50, 40}, {60, 50}};
  view.curves.push_back(ring);
  DrawingNote note;
  note.text = "3";
  note.style = NoteStyle::Balloon;
  note.viewId = 1;
  note.leader.viewId = 1;
  note.leader.occurrenceId = 5;
  note.leader.edge = ElementRef{2, "ring"};
  note.leader.extreme = "left";
  note.pos = {20, 50};
  const auto rn = drawing::ResolveNote(note, &view);
  REQUIRE(rn.hasLeader);
  CHECK(rn.anchor.x == Approx(40.0)); // on the ring, not in the bore
  // ...and a whole circle seen whole is the one that gets a centre mark.
  CHECK(drawing::HasCentreMark(ring));
  ring.closed = false; // an arc of a circle mostly hidden
  CHECK_FALSE(drawing::HasCentreMark(ring));
}

TEST_CASE("A label sits on a view when it covers the part, not its box",
          "[drawing][validate]") {
  DrawingDocument dwg;
  dwg.GetSheet().size = SheetSize::A3;
  DrawingView v;
  const uint64_t id = dwg.AddView(v);
  // A diamond inside a 100 x 100 box: the box's corners are empty paper.
  drawing::SheetView sv;
  sv.viewId = id;
  sv.hasGeometry = true;
  sv.solvedSourceVersion = 1;
  sv.bboxMin = {100, 100};
  sv.bboxMax = {200, 200};
  drawing::SheetCurve outline;
  outline.points = {{150, 100}, {200, 150}, {150, 200}, {100, 150}, {150, 100}};
  sv.curves.push_back(outline);
  const std::map<uint64_t, drawing::SheetView> renders = {{id, sv}};
  const auto over = [&](glm::dvec2 at) {
    drawing::ResolvedDim d;
    d.id = 9;
    d.resolved = true;
    d.text = "12";
    d.labelPos = at;
    d.ext1 = d.ext2 = d.dim1 = d.dim2 = at;
    for (const auto& i : drawing::ValidateDrawing(dwg, renders, {d}, {}))
      if (i.kind == "dim_label_over_view")
        return true;
    return false;
  };
  CHECK_FALSE(over({108, 192})); // in the box's corner: clear of the part
  CHECK(over({150, 150}));       // inside the outline
  CHECK(over({175, 125}));       // on its edge
}

TEST_CASE("A value inside a section stands on its hatching",
          "[drawing][validate]") {
  DrawingDocument dwg;
  DrawingView v;
  const uint64_t id = dwg.AddView(v);
  drawing::SheetView sv;
  sv.viewId = id;
  sv.hasGeometry = true;
  sv.solvedSourceVersion = 1;
  sv.sectioned = true;
  sv.bboxMin = {100, 100};
  sv.bboxMax = {200, 200};
  drawing::SheetCurve outline;
  outline.points = {{100, 100}, {200, 100}, {200, 200}, {100, 200}, {100, 100}};
  sv.curves.push_back(outline);
  drawing::ResolvedDim d;
  d.id = 9;
  d.resolved = true;
  d.text = "12.7";
  d.labelPos = {150, 150};
  d.ext1 = d.ext2 = d.dim1 = d.dim2 = d.labelPos;
  const auto issues = drawing::ValidateDrawing(dwg, {{id, sv}}, {d}, {});
  CHECK(std::none_of(issues.begin(), issues.end(), [](const auto& i) {
    return i.kind == "dim_label_over_view";
  }));
}

TEST_CASE("The title block says material, scale and notes",
          "[drawing][export]") {
  Sheet sheet;
  sheet.titleBlock.material = "6082-T6";
  sheet.titleBlock.scale = "2:1";
  sheet.titleBlock.notes = "ISO 2768-mK";
  const auto rows = DrawingExport::TitleBlockRows(sheet, {});
  std::vector<std::string> labels;
  for (const auto& r : rows)
    labels.push_back(r.label);
  CHECK(labels == std::vector<std::string>{"PROJECT", "DOCUMENT", "MATERIAL",
                                           "SCALE", "AUTHOR", "DATE", "UNITS",
                                           "NOTES"});
  CHECK(rows[2].value == "6082-T6");
  CHECK(rows[7].value == "ISO 2768-mK");
  // The block is as tall as its rows.
  const SheetChrome chrome = ChromeOf(sheet);
  CHECK(chrome.titleBlockMax.y - chrome.titleBlockMin.y ==
        Approx(6.0 * static_cast<double>(rows.size())));
}

TEST_CASE("Drawing golden loads and round-trips (v147 sheet furniture)",
          "[drawing][serialization][golden]") {
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v147-sheet-furniture.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  const auto check = [](const DrawingDocument& d) {
    CHECK(d.GetSheet().titleBlock.material == "6082-T6 or 6061-T6");
    CHECK(d.GetSheet().titleBlock.notes == "General tolerances ISO 2768-mK");
    REQUIRE(d.ViewCount() == 3);
    CHECK_FALSE(d.GetViews()[0].section.on);
    REQUIRE(d.GetViews()[1].section.on);
    CHECK(d.GetViews()[1].section.label == "A");
    REQUIRE(d.NoteCount() == 3);
    CHECK(d.GetNotes()[0].style == NoteStyle::Datum);
    CHECK(d.GetNotes()[0].leader.extreme == "left");
    CHECK(d.GetNotes()[1].table == NoteTable::Bom);
    CHECK(d.GetNotes()[2].style == NoteStyle::Balloon);
    CHECK(d.GetNotes()[2].leader.occurrenceId == 1);
  };
  check(*dwg);
  const fs::path tmp = fs::temp_directory_path() / "dwg_v147_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  check(*reloaded);
  fs::remove(tmp);

  // A note style or table this build does not know is refused, not drawn
  // as plain text.
  std::ifstream in(golden);
  std::stringstream ss;
  ss << in.rdbuf();
  for (const auto& [from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"\"style\": \"datum\"", "\"style\": \"datums\""},
           {"\"table\": \"bom\"", "\"table\": \"parts\""}}) {
    std::string text = ss.str();
    text.replace(text.find(from), from.size(), to);
    const fs::path bad = fs::temp_directory_path() / "dwg_v147_bad.json";
    std::ofstream(bad) << text;
    CHECK(testio::LoadDrawing(bad.string()) == nullptr);
    fs::remove(bad);
  }
}
