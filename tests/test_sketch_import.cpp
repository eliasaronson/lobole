// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// DXF and SVG into a sketch (docs/sketching/README.md#importing-dxf-and-svg).
// The fixtures are tests/fixtures/import/, written by its make_fixtures.py
// (the DXFs by ezdxf, an independent writer; the flat pattern by our own
// export_dxf).

#include "TestPaths.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/ImportDxf.h"
#include "core/io/ImportSvg.h"
#include "core/io/SketchImport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = std::numbers::pi;

std::string Fixture(const char* name) {
  return std::string(TestSourceDir()) + "/tests/fixtures/import/" + name;
}

std::map<std::string, int> Kinds(const std::vector<sketch::Entity>& es) {
  std::map<std::string, int> out;
  for (const sketch::Entity& e : es)
    out[std::visit(
        [](const auto& x) -> std::string {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, sketch::Point>)
            return "point";
          else if constexpr (std::is_same_v<T, sketch::Line>)
            return "line";
          else if constexpr (std::is_same_v<T, sketch::Circle>)
            return "circle";
          else if constexpr (std::is_same_v<T, sketch::Arc>)
            return "arc";
          else if constexpr (std::is_same_v<T, sketch::Ellipse>)
            return "ellipse";
          else if constexpr (std::is_same_v<T, sketch::EllipseArc>)
            return "ellipse_arc";
          else if constexpr (std::is_same_v<T, sketch::BSpline>)
            return "spline";
          else
            return "text";
        },
        e)]++;
  return out;
}

template <class T> std::vector<T> All(const std::vector<sketch::Entity>& es) {
  std::vector<T> out;
  for (const sketch::Entity& e : es)
    if (const T* t = std::get_if<T>(&e))
      out.push_back(*t);
  return out;
}

// A DXF of ENTITIES only (R12 reads a file with nothing else).
std::string Dxf(const std::string& entities, int insunits = 4,
                const std::string& blocks = "") {
  std::string s = "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n" +
                  std::to_string(insunits) + "\n0\nENDSEC\n";
  if (!blocks.empty())
    s += "0\nSECTION\n2\nBLOCKS\n" + blocks + "0\nENDSEC\n";
  return s + "0\nSECTION\n2\nENTITIES\n" + entities + "0\nENDSEC\n0\nEOF\n";
}

std::string Refusal(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

double ArcSweep(const sketch::Arc& a) {
  double s = a.endAngle - a.startAngle;
  while (s <= 0.0)
    s += 2.0 * kPi;
  return s;
}

glm::dvec2 ArcPoint(const sketch::Arc& a, double t) {
  return a.center + a.radius * glm::dvec2(std::cos(t), std::sin(t));
}

} // namespace

TEST_CASE("an affine image of a conic is the exact conic, mirrors reversed",
          "[sketch][import]") {
  using sketchimport::ConicEntity;
  // A similarity: a circle stays a circle, an arc an arc with its sweep.
  const glm::dmat2 rot30(std::cos(kPi / 6), std::sin(kPi / 6),
                         -std::sin(kPi / 6), std::cos(kPi / 6));
  auto c = ConicEntity({1, 2}, 3.0 * rot30, 0.0, 2 * kPi, true);
  REQUIRE(std::holds_alternative<sketch::Circle>(c));
  CHECK_THAT(std::get<sketch::Circle>(c).radius, WithinAbs(3.0, 1e-12));
  auto a = ConicEntity({0, 0}, glm::dmat2(2.0), 0.0, kPi / 2, false);
  REQUIRE(std::holds_alternative<sketch::Arc>(a));
  CHECK_THAT(ArcSweep(std::get<sketch::Arc>(a)), WithinAbs(kPi / 2, 1e-12));
  // A mirror (x -> -x): the quarter from +X to +Y becomes the quarter from
  // +Y to -X, still CCW in the model.
  auto m =
      ConicEntity({0, 0}, glm::dmat2(-2.0, 0.0, 0.0, 2.0), 0.0, kPi / 2, false);
  REQUIRE(std::holds_alternative<sketch::Arc>(m));
  const auto& ma = std::get<sketch::Arc>(m);
  CHECK(glm::length(ArcPoint(ma, ma.startAngle) - glm::dvec2(0, 2)) < 1e-12);
  CHECK(glm::length(ArcPoint(ma, ma.endAngle) - glm::dvec2(-2, 0)) < 1e-12);
  // A skew: an ellipse arc through exactly the images of the arc's points.
  const glm::dmat2 skew(1.0, 0.0, std::tan(0.3), 1.0);
  auto e = ConicEntity({5, 5}, skew * 2.0, 0.2, 1.4, false);
  REQUIRE(std::holds_alternative<sketch::EllipseArc>(e));
  const auto& ea = std::get<sketch::EllipseArc>(e);
  const auto onEllipse = [&](glm::dvec2 p) {
    const glm::dvec2 u(std::cos(ea.rotation), std::sin(ea.rotation));
    const glm::dvec2 v(-u.y, u.x);
    const glm::dvec2 d = p - ea.center;
    const double x = glm::dot(d, u) / ea.rMajor, y = glm::dot(d, v) / ea.rMinor;
    return x * x + y * y;
  };
  for (double t : {0.2, 0.8, 1.4}) {
    const glm::dvec2 p =
        glm::dvec2(5, 5) + skew * (2.0 * glm::dvec2(std::cos(t), std::sin(t)));
    CHECK_THAT(onEllipse(p), WithinAbs(1.0, 1e-12));
  }
  CHECK_THAT(ea.endParam - ea.startParam, WithinAbs(1.2, 1e-9));
}

