// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A part's numbers outside the features that already took expressions
// (docs/variables/README.md#numbers-that-are-expressions): the sheet-metal
// and tube rules, a tolerance's limits, the feature fields that were plain
// numbers, a sketch pattern's op and an arc's angles. A bound one follows its
// variable when it changes, one that cannot stand keeps its last good value
// and says why, rename and delete reach every one, and a file holding a text
// that does not parse still opens. The probe at the end is "nothing new
// arrives unbindable" for a part.

#include "TestIO.h"
#include "TestPaths.h"
#include "core/Angles.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Serialization.h"
#include "core/tube/Rules.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

sheet::SheetMetalRules AluRules() {
  sheet::SheetMetalRules r;
  r.material = "aluminum_6061_t6";
  r.gauge = "1.5mm";
  r.thickness = 1.5;
  return r;
}

// The stable name of the straight edge running between `a` and `b`.
std::string EdgeNamed(const Body& body, const glm::dvec3& a,
                      const glm::dvec3& b) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  const auto near = [](const gp_Pnt& p, const glm::dvec3& q) {
    return std::abs(p.X() - q.x) < 1e-6 && std::abs(p.Y() - q.y) < 1e-6 &&
           std::abs(p.Z() - q.z) < 1e-6;
  };
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt p0 = c.Value(c.FirstParameter());
    const gp_Pnt p1 = c.Value(c.LastParameter());
    if ((near(p0, a) && near(p1, b)) || (near(p0, b) && near(p1, a)))
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
  }
  return {};
}

// A 100 x 60 aluminium blank with one wall bent up off its front edge.
void Bent(Document& doc, const sheet::SheetMetalRules& rules) {
  doc.SetSheetMetal(rules);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {100, 0}, 1});
  sk->AddEntity(sketch::Line{{100, 0}, {100, 60}, 2});
  sk->AddEntity(sketch::Line{{100, 60}, {0, 60}, 3});
  sk->AddEntity(sketch::Line{{0, 60}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<BaseFlangeFeature>("Base", sketchId));
  const uint64_t base = doc.GetFeature(1)->GetId();
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {100, 0, 0});
  REQUIRE_FALSE(edge.empty());
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Wall", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0));
  REQUIRE(doc.GetFeature(2)->GetError().empty());
}

double BendRadius(const Document& doc) {
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  return doc.GetBodies()[0].bends[0].radius;
}

bool HasProblem(const Document& doc, const std::string& owner,
                const std::string& needle) {
  for (const DocumentExprProblem& p : doc.ExprProblems())
    if (p.owner == owner && p.error.find(needle) != std::string::npos)
      return true;
  return false;
}

} // namespace

TEST_CASE("A bend radius bound to a variable rebuilds the bends",
          "[variables][sheetmetal]") {
  Document doc;
  REQUIRE(doc.SetParameter("r_bend", "3"));
  sheet::SheetMetalRules rules = AluRules();
  rules.bendRadius.expr = "r_bend";
  rules.bendRadius = 3.0;
  Bent(doc, rules);
  CHECK_THAT(BendRadius(doc), WithinAbs(3.0, 1e-9));

  REQUIRE(doc.SetParameter("r_bend", "4"));
  CHECK_THAT(doc.SheetMetal()->bendRadius.value, WithinAbs(4.0, 1e-12));
  CHECK_THAT(BendRadius(doc), WithinAbs(4.0, 1e-9));
  CHECK(doc.ExprProblems().empty());

  // Under what aluminium takes (2.25 at 1.5 mm): the rules' error, in what
  // every mutating call answers, and the bends keep the last good radius.
  REQUIRE(doc.SetParameter("r_bend", "1"));
  CHECK(HasProblem(doc, "sheet-metal rules", "bend radius"));
  CHECK_THAT(doc.SheetMetal()->bendRadius.value, WithinAbs(4.0, 1e-12));
  CHECK(doc.SheetMetal()->bendRadius.expr == "r_bend");
  CHECK_THAT(BendRadius(doc), WithinAbs(4.0, 1e-9));

  // Back to one it takes: the problem goes, and the bends follow.
  REQUIRE(doc.SetParameter("r_bend", "5"));
  CHECK(doc.ExprProblems().empty());
  CHECK_THAT(BendRadius(doc), WithinAbs(5.0, 1e-9));
}

TEST_CASE("A tolerance's limits and the tube rules follow their variables",
          "[variables][tolerances][tube]") {
  Document doc;
  REQUIRE(doc.SetParameter("tol", "0.1"));
  REQUIRE(doc.SetParameter("die", "60"));
  Tolerance t;
  t.ref = {1, "1/face0"};
  t.ref2 = {1, "1/face1"};
  t.lower.expr = "-tol";
  t.upper.expr = "tol";
  const uint64_t id = doc.SetTolerance(t).id;
  tube::TubeRules tr;
  tr.material = tube::Materials().front();
  tr.formerRadii = {50.0, 80.0};
  tr.formerRadiiExpr = {"die", ""};
  tr.minStraight.expr = "die / 2";
  doc.SetTube(tr);
  doc.Regenerate();
  CHECK_THAT(doc.FindTolerance(id)->lower.value, WithinAbs(-0.1, 1e-12));
  CHECK_THAT(doc.FindTolerance(id)->upper.value, WithinAbs(0.1, 1e-12));
  CHECK_THAT(doc.Tube()->formerRadii[0], WithinAbs(60.0, 1e-12));
  CHECK_THAT(doc.Tube()->formerRadii[1], WithinAbs(80.0, 1e-12));
  CHECK_THAT(doc.Tube()->minStraight.value, WithinAbs(30.0, 1e-12));

  REQUIRE(doc.SetParameter("tol", "0.25"));
  CHECK_THAT(doc.FindTolerance(id)->upper.value, WithinAbs(0.25, 1e-12));
  // A reversed band is the tolerance's error; it keeps the last good one.
  REQUIRE(doc.SetParameter("tol", "-0.1"));
  CHECK(HasProblem(doc, "tolerance " + std::to_string(id), "reversed"));
  CHECK_THAT(doc.FindTolerance(id)->upper.value, WithinAbs(0.25, 1e-12));
  // A negative straight is what tube::Validate refuses.
  REQUIRE(doc.SetParameter("die", "-10"));
  CHECK(HasProblem(doc, "tube rules", ""));
  CHECK_THAT(doc.Tube()->formerRadii[0], WithinAbs(60.0, 1e-12));
}

TEST_CASE("The feature numbers that were plain take expressions",
          "[variables][modeling]") {
  Document doc;
  REQUIRE(doc.SetParameter("x0", "10"));
  auto box = std::make_unique<BoxFeature>("B", glm::dvec3(0.0), 2, 2, 2);
  box->positionExpr = {"x0", "", "x0 / 2"};
  doc.AddFeature(std::move(box));
  glm::dvec3 lo, hi;
  doc.GetBodies().at(0).shape.GetBoundingBox(lo, hi);
  CHECK_THAT(lo.x, WithinAbs(10.0, 1e-6));
  CHECK_THAT(lo.z, WithinAbs(5.0, 1e-6));
  REQUIRE(doc.SetParameter("x0", "20"));
  doc.GetBodies().at(0).shape.GetBoundingBox(lo, hi);
  CHECK_THAT(lo.x, WithinAbs(20.0, 1e-6));

  // A count that comes out fractional is the feature's error, never rounded.
  REQUIRE(doc.SetParameter("n", "41"));
  auto wheel = std::make_unique<WormWheelFeature>("Wheel", ElementRef{});
  wheel->teeth.expr = "n / 2";
  doc.AddFeature(std::move(wheel));
  CHECK_THAT(doc.GetFeature(1)->GetError(), ContainsSubstring("whole number"));
}