TEST_CASE("each DXF entity becomes the sketch entity its row says",
          "[sketch][import]") {
  dxfimport::Options opt;
  // LINE, CIRCLE, ARC in degrees CCW, POINT.
  auto d = dxfimport::Parse(
      Dxf("0\nLINE\n5\nA1\n8\n0\n10\n0\n20\n0\n30\n0\n11\n10\n21\n0\n31\n0\n"
          "0\nCIRCLE\n5\nA2\n8\n0\n10\n5\n20\n5\n30\n0\n40\n2\n"
          "0\nARC\n5\nA3\n8\n0\n10\n0\n20\n0\n30\n0\n40\n4\n50\n90\n51\n180\n"
          "0\nPOINT\n5\nA4\n8\n0\n10\n7\n20\n8\n30\n0\n"),
      opt);
  CHECK(Kinds(d.entities) ==
        std::map<std::string, int>{
            {"line", 1}, {"circle", 1}, {"arc", 1}, {"point", 1}});
  const auto arc = All<sketch::Arc>(d.entities).at(0);
  CHECK_THAT(arc.radius, WithinAbs(4.0, 1e-12));
  CHECK(glm::length(ArcPoint(arc, arc.startAngle) - glm::dvec2(0, 4)) < 1e-12);
  CHECK(glm::length(ArcPoint(arc, arc.endAngle) - glm::dvec2(-4, 0)) < 1e-12);
  CHECK(d.unit == "mm");
  CHECK(d.unitSource == "$INSUNITS 4");

  // LWPOLYLINE with a bulge: a half circle (bulge 1) of the chord's
  // diameter, and a negative bulge runs clockwise, stored reversed.
  d = dxfimport::Parse(
      Dxf("0\nLWPOLYLINE\n5\nB1\n8\n0\n90\n3\n70\n0\n"
          "10\n0\n20\n0\n42\n1\n10\n10\n20\n0\n42\n-0.5\n10\n20\n20\n0\n"),
      opt);
  const auto arcs = All<sketch::Arc>(d.entities);
  REQUIRE(arcs.size() == 2);
  CHECK_THAT(arcs[0].radius, WithinAbs(5.0, 1e-12));
  CHECK(glm::length(arcs[0].center - glm::dvec2(5, 0)) < 1e-12);
  // The CCW half from (0,0) to (10,0) passes BELOW the chord.
  CHECK_THAT(ArcPoint(arcs[0], arcs[0].startAngle + ArcSweep(arcs[0]) / 2).y,
             WithinAbs(-5.0, 1e-9));
  // bulge -0.5: sweep 4·atan(0.5), radius chord / (2 sin(sweep/2)).
  const double sweep = 4.0 * std::atan(0.5);
  CHECK_THAT(ArcSweep(arcs[1]), WithinAbs(sweep, 1e-12));
  CHECK_THAT(arcs[1].radius,
             WithinAbs(10.0 / (2.0 * std::sin(sweep / 2)), 1e-9));
  // Clockwise from (10,0) to (20,0): the arc bulges above the chord.
  CHECK(ArcPoint(arcs[1], arcs[1].startAngle + ArcSweep(arcs[1]) / 2).y > 0);

  // 2D POLYLINE / VERTEX, closed (flag 1).
  d = dxfimport::Parse(
      Dxf("0\nPOLYLINE\n5\nC1\n8\n0\n66\n1\n10\n0\n20\n0\n30\n0\n70\n1\n"
          "0\nVERTEX\n8\n0\n10\n0\n20\n0\n0\nVERTEX\n8\n0\n10\n4\n20\n0\n"
          "0\nVERTEX\n8\n0\n10\n4\n20\n3\n0\nSEQEND\n8\n0\n"),
      opt);
  CHECK(Kinds(d.entities) == std::map<std::string, int>{{"line", 3}});

  // ELLIPSE: a full turn is an Ellipse, part of one an EllipseArc, with
  // rMinor = ratio·|major axis|.
  d = dxfimport::Parse(
      Dxf("0\nELLIPSE\n5\nD1\n8\n0\n10\n0\n20\n0\n30\n0\n11\n0\n21\n6\n31\n0\n"
          "40\n0.5\n41\n0\n42\n6.283185307179586\n"
          "0\nELLIPSE\n5\nD2\n8\n0\n10\n0\n20\n0\n30\n0\n11\n6\n21\n0\n31\n0\n"
          "40\n0.5\n41\n0\n42\n1.5707963267948966\n"),
      opt);
  const auto full = All<sketch::Ellipse>(d.entities).at(0);
  CHECK_THAT(full.rMajor, WithinAbs(6.0, 1e-12));
  CHECK_THAT(full.rMinor, WithinAbs(3.0, 1e-12));
  CHECK_THAT(std::abs(std::sin(full.rotation)), WithinAbs(1.0, 1e-12));
  const auto part = All<sketch::EllipseArc>(d.entities).at(0);
  CHECK_THAT(part.endParam - part.startParam, WithinAbs(kPi / 2, 1e-12));
}

TEST_CASE("a non-uniform DXF spline is split into exact Bezier pieces",
          "[sketch][import]") {
  const std::vector<double> knots = {0, 0, 0, 0, 0.3, 1.2, 2, 2, 2, 2};
  const std::vector<glm::dvec2> poles = {{0.5, 1.5}, {1.0, 1.8}, {1.5, 1.4},
                                         {2.0, 1.9}, {2.5, 1.5}, {3.0, 1.7}};
  const auto pieces = sketchimport::SplineToSketch(knots, poles, 3);
  REQUIRE(pieces.size() == 3); // one per non-empty knot span
  // Each piece is a Bezier: its own parameter runs 0..1 over its span.
  const double spans[4] = {0.0, 0.3, 1.2, 2.0};
  for (size_t i = 0; i < pieces.size(); i++) {
    REQUIRE(pieces[i].poles.size() == 4);
    for (int k = 0; k <= 20; k++) {
      const double s = k / 20.0;
      const double u = spans[i] + s * (spans[i + 1] - spans[i]);
      const glm::dvec2 want = sketchimport::EvalSpline(knots, poles, 3, u);
      const glm::dvec2 got = sketch::BSplineEvalUV(pieces[i], s);
      CHECK(glm::length(got - want) < 1e-9);
    }
  }
  // Clamped-uniform comes through as it is.
  const auto same =
      sketchimport::SplineToSketch({0, 0, 0, 0, 1, 2, 3, 3, 3, 3}, poles, 3);
  REQUIRE(same.size() == 1);
  CHECK(same[0].poles == poles);
  // An unclamped (periodic-style) knot vector: pieces over [U_p, U_n] only.
  const std::vector<double> uniform = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const auto open = sketchimport::SplineToSketch(uniform, poles, 3);
  REQUIRE(open.size() == 3);
  for (size_t i = 0; i < open.size(); i++)
    for (int k = 0; k <= 10; k++) {
      const double s = k / 10.0;
      const glm::dvec2 want =
          sketchimport::EvalSpline(uniform, poles, 3, 3.0 + i + s);
      CHECK(glm::length(sketch::BSplineEvalUV(open[i], s) - want) < 1e-9);
    }
}