TEST_CASE("A sketch pattern's count and an arc's angle follow variables",
          "[variables][sketch]") {
  Document doc;
  REQUIRE(doc.SetParameter("n", "4"));
  REQUIRE(doc.SetParameter("sweep", "90"));
  auto sk = std::make_unique<SketchFeature>(
      "S", SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  const uint32_t c = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
  const uint32_t a =
      sk->AddEntity(sketch::Arc{{20, 0}, 3.0, 0.0, angles::ToRadians(45.0)});
  sk->AddLinearPattern({c}, {5.0, 0.0}, 2, /*link=*/true);
  sk->derived.back().count.expr = "n";
  sk->derived.back().vecExpr[0] = "n * 2";
  sk->entityExprs[a]["end_angle_deg"] = "sweep";
  doc.AddFeature(std::move(sk));
  const auto* s = static_cast<const SketchFeature*>(doc.GetFeature(0));
  CHECK(s->GetError().empty());
  CHECK(s->derived.at(0).instances.size() == 3);
  CHECK_THAT(s->derived.at(0).vec.x, WithinAbs(8.0, 1e-12));
  const auto arcEnd = [&] {
    const auto* sk2 = static_cast<const SketchFeature*>(doc.GetFeature(0));
    for (const sketch::Entity& e : sk2->entities)
      if (const auto* arc = std::get_if<sketch::Arc>(&e);
          arc != nullptr && sketch::EntityId(e) == a)
        return angles::ToDegrees(arc->endAngle);
    return -1.0;
  };
  CHECK_THAT(arcEnd(), WithinAbs(90.0, 1e-6));

  REQUIRE(doc.SetParameter("n", "6"));
  REQUIRE(doc.SetParameter("sweep", "120"));
  s = static_cast<const SketchFeature*>(doc.GetFeature(0));
  CHECK(s->derived.at(0).instances.size() == 5);
  CHECK_THAT(arcEnd(), WithinAbs(120.0, 1e-6));
  // Through a save the texts stay.
  const nlohmann::json j = DocumentIO::FeatureToJson(*s);
  auto back = DocumentIO::FeatureFromJson(j);
  const auto* sb = static_cast<const SketchFeature*>(back.get());
  CHECK(sb->derived.at(0).count.expr == "n");
  CHECK(sb->derived.at(0).vecExpr[0] == "n * 2");
  CHECK(sb->entityExprs.at(a).at("end_angle_deg") == "sweep");

  // A count that comes out fractional is the sketch's error.
  REQUIRE(doc.SetParameter("n", "2.5"));
  CHECK_THAT(doc.GetFeature(0)->GetError(), ContainsSubstring("whole number"));
}

TEST_CASE("Rename and delete reach the rules, the tolerances and the gaps",
          "[variables]") {
  Document doc;
  REQUIRE(doc.SetParameter("g", "3"));
  sheet::SheetMetalRules rules = AluRules();
  rules.reliefWidth.expr = "g";
  doc.SetSheetMetal(rules);
  tube::TubeRules tr;
  tr.material = tube::Materials().front();
  tr.tailLength.expr = "g * 10";
  doc.SetTube(tr);
  Tolerance t;
  t.ref = {1, "1/face0"};
  t.ref2 = {1, "1/face1"};
  t.lower = -0.1;
  t.upper.expr = "g / 30";
  const uint64_t tid = doc.SetTolerance(t).id;
  auto box = std::make_unique<BoxFeature>("Plate", glm::dvec3(0.0), 2, 2, 2);
  box->positionExpr[1] = "g";
  doc.AddFeature(std::move(box));

  const std::vector<std::string> refs = doc.FindParameterReferences("g");
  const auto has = [&](const std::string& s) {
    return std::find(refs.begin(), refs.end(), s) != refs.end();
  };
  CHECK(has("feature 'Plate'"));
  CHECK(has("sheet-metal rules"));
  CHECK(has("tube rules"));
  CHECK(has("tolerance " + std::to_string(tid)));
  CHECK_FALSE(doc.RemoveParameter("g"));

  REQUIRE(doc.RenameParameter("g", "gap"));
  CHECK(doc.SheetMetal()->reliefWidth.expr == "gap");
  CHECK(doc.Tube()->tailLength.expr == "gap * 10");
  CHECK(doc.FindTolerance(tid)->upper.expr == "gap / 30");
  CHECK(static_cast<const BoxFeature*>(doc.GetFeature(0))->positionExpr[1] ==
        "gap");
}

TEST_CASE("A part file whose rule holds a text that does not parse opens",
          "[variables][sheetmetal]") {
  Document doc;
  sheet::SheetMetalRules rules = AluRules();
  rules.bendRadius = 3.0;
  doc.SetSheetMetal(rules);
  const fs::path path =
      fs::temp_directory_path() /
      ("lobole_part_bad_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::Save(doc, path.string()));
  nlohmann::json j;
  {
    std::ifstream in(path);
    j = nlohmann::json::parse(in);
  }
  j["sheet_metal"]["bend_radius_expr"] = "r *";
  {
    std::ofstream out(path);
    out << j.dump();
  }
  auto loaded = testio::Load(path.string());
  fs::remove(path);
  REQUIRE(loaded != nullptr);
  CHECK(loaded->SheetMetal()->bendRadius.expr == "r *");
  CHECK_THAT(loaded->SheetMetal()->bendRadius.value, WithinAbs(3.0, 1e-12));
  CHECK(HasProblem(*loaded, "sheet-metal rules", "bend_radius_expr"));
}

// ---- The probe: nothing new arrives unbindable ------------------------------
//
// Every feature of every part golden (and the stage's own), plus the part's
// rules and tolerances: each slot VisitExprs hands out is given a sentinel
// text, the feature (or the part) is written, and (1) each sentinel must be
// in what was written and read back where it was, and (2) every number
// written must be a visited slot or on the list below of numbers that are
// deliberately not bindable, each with its reason. A new field then has to
// choose: a slot, or a line here saying why not.

namespace {

// By "<Type>/<path>" with every index written `*`; a trailing "/**" is the
// whole subtree. "<part>/..." is the part's own JSON outside its features.
const std::vector<std::pair<std::string, std::string>> kNotBindable = {
    // Every feature's.
    {"*/id", "an id"},
    {"*/appearance/**", "a colour, cosmetic"},
    {"*/face_appearance/**", "a colour, cosmetic"},
    {"*/rollback_before", "an index"},
    // Sketches.
    {"Sketch/plane/**",
     "where a sketch lies: its frame, which a face or a plane ref carries; "
     "the literals lint reports placements"},
    {"Sketch/plane_ref", "an id"},
    {"Sketch/plane_ref_sketch", "an id"},
    {"Sketch/face_ref/**", "an id and a name"},
    {"Sketch/next_entity_id", "an id"},
    {"Sketch/next_image_id", "an id"},
    {"Sketch/entities/*/id", "an id"},
    {"Sketch/entities/*/a/*",
     "a line's end: bindable as its field (ax, ay; bx, by), an entity "
     "expression visited once it has a text"},
    {"Sketch/entities/*/b/*", "the same"},
    {"Sketch/entities/*/p/*", "a point: x, y, the same"},
    {"Sketch/entities/*/center/*", "a circle's or an arc's: cx, cy, the same"},
    {"Sketch/entities/*/radius", "a circle's or an arc's: r, the same"},
    {"Sketch/entities/*/poles/*/*", "a spline's: p<k>x, p<k>y, the same"},
    {"Sketch/entities/*/anchor/*", "a text's: x, y, the same"},
    {"Sketch/entities/*/height", "a text's, the same"},
    {"Sketch/entities/*/spacing", "a text's, the same"},
    {"Sketch/entities/*/angle", "a text's turn in radians: angle_deg"},
    {"Sketch/derived_ops/*/count",
     "a mirror's and a linked copy's, which make one copy; a pattern's is "
     "its slot"},
    {"Sketch/entities/*/start_angle",
     "radians in the file; its slot is start_angle_deg"},
    {"Sketch/entities/*/end_angle", "the same: end_angle_deg"},
    {"Sketch/entities/*/rotation",
     "an ellipse's or a text's turn: an ellipse is driven by dimensions to "
     "its axis ends, a text's turn by its angle_deg"},
    {"Sketch/entities/*/r_major",
     "an ellipse's: drive a distance from its centre to an axis end"},
    {"Sketch/entities/*/r_minor", "the same"},
    {"Sketch/entities/*/start_param",
     "an elliptical arc's parameter, not an angle"},
    {"Sketch/entities/*/end_param", "the same"},
    {"Sketch/entities/*/degree", "a spline's order, chosen with its poles"},
    {"Sketch/entities/*/knots/*", "derived from the poles"},
    {"Sketch/entities/*/weights/*", "a rational spline's, drawn not typed"},
    {"Sketch/entities/*/size_px", "a text's, view state"},
    {"Sketch/constraints/**",
     "a dimension's value is its slot (constraints/<k>); the rest are ids, "
     "indices and a label's offset"},
    {"Sketch/derived_ops/*/source_ids/*", "an id"},
    {"Sketch/derived_ops/*/source_sketch_id", "an id"},
    {"Sketch/derived_ops/*/axis_entity", "an id"},
    {"Sketch/derived_ops/*/center_entity", "an id"},
    {"Sketch/derived_ops/*/center_which", "a point index"},
    {"Sketch/derived_ops/*/instances/**", "ids"},
    {"Sketch/derived_ops/*/scalar",
     "a linear pattern's or a copy's, which use none; a circular pattern's "
     "is its slot"},
    {"Sketch/derived_ops/*/vec/*",
     "a mirror's and a centre-referenced pattern's, which use none"},
    {"Sketch/external/**", "ids, fingerprints and a refit tolerance"},
    {"Sketch/images/**", "an underlay, cosmetic and out of the chain hash"},
    // Ids and references.
    {"*/sketch_id", "an id"},
    {"*/source_id", "an id"},
    {"*/member_id", "an id"},
    {"*/from/**", "an id and a name"},
    {"Cope/against/*", "ids"},
    {"Cope/mitre/*", "ids"},
    {"*/body_id", "an id"},
    {"*/parent_id", "an id"},
    {"*/target_id", "an id"},
    {"*/tool_id", "an id"},
    {"*/path_id", "an id"},
    {"*/guide_id", "an id"},
    {"*/profile_id", "an id"},
    {"*/datum_id", "an id"},
    {"*/unfold_id", "an id"},
    {"*/frame", "an id"},
    {"*/sketch_ids/*", "an id"},
    {"*/regions/**", "entity ids"},
    {"*/edges/**", "ids and names"},
    {"*/faces/**", "ids, names and their fingerprints"},
    {"*/face/**", "an id and a name"},
    {"*/elements/**", "ids and names"},
    {"*/end_target/**", "an id and a name"},
    {"*/neutral_plane/**", "an id and a name"},
    {"*/open_faces/**", "ids and names"},
    {"*/fixed_face/**", "an id and a name"},
    {"*/bends/*", "ids"},
    {"*/knife/**", "an id"},
    {"*/target/**", "an id and a name"},
    {"*/fit/**",
     "a fitted datum's body, seed pick and search tolerance -- a pick, not a "
     "dimension (a hole's fit pitch is its slot)"},
    {"Hole/fit/pitch", "a slot when stated (fit/pitch)"},
    {"*/axis/kind", "a kind"},
    {"*/axis/sketch_id", "an id"},
    {"*/axis/entity_id", "an id"},
    {"*/axis/datum_axis_id", "an id"},
    {"*/axis/direction/*",
     "a custom axis's direction, read as a way to point (its origin is a "
     "slot)"},
    {"Cylinder/direction/*", "a direction, not a dimension"},
    {"Curve3D/axis/*", "a helix's axis, a direction"},
    {"Frame/z_axis/*", "a direction"},
    {"Frame/x_axis/*", "a direction"},
    {"DatumAxis/entity_id", "an id"},
    {"Derived/placement/orientation/*",
     "a quaternion has no one number to bind (the position does)"},
    {"Derived/source_part", "an id"},
    {"Derived/configuration", "an id"},
    {"Derived/bodies/*", "an id"},
    {"Derived/sketches/*", "an id"},
    {"Derived/snapshot/**", "the copy taken: derived outputs"},
    {"Template/**",
     "an instance's parameters are its template's variables, set by name; "
     "the rest are ids"},
    {"Import/**",
     "where a file is put and how it is read (the placement the alignment "
     "tools compose, the turn, the scale, a mesh's minimum sides): binding "
     "them is not built"},
    {"CamProfile/lift/**",
     "a cam's lift table: binding a law's table is not built"},
    {"CamProfile/segments/**", "a cam's law segments: the same"},
    {"CamProfile/follower/**", "a cam's follower: the same"},
    {"ReplaceFace/**", "ids and fingerprints"},
    {"DeleteFace/**", "ids and fingerprints"},
    {"MoveFace/axis/origin/*", "a slot when custom (axis/origin)"},
    // The part's own JSON.
    {"<part>/version", "the format's"},
    {"<part>/next_id", "an id"},
    {"<part>/active_configuration", "an id"},
    {"<part>/rollback", "an index"},
    {"<part>/parameters/**", "a variable's value is its expression"},
    {"<part>/tolerances/*/id", "an id"},
    {"<part>/tolerances/*/ref/**", "an id and a name"},
    {"<part>/tolerances/*/ref2/**", "an id and a name"},
    {"<part>/sheet_metal/thickness",
     "the gauge's: a gauge is the stock, and it fixes the thickness"},
    {"<part>/print/**", "the print process's profile: a profile is data"},
    {"<part>/print_settings/**", "a slicer's settings"},
    {"<part>/units/**", "presentation"},
    {"<part>/studies/**", "a FEM study's numbers: the study's visitor"},
    {"<part>/configurations/**", "a configuration's overrides are texts"},
    {"<part>/requirements/**", "a requirement's bounds: their own visitor"},
    {"<part>/tables/**", "a table's cells are numbers by design"},
    {"<part>/templates/**", "a template's own document"},
    {"<part>/generator/**", "how a generator made the part"},
    {"<part>/features/**", "probed per feature"},
    {"<part>/assets/**", "file references"},
    {"<part>/geometry/**", "the baked geometry cache"},
    {"<part>/reference_images/**", "cosmetic"},
};

std::string Generic(const std::string& path) {
  std::string out;
  size_t from = 0;
  while (from <= path.size()) {
    const size_t slash = path.find('/', from);
    const std::string seg = path.substr(
        from, slash == std::string::npos ? std::string::npos : slash - from);
    const bool index = !seg.empty() &&
                       seg.find_first_not_of("0123456789") == std::string::npos;
    out += (out.empty() ? "" : "/") + (index ? std::string("*") : seg);
    if (slash == std::string::npos)
      break;
    from = slash + 1;
  }
  return out;
}

bool Matches(const std::string& pattern, const std::string& path) {
  if (pattern.ends_with("/**"))
    return path.starts_with(pattern.substr(0, pattern.size() - 2)) ||
           path == pattern.substr(0, pattern.size() - 3);
  return pattern == path;
}

// `generic` is "<Type>/<path>"; a "*/..." pattern is any type's.
bool Listed(const std::string& type, const std::string& path) {
  for (const auto& [pattern, why] : kNotBindable) {
    if (pattern.starts_with("*/") && Matches(pattern.substr(2), path))
      return true;
    if (Matches(pattern, type + "/" + path))
      return true;
  }
  return false;
}

void Numbers(const nlohmann::json& j, const std::string& at,
             std::vector<std::string>& out) {
  if (j.is_number()) {
    out.push_back(at);
  } else if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      Numbers(it.value(), at.empty() ? it.key() : at + "/" + it.key(), out);
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++)
      Numbers(j[i], at + "/" + std::to_string(i), out);
  }
}

void Strings(const nlohmann::json& j, std::set<std::string>& out) {
  if (j.is_string())
    out.insert(j.get<std::string>());
  else if (j.is_structured())
    for (const auto& v : j)
      Strings(v, out);
}

// The part goldens: every era's features, as files carry them.
std::vector<fs::path> PartGoldens() {
  std::vector<fs::path> out;
  for (const auto& e :
       fs::directory_iterator(fs::path(TestSourceDir()) / "tests" / "golden")) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.ends_with(".json") &&
        !name.ends_with(".expect.json"))
      out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// One feature: sentinels through its slots, written, checked.
void ProbeFeature(Feature& f, std::set<std::string>& seenTypes) {
  const std::string type = f.TypeName();
  seenTypes.insert(type);
  // A sketch entity's fields are slots once bound: bind every one, so the
  // round trip below checks each of them too.
  if (auto* sk = dynamic_cast<SketchFeature*>(&f))
    for (const sketch::Entity& e : sk->entities)
      for (const std::string& field : sketch::EntityExprFields(e))
        sk->entityExprs[sketch::EntityId(e)][field] = "";
  int n = 0;
  std::vector<std::pair<std::string, std::string>> sentinels;
  f.VisitExprs([&](const ExprSlot& slot) {
    const std::string text = "probe_" + std::to_string(n++);
    slot.Expr() = text;
    sentinels.push_back({slot.Path(), text});
  });
  const nlohmann::json j = DocumentIO::FeatureToJson(f);
  std::set<std::string> written;
  Strings(j, written);
  std::vector<std::string> numbers;
  Numbers(j, "", numbers);
  const std::set<std::string> numberAt(numbers.begin(), numbers.end());
  std::set<std::string> visited;
  std::vector<std::pair<std::string, std::string>> live;
  for (const auto& [path, text] : sentinels) {
    visited.insert(Generic(path));
    // A slot the serializer does not write for this feature's kind (a
    // one-sided extrude's second distance) is dormant: no number, so no
    // text. Where its number is written, its text must be too.
    const bool entity = path.starts_with("entities/");
    if (!entity && !numberAt.contains(path) && !written.contains(text))
      continue;
    INFO(type << " slot " << path);
    CHECK(written.contains(text));
    live.push_back({path, text});
  }
  for (const std::string& path : numbers) {
    const std::string g = Generic(path);
    INFO(type << " writes a number at " << path
              << ": make it a slot, or list it with its reason");
    CHECK((visited.contains(g) || Listed(type, g)));
  }
  // ...and read back where it was.
  auto back = DocumentIO::FeatureFromJson(j);
  REQUIRE(back != nullptr);
  std::map<std::string, std::string> again;
  back->VisitExprs(
      [&](const ExprSlot& slot) { again[slot.Path()] = slot.Expr(); });
  for (const auto& [path, text] : live) {
    INFO(type << " slot " << path << " read back");
    CHECK(again[path] == text);
  }
}

} // namespace