TEST_CASE("the ezdxf plate: inches, nested blocks, layers, CRLF",
          "[sketch][import]") {
  dxfimport::Options opt;
  auto d = dxfimport::Read(Fixture("plate-ezdxf.dxf"), opt);
  CHECK(d.unit == "in");
  CHECK(d.unitSource == "$INSUNITS 1");
  CHECK(Kinds(d.entities) == std::map<std::string, int>{{"arc", 1},
                                                        {"circle", 3},
                                                        {"ellipse", 1},
                                                        {"line", 3},
                                                        {"spline", 3}});
  CHECK(d.notImported == std::map<std::string, int>{{"MTEXT", 1}, {"TEXT", 1}});
  // The bulged end: a half circle of radius 1 in about (3, 1) in.
  const auto arc = All<sketch::Arc>(d.entities).at(0);
  CHECK_THAT(arc.radius, WithinAbs(25.4, 1e-9));
  CHECK(glm::length(arc.center - glm::dvec2(76.2, 25.4)) < 1e-9);
  // The nested INSERT (PAIR rotated 90 at (2.5, 1), BOLT at x ±0.3): the
  // bolts land at (2.5, 0.7) and (2.5, 1.3) in.
  int bolts = 0;
  for (const auto& c : All<sketch::Circle>(d.entities))
    if (std::abs(c.radius - 2.54) < 1e-9) {
      bolts++;
      CHECK_THAT(c.center.x, WithinAbs(63.5, 1e-9));
      CHECK((std::abs(c.center.y - 17.78) < 1e-9 ||
             std::abs(c.center.y - 33.02) < 1e-9));
    }
  CHECK(bolts == 2);
  // Layers: only the outline and holes (the BOLT block's layer-0 circles
  // take the INSERT's layer, HOLES); construction for the engraving.
  opt.layers = {"OUTLINE", "HOLES"};
  d = dxfimport::Read(Fixture("plate-ezdxf.dxf"), opt);
  CHECK(Kinds(d.entities).count("spline") == 0);
  CHECK(Kinds(d.entities).at("circle") == 3);
  CHECK(sketchimport::OpenEnds(d.entities, sketchimport::kChainTol).empty());
  opt.layers.clear();
  opt.constructionLayers = {"ENGRAVE"};
  d = dxfimport::Read(Fixture("plate-ezdxf.dxf"), opt);
  int construction = 0;
  for (const auto& e : d.entities)
    construction += sketch::IsConstruction(e) ? 1 : 0;
  CHECK(construction == 3);
  // An unknown layer is refused, listing the file's.
  opt.constructionLayers = {"BEND"};
  const std::string why =
      Refusal([&] { (void)dxfimport::Read(Fixture("plate-ezdxf.dxf"), opt); });
  CHECK(why.find("no layer 'BEND'") != std::string::npos);
  CHECK(why.find("OUTLINE") != std::string::npos);
}