TEST_CASE("Every number a part writes is a slot or says why it is not",
          "[variables][conventions]") {
  std::set<std::string> seenTypes;
  size_t parts = 0;
  // Each feature as its file holds it, read alone (no regeneration): one an
  // older era wrote that only the document's migration reads is skipped,
  // and some later golden holds its type.
  for (const fs::path& golden : PartGoldens()) {
    INFO(golden.filename().string());
    nlohmann::json j;
    {
      std::ifstream in(golden);
      j = nlohmann::json::parse(in);
    }
    if (!j.is_object() || !j.contains("features") ||
        !j.at("features").is_array())
      continue;
    parts++;
    for (const nlohmann::json& fj : j.at("features")) {
      std::unique_ptr<Feature> f;
      try {
        f = DocumentIO::FeatureFromJson(fj);
      } catch (const std::exception&) {
        continue;
      }
      ProbeFeature(*f, seenTypes);
    }
  }
  REQUIRE(parts > 50);
  // The types no golden holds, as each is made.
  {
    WormWheelFeature wheel("Wheel", ElementRef{1, "1/face0"});
    ProbeFeature(wheel, seenTypes);
    ShellFeature shell("Shell", 1, {ElementRef{1, "1/face0"}}, 1.0);
    ProbeFeature(shell, seenTypes);
    DraftFeature draft("Draft", 1, {ElementRef{1, "1/face0"}}, 3.0);
    ProbeFeature(draft, seenTypes);
  }

  // The part's own numbers: rules and tolerances, through a save.
  Document doc;
  sheet::SheetMetalRules rules = AluRules();
  rules.bendRadius = 3.0;
  rules.kFactor = 0.4;
  rules.reliefWidth = 2.0;
  rules.reliefDepth = 4.0;
  doc.SetSheetMetal(rules);
  tube::TubeRules tr;
  tr.material = tube::Materials().front();
  tr.formerRadii = {50.0, 80.0};
  tr.minStraight = 40.0;
  tr.tailLength = 60.0;
  tr.maxThinning = 0.2;
  doc.SetTube(tr);
  Tolerance t;
  t.ref = {1, "1/face0"};
  t.ref2 = {1, "1/face1"};
  t.lower = -0.1;
  t.upper = 0.1;
  doc.SetTolerance(t);
  // A sentinel text at every slot, written as a file holds them: a text
  // that names nothing still loads.
  const fs::path path =
      fs::temp_directory_path() /
      ("lobole_part_probe_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::Save(doc, path.string()));
  nlohmann::json j;
  {
    std::ifstream in(path);
    j = nlohmann::json::parse(in);
  }
  {
    nlohmann::json& sm = j["sheet_metal"];
    for (const char* k :
         {"bend_radius", "k_factor", "relief_width", "relief_depth"})
      sm[std::string(k) + "_expr"] = "probe_" + std::string(k);
    nlohmann::json& tj = j["tube"];
    tj["former_radii_expr"] = {"probe_r0", "probe_r1"};
    for (const char* k : {"min_straight", "tail_length", "max_thinning"})
      tj[std::string(k) + "_expr"] = "probe_" + std::string(k);
    j["tolerances"][0]["lower_expr"] = "probe_lower";
    j["tolerances"][0]["upper_expr"] = "probe_upper";
  }
  {
    std::ofstream out(path);
    out << j.dump();
  }
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  // Every slot the part's walk visits came back bound...
  std::vector<std::string> paths;
  {
    sheet::SheetMetalRules s2 = *loaded->SheetMetal();
    s2.VisitExprs([&](const ExprSlot& slot) {
      INFO("sheet_metal/" << slot.Path());
      CHECK(slot.Expr() == "probe_" + slot.Path());
      paths.push_back("sheet_metal/" + slot.Path());
    });
    tube::TubeRules t2 = *loaded->Tube();
    t2.VisitExprs([&](const ExprSlot& slot) {
      INFO("tube/" << slot.Path());
      CHECK(slot.Bound());
      paths.push_back("tube/" + slot.Path());
    });
    CHECK(loaded->GetTolerances().at(0).lower.expr == "probe_lower");
    CHECK(loaded->GetTolerances().at(0).upper.expr == "probe_upper");
    paths.push_back("tolerances/*/lower");
    paths.push_back("tolerances/*/upper");
  }
  // ...and every number the part writes outside its features is one of
  // them or listed.
  REQUIRE(testio::Save(*loaded, path.string()));
  nlohmann::json written;
  {
    std::ifstream in(path);
    written = nlohmann::json::parse(in);
  }
  fs::remove(path);
  std::set<std::string> visited;
  for (const std::string& p : paths)
    visited.insert(Generic(p));
  std::vector<std::string> numbers;
  Numbers(written, "", numbers);
  for (const std::string& p : numbers) {
    const std::string g = Generic(p);
    INFO("the part writes a number at " << p);
    CHECK((visited.contains(g) || Listed("<part>", g)));
  }

  // Every feature type a document can hold was probed: a type no golden
  // carries would go unseen, so the stage's golden carries the rest.
  for (const DocumentIO::FeatureTypeInfo& info : DocumentIO::FeatureTypes()) {
    INFO("no part golden holds a " << info.type
                                   << ": add one to a golden so the probe "
                                      "sees its numbers");
    CHECK(seenTypes.contains(info.type));
  }
}