TEST_CASE("a DXF's unit comes from $INSUNITS, else the caller, else mm",
          "[sketch][import]") {
  dxfimport::Options opt;
  auto d = dxfimport::Read(Fixture("unitless.dxf"), opt);
  CHECK(d.unit == "mm");
  CHECK(d.unitSource.find("default") != std::string::npos);
  opt.units = units::Length::Inch;
  d = dxfimport::Read(Fixture("unitless.dxf"), opt);
  CHECK(d.unit == "in");
  glm::dvec2 lo, hi;
  REQUIRE(sketchimport::Bounds(d.entities, lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(40 * 25.4, 1e-9));
  // A unit contradicting the file's is refused, naming both.
  opt.units = units::Length::Millimetre;
  const std::string why =
      Refusal([&] { (void)dxfimport::Read(Fixture("plate-ezdxf.dxf"), opt); });
  CHECK(why.find("contradicts") != std::string::npos);
  CHECK(why.find("$INSUNITS 1") != std::string::npos);
  CHECK(Refusal([&] {
          (void)dxfimport::Read(Fixture("refuse-insunits.dxf"), {});
        }).find("Accepted: 1 (in), 4 (mm), 5 (cm), 6 (m)") !=
        std::string::npos);
}

TEST_CASE("every refused DXF names the entity and imports nothing",
          "[sketch][import]") {
  struct Row {
    const char* file;
    const char* says;
  };
  for (const Row& r : {Row{"refuse-rational-spline.dxf", "rational spline"},
                       Row{"refuse-fit-spline.dxf", "fit points"},
                       Row{"refuse-wide-polyline.dxf", "with width"},
                       Row{"refuse-uneven-insert.dxf", "unevenly"},
                       Row{"refuse-extrusion.dxf", "not +Z or -Z"},
                       Row{"refuse-mixed-z.dxf", "mixed elevations"},
                       Row{"refuse-3d-polyline.dxf", "3D polyline"},
                       Row{"refuse-mesh.dxf", "polygon mesh"},
                       Row{"refuse-3dface.dxf", "3DFACE"}}) {
    INFO(r.file);
    const std::string why =
        Refusal([&] { (void)dxfimport::Read(Fixture(r.file), {}); });
    INFO(why);
    CHECK(why.find(r.says) != std::string::npos);
    CHECK(why.find("(handle ") != std::string::npos);
  }
  CHECK(Refusal([&] {
          (void)dxfimport::Read(Fixture("refuse-binary.dxf"), {});
        }).find("binary DXF") != std::string::npos);
}

TEST_CASE("a mirrored OCS (-Z extrusion) is mapped, not refused",
          "[sketch][import]") {
  const auto d = dxfimport::Read(Fixture("mirrored-ocs.dxf"), {});
  const auto arc = All<sketch::Arc>(d.entities).at(0);
  // OCS centre (10, 0) is WCS (-10, 0); OCS 0..90 deg runs from (-15, 0)
  // to (-10, 5), clockwise as seen from +Z: stored CCW the other way.
  CHECK(glm::length(arc.center - glm::dvec2(-10, 0)) < 1e-12);
  CHECK(glm::length(ArcPoint(arc, arc.startAngle) - glm::dvec2(-10, 5)) <
        1e-12);
  CHECK(glm::length(ArcPoint(arc, arc.endAngle) - glm::dvec2(-15, 0)) < 1e-12);
}

TEST_CASE("our own flat-pattern DXF extrudes to the blank's area",
          "[sketch][import]") {
  dxfimport::Options opt;
  opt.constructionLayers = {"BEND_UP", "BEND_DOWN"};
  auto d = dxfimport::Read(Fixture("flat-pattern.dxf"), opt);
  sketchimport::SnapEndpoints(d.entities, sketchimport::kChainTol);
  CHECK(sketchimport::OpenEnds(d.entities, sketchimport::kChainTol).empty());
  glm::dvec2 lo, hi;
  REQUIRE(sketchimport::Bounds(d.entities, lo, hi));
  const double width = hi.x - lo.x, length = hi.y - lo.y;
  CHECK_THAT(width, WithinAbs(100.0, 1e-6));
  CHECK(length > 60.0); // the blank, unfolded: base, bend and wall
  Document doc;
  auto sk =
      std::make_unique<SketchFeature>("Blank", SketchPlane(PlaneType::XY));
  for (auto& e : d.entities)
    sk->AddEntity(e);
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Plate", doc.GetFeature(0)->GetId(), 1.0, ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(width * length - kPi * 25.0, 1e-7));
}

TEST_CASE("an SVG comes in exact, Y flipped, in the unit its root fixes",
          "[sketch][import]") {
  auto d = svgimport::Read(Fixture("inkscape-mm.svg"), {});
  CHECK(d.unit == "mm");
  CHECK(d.unitSource.find("100mm") != std::string::npos);
  CHECK(d.notImported ==
        std::map<std::string, int>{{"hidden", 1}, {"image", 1}, {"text", 1}});
  const auto k = Kinds(d.entities);
  // The rounded rect: 4 lines + 4 quarter arcs. The path: 2 cubics, 2
  // quadratics, a half-circle arc, 3 lines (L, H, Z). The bore and the used
  // symbol's hole are circles; the slot is an ellipse; the skewed circle is
  // an ellipse; the triangle 3 lines.
  CHECK(k.at("circle") == 2);
  CHECK(k.at("ellipse") == 2);
  CHECK(k.at("spline") == 4);
  CHECK(k.at("arc") == 5);
  CHECK(k.at("line") == 10);
  // The bore at user (20, 15) inside translate(5, 5): (25, -20) mm, Y up.
  bool bore = false;
  for (const auto& c : All<sketch::Circle>(d.entities))
    if (std::abs(c.radius - 6.0) < 1e-12) {
      bore = true;
      CHECK(glm::length(c.center - glm::dvec2(25, -20)) < 1e-12);
    }
  CHECK(bore);
  // The arc segment "A 5,5 0 0 1 80,40" from (70, 40): a half circle about
  // (75, 40) user, (80, -45) mm.
  bool half = false;
  for (const auto& a : All<sketch::Arc>(d.entities))
    if (std::abs(a.radius - 5.0) < 1e-9 &&
        glm::length(a.center - glm::dvec2(80, -45)) < 1e-9) {
      half = true;
      CHECK_THAT(ArcSweep(a), WithinAbs(kPi, 1e-9));
    }
  CHECK(half);
  // Stroked shapes are reported: their width is not geometry.
  bool stroked = false;
  for (const auto& n : d.notes)
    stroked = stroked || n.find("stroked") != std::string::npos;
  CHECK(stroked);
  // A root that fixes its scale refuses `units`.
  svgimport::Options px;
  px.units = "px";
  CHECK(Refusal([&] {
          (void)svgimport::Read(Fixture("inkscape-mm.svg"), px);
        }).find("fixes its own scale") != std::string::npos);
}

TEST_CASE("an Illustrator SVG in pt, and px when nothing is declared",
          "[sketch][import]") {
  auto d = svgimport::Read(Fixture("illustrator-pt.svg"), {});
  CHECK(d.unit == "mm");
  glm::dvec2 lo, hi;
  REQUIRE(sketchimport::Bounds(d.entities, lo, hi));
  // The frame polygon spans 10..134 pt = 124 pt.
  CHECK_THAT(hi.x - lo.x, WithinAbs(124.0 * 25.4 / 72.0, 1e-9));
  const auto k = Kinds(d.entities);
  CHECK(k.at("spline") == 4); // the circle path's four cubics
  CHECK(k.at("arc") == 2);    // the slot's two half circles
  sketchimport::SnapEndpoints(d.entities, sketchimport::kChainTol);
  CHECK(sketchimport::OpenEnds(d.entities, sketchimport::kChainTol).empty());

  d = svgimport::Read(Fixture("px-default.svg"), {});
  CHECK(d.unit == "px");
  REQUIRE(sketchimport::Bounds(d.entities, lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(25.4, 1e-9)); // 96 px = 1 in
  svgimport::Options mm;
  mm.units = "mm";
  d = svgimport::Read(Fixture("px-default.svg"), mm);
  REQUIRE(sketchimport::Bounds(d.entities, lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(96.0, 1e-9));
  svgimport::Options bad;
  bad.units = "furlong";
  CHECK(Refusal([&] {
          (void)svgimport::Read(Fixture("px-default.svg"), bad);
        }).find("Accepted: mm, cm, m, in, px, pt") != std::string::npos);
}

TEST_CASE("every refused SVG names the element", "[sketch][import]") {
  for (const auto& [file, says] :
       {std::pair{"refuse-clip.svg", "<circle id=\"clipped\"> is clipped"},
        std::pair{"refuse-mask.svg", "<rect id=\"masked\"> is masked"},
        std::pair{"refuse-foreign.svg", "<foreignObject id=\"html\">"}}) {
    INFO(file);
    const std::string why =
        Refusal([&] { (void)svgimport::Read(Fixture(file), {}); });
    INFO(why);
    CHECK(why.find(says) != std::string::npos);
  }
}

TEST_CASE("endpoints within the chain tolerance are joined, onto arcs",
          "[sketch][import]") {
  std::vector<sketch::Entity> es = {
      sketch::Arc{{0, 0}, 1.0, 0.0, kPi / 2},
      sketch::Line{{1.00005, 0.0}, {2, 0}},
      sketch::Line{{2, 0}, {0.0, 1.00003}},
  };
  sketchimport::SnapEndpoints(es, sketchimport::kChainTol);
  CHECK(std::get<sketch::Line>(es[1]).a == glm::dvec2(1.0, 0.0));
  CHECK(glm::length(std::get<sketch::Line>(es[2]).b - glm::dvec2(0, 1)) <
        1e-15);
  CHECK(sketchimport::OpenEnds(es, sketchimport::kChainTol).empty());
  es.push_back(sketch::Line{{5, 5}, {6, 6}});
  CHECK(sketchimport::OpenEnds(es, sketchimport::kChainTol).size() == 2);
}
