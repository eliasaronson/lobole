// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "core/BodyMode.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"

#include "core/Measure.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRep_TEdge.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>

#include <nlohmann/json.hpp>

#include <string>

using Catch::Matchers::WithinRel;

// docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept

namespace {

// The dogfood case, reduced (the PES-on-OM615 intake flange): a block, two
// bores swept round elbows as the manifold's branches are, and a slot whose
// round ends are the bores' own cylinders, run on 13 mm INTO the bores. The
// numbers are the generator's, float32 plane origin and all, because the
// kernel's mistake depends on them: at z = 0 the same cut comes out right.
// The features are written as the SDK sends them, so this is the document
// the dogfood built and not a C++ approximation of it.
const char* kBlock = R"({"type": "Sketch", "id": 1, "name": "Block",
  "plane": {"normal": [-1, 0, 0], "origin": [-100, -97, 283.3999938964844],
            "uAxis": [0, 1, 0]},
  "entities": [{"a": [-40, -25], "b": [40, -25], "id": 2, "type": "line"},
               {"a": [40, -25], "b": [40, 25], "id": 3, "type": "line"},
               {"a": [40, 25], "b": [-40, 25], "id": 4, "type": "line"},
               {"a": [-40, 25], "b": [-40, -25], "id": 5, "type": "line"}]})";
const char* kBlockExtrude = R"({"type": "Extrude", "id": 2, "name": "Block",
  "sketch_id": 1, "distance": 20.0, "mode": "new_body"})";

std::string BoreArc(int id, double y) {
  return R"({"type": "Sketch", "id": )" + std::to_string(id) +
         R"(, "name": "Bore arc",
    "plane": {"normal": [0, -1, 0], "origin": [0, )" +
         std::to_string(y) + R"(, 0], "uAxis": [1, 0, 0]},
    "entities": [{"type": "arc", "id": 1, "center": [-100, 341.9468688964844],
                  "radius": 58.546875, "start_angle": 2.4474694465108136,
                  "end_angle": -1.5707963267948986}]})";
}
std::string BorePipe(int id, int path) {
  return R"({"type": "Pipe", "id": )" + std::to_string(id) +
         R"(, "name": "Bore", "path_id": )" + std::to_string(path) +
         R"(, "outer_diameter": 26.0, "mode": "cut"})";
}

// A slot across both bores, `width` wide, from `x0` running `depth` in -X.
std::string Slot(double x0, double width) {
  const std::string r = std::to_string(width / 2.0);
  return R"({"type": "Sketch", "id": 7, "name": "Slot",
    "plane": {"normal": [-1, 0, 0], "origin": [)" +
         std::to_string(x0) + R"(, -97, 283.3999938964844],
              "uAxis": [0, 1, 0]},
    "entities": [
      {"type": "line", "id": 1, "a": [-15, )" +
         r + R"(], "b": [15, )" + r + R"(]},
      {"type": "line", "id": 2, "a": [-15, -)" +
         r + R"(], "b": [15, -)" + r + R"(]},
      {"type": "arc", "id": 3, "center": [15, 0], "radius": )" +
         r + R"(,
       "start_angle": -1.5707963267948966, "end_angle": 1.5707963267948966},
      {"type": "arc", "id": 4, "center": [-15, 0], "radius": )" +
         r + R"(,
       "start_angle": 1.5707963267948966, "end_angle": 4.71238898038469}]})";
}
const char* kSlotCut = R"({"type": "Extrude", "id": 8, "name": "Slot",
  "sketch_id": 7, "distance": 14.0, "mode": "cut"})";

void Add(Document& doc, const std::string& json) {
  doc.AddFeature(DocumentIO::FeatureFromJson(nlohmann::json::parse(json)));
}

// The block with its two bores, and the volume it has before any slot.
double Bored(Document& doc) {
  Add(doc, kBlock);
  Add(doc, kBlockExtrude);
  Add(doc, BoreArc(3, -112.0));
  Add(doc, BorePipe(4, 3));
  Add(doc, BoreArc(5, -82.0));
  Add(doc, BorePipe(6, 5));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  return doc.GetBodies().front().shape.Volume();
}

CADShape Box(double x, double y, double z, double dx, double dy, double dz) {
  return CADShape(BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape());
}

// The chassis jig's rear plate, reduced (scratch/kernel-repros/jig_repro.py):
// a 5 mm plate, a round seat for a lower rail notched through it, and a slot
// from the seat's axis up past the top edge so the rail drops in. With the
// slot `halfWidth` = the seat's radius its walls are tangent to the seat
// along the edge its floor sits on, and the exact cut came back UNCUT. The
// numbers are the generator's, rail direction and all.
constexpr double kJigX = 1550.0, kJigY = -230.025, kJigZ = 128.797;
constexpr double kSeatR = 13.0;

void JigPlate(Document& doc, double halfWidth) {
  const double u[3] = {1.0, -0.0025, 0.0001};
  const nlohmann::json plate = {
      {"type", "Box"},   {"id", 1},
      {"name", "Plate"}, {"position", {kJigX, -330.0, 0.0}},
      {"width", 5.0},    {"height", 660.0},
      {"depth", 380.0}};
  const nlohmann::json seat = {
      {"type", "Cylinder"},
      {"id", 2},
      {"name", "Seat"},
      {"center", {kJigX - 200 * u[0], kJigY - 200 * u[1], kJigZ - 200 * u[2]}},
      {"direction", {u[0], u[1], u[2]}},
      {"radius", kSeatR},
      {"height", 400.0}};
  const nlohmann::json notch = {{"type", "Boolean"}, {"id", 3},
                                {"name", "Notch"},   {"op", "subtract"},
                                {"target_id", 1},    {"tool_id", 2}};
  const nlohmann::json slot = {
      {"type", "Box"},
      {"id", 4},
      {"name", "Slot"},
      {"position", {kJigX - 50.0, kJigY - halfWidth, kJigZ}},
      {"width", 100.0},
      {"height", 2 * halfWidth},
      {"depth", 380.0 - kJigZ + 20}};
  const nlohmann::json open = {{"type", "Boolean"}, {"id", 5},
                               {"name", "Open"},    {"op", "subtract"},
                               {"target_id", 3},    {"tool_id", 4}};
  for (const nlohmann::json& f : {plate, seat, notch, slot, open})
    doc.AddFeature(DocumentIO::FeatureFromJson(f));
}

// What the notched and slotted plate must weigh: the plate less its section
// (the seat's disc and the slot above the axis, less the half disc they
// share, for a slot at least as wide as the seat) through 5 mm. The rail's
// tilt moves this by well under a cubic millimetre.
double JigPlateVolume(double halfWidth) {
  REQUIRE(halfWidth >= kSeatR);
  const double pi = 3.14159265358979323846;
  const double disc = pi * kSeatR * kSeatR;
  const double slot = 2 * halfWidth * (380.0 - kJigZ);
  return 5.0 * 660.0 * 380.0 - 5.0 * (disc + slot - disc / 2.0);
}

// The bicycle's rear wheel, reduced (scratch/kernel-repros/spoke_repro2.py):
// one 70 mm flange and two 2 mm spokes, a leading and a trailing one, laced
// from the flange's mid-plane to the rim. Their axes cross 8 um apart —
// equal cylinders on all but intersecting axes — and the exact union kept
// one spoke's skin inside the other, counting the overlap twice. The
// planes are the SDK's, float32 as the document stores them.
std::string SpokePath(int id, const char* name, const char* normal,
                      const std::string& origin, const char* uAxis) {
  return std::string(R"({"type": "Sketch", "id": )") + std::to_string(id) +
         R"(, "name": ")" + name + R"(",
    "plane": {"normal": )" +
         normal + R"(, "origin": )" + origin + R"(, "uAxis": )" + uAxis + R"(},
    "entities": [{"type": "line", "id": 1, "a": [0, 0],
                  "b": [286.1747741699219, 0]}]})";
}
std::string SpokePipe(int id, int path, const char* name, const char* mode) {
  return R"({"type": "Pipe", "id": )" + std::to_string(id) + R"(, "name": ")" +
         name + R"(", "path_id": )" + std::to_string(path) +
         R"(, "outer_diameter": 2.0, "mode": ")" + mode + R"("})";
}

// Flange (id 1), spoke 3 (id 3) and spoke 5 (id 5), each spoke a Pipe in
// `mode`. `spoke5z` is where spoke 5 leaves the flange: -37 is as drawn.
void FlangeAndSpokes(Document& doc, const char* spoke5z, const char* mode) {
  Add(doc, R"({"type": "Cylinder", "id": 1, "name": "flange",
    "center": [0, 0, -39], "direction": [0, 0, 1], "radius": 35.0,
    "height": 4.0})");
  Add(doc, SpokePath(2, "spoke 3 path",
                     "[0.4081619679927826, -0.9110320806503296, "
                     "0.05851771682500839]",
                     "[1.8982025288627816e-15, 31.0, -37.0]",
                     "[0.9018110632896423, 0.41233545541763306, "
                     "0.1292916238307953]"));
  Add(doc, SpokePipe(3, 2, "spoke 3", mode));
  Add(doc, SpokePath(4, "spoke 5 path",
                     "[0.795771062374115, -0.5773323774337769, "
                     "0.18285444378852844]",
                     "[30.529041290283203, -5.383093357086182, " +
                         std::string(spoke5z) + "]",
                     "[0.5626689791679382, 0.8165091872215271, "
                     "0.1292916238307953]"));
  Add(doc, SpokePipe(5, 4, "spoke 5", mode));
}

} // namespace

TEST_CASE("A slot exactly as wide as its seat is cut by the fuzzy retry",
          "[boolean]") {
  // The exact cut came back UNCUT (1251345.3 mm3, the notched plate) with a
  // face left inside the slot, and was refused. The retry cuts it, to the
  // volume the section says, and the feature says it needed the retry.
  Document doc;
  JigPlate(doc, kSeatR);
  REQUIRE(doc.Regenerate());
  const Feature* open = doc.GetFeature(doc.FeatureCount() - 1);
  INFO(open->GetError() << " / " << open->GetWarning());
  CHECK(open->GetError().empty());
  CHECK(open->GetWarning().find("needed OpenCASCADE's fuzzy option at") !=
        std::string::npos);
  CHECK(open->GetWarning().find("What the exact one got wrong: cut 'Open'") !=
        std::string::npos);
  REQUIRE(doc.GetBodies().size() == 1);
  const CADShape& plate = doc.GetBodies().front().shape;
  CHECK_THAT(plate.Volume(), WithinRel(JigPlateVolume(kSeatR), 1e-5));
  CHECK(Measure::CheckGeometry(plate).valid);
}

TEST_CASE("Two spokes crossing 8 um apart are united by the fuzzy retry",
          "[boolean]") {
  // The same wheel section built twice: once as drawn, where the exact union
  // of the second spoke is refused and the retry makes it, and once with
  // that spoke moved 0.01 mm off the crossing, which the exact boolean gets
  // right. The two must weigh the same. Both ways of adding a body are
  // covered: a Boolean feature, and a Pipe in "add" mode (ApplyBodyMode).
  const auto wheel = [](const char* spoke5z, bool asFeature) {
    Document doc;
    FlangeAndSpokes(doc, spoke5z, asFeature ? "new_body" : "add");
    if (asFeature) {
      Add(doc, R"({"type": "Boolean", "id": 6, "name": "u1", "op": "union",
                   "target_id": 1, "tool_id": 3})");
      Add(doc, R"({"type": "Boolean", "id": 7, "name": "u2", "op": "union",
                   "target_id": 6, "tool_id": 5})");
    }
    REQUIRE(doc.Regenerate());
    REQUIRE(doc.GetBodies().size() == 1);
    const Feature* last = doc.GetFeature(doc.FeatureCount() - 1);
    return std::pair{doc.GetBodies().front().shape.Volume(),
                     last->GetWarning()};
  };
  for (const bool asFeature : {true, false}) {
    INFO((asFeature ? "Boolean feature" : "Pipe mode add"));
    const auto [nudged, clean] = wheel("-36.99", asFeature);
    CHECK(clean.empty());
    const auto [drawn, warning] = wheel("-37.0", asFeature);
    INFO(warning);
    CHECK(warning.find("needed OpenCASCADE's fuzzy option at 0.00") !=
          std::string::npos);
    CHECK_THAT(drawn, WithinRel(nudged, 1e-5));
  }
}

TEST_CASE("The fuzzy retry leaves its operands as it found them", "[boolean]") {
  // An OpenCASCADE boolean writes the p-curves it computes onto its
  // arguments' edges unless it is built non-destructively, and the retry
  // runs on the document's own bodies. Counted here as the curve
  // representations on the operands' edges, which the exact (destructive)
  // boolean is seen to add to — so the count can see a write.
  const auto reps = [](const CADShape& s) {
    int n = 0;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(s.GetShape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); i++)
      n += Handle(BRep_TEdge)::DownCast(edges(i).TShape())->Curves().Extent();
    return n;
  };
  // Two blocks sharing a face: the exact union computes where each one's
  // edges lie on the other's face, and writes it onto them.
  const CADShape a = Box(0, 0, 0, 10, 10, 10);
  const CADShape b = Box(10, 2, 2, 10, 6, 6);
  const int before = reps(a) + reps(b);

  ElementMap map;
  const CADShape fuzzy = topo::BooleanNamed(BooleanOp::Union, a, {}, b, {}, 9,
                                            map, nullptr, FuzzyTolerance(a, b));
  REQUIRE(fuzzy.IsValid());
  CHECK(reps(a) + reps(b) == before);

  const CADShape exact =
      topo::BooleanNamed(BooleanOp::Union, a, {}, b, {}, 9, map);
  REQUIRE(exact.IsValid());
  CHECK(reps(a) + reps(b) > before);
}

TEST_CASE("A cut that shares a bore's cylinder is refused, not kept wrong",
          "[boolean][extrude]") {
  // Before this, OpenCASCADE's answer was accepted: a valid B-Rep (BRepCheck
  // passes) in which the bores' floors stood inside the slot and the slot's
  // far end stood in the empty bores. In the full manifold that read as the
  // bores turned INSIDE OUT — a section solid where the holes are, and the
  // part 13,800 mm3 lighter than any cut could have made it; here the same
  // mistake removes nothing where it should have taken the web between the
  // bores.
  Document doc;
  const double bored = Bored(doc);
  Add(doc, Slot(-99.0, 26.0)); // 1 mm into the block, 13 on into the bores
  Add(doc, kSlotCut);
  CHECK_FALSE(doc.Regenerate());

  const Feature* slot = doc.GetFeature(doc.FeatureCount() - 1);
  const std::string& why = slot->GetError();
  INFO(why);
  // It names the feature, the operation, the body and both volumes...
  CHECK(why.find("cut 'Slot'") != std::string::npos);
  CHECK(why.find("body 'Block'") != std::string::npos);
  CHECK(why.find("mm3 before") != std::string::npos);
  CHECK(why.find("mm3 after") != std::string::npos);
  // ...and says what to do about it.
  CHECK(why.find("shares a surface") != std::string::npos);
  // It is a case the fuzzy retry cannot save, and the refusal says the retry
  // was made and at what tolerance
  // (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry).
  CHECK(why.find("Retried with OpenCASCADE's fuzzy option at 0.000") !=
        std::string::npos);
  CHECK(why.find("1e-05 of the smaller operand's size), and that was "
                 "refused too: cut 'Slot'") != std::string::npos);
  // A refused feature contributes nothing: the block is the bored block.
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies().front().shape.Volume(), WithinRel(bored, 1e-9));
}

TEST_CASE("The same slot clear of the bores' cylinders is kept",
          "[boolean][extrude]") {
  // The control: 1 mm narrower, the slot's round ends are no longer the
  // bores' surfaces, and the kernel gets it right — the check must not be
  // what stands in the way of a sound cut.
  Document doc;
  const double bored = Bored(doc);
  Add(doc, Slot(-99.0, 25.0));
  Add(doc, kSlotCut);
  REQUIRE(doc.Regenerate());
  const Feature* slot = doc.GetFeature(doc.FeatureCount() - 1);
  INFO(slot->GetError() << " / " << slot->GetWarning());
  CHECK(slot->GetError().empty());
  CHECK(slot->GetWarning().empty());
  // It took the web between the bores, and no more than its own volume.
  const double after = doc.GetBodies().front().shape.Volume();
  CHECK(after < bored);
}

TEST_CASE("A boolean result the wrong size or the wrong way out is refused",
          "[boolean]") {
  // The volume half of the check, on shapes built to break it: a kernel that
  // returned these would be wrong, and the refusal names what is wrong in
  // numbers.
  const CADShape block = Box(0, 0, 0, 10, 10, 10);  // 1000
  const CADShape tool = Box(5, 0, 0, 10, 10, 10);   // 1000, half inside
  const CADShape bigger = Box(0, 0, 0, 10, 10, 12); // 1200

  const std::string added = BooleanRefusal("Pocket", BooleanOp::Subtract,
                                           "Block", block, tool, bigger);
  INFO(added);
  CHECK(added.find("cut 'Pocket' ADDED material") != std::string::npos);
  CHECK(added.find("1000.0 mm3 before, 1200.0 mm3 after") != std::string::npos);

  const std::string lost = BooleanRefusal("Boss", BooleanOp::Union, "Block",
                                          block, tool, Box(0, 0, 0, 5, 5, 5));
  CHECK(lost.find("add LOST material") != std::string::npos);

  const std::string large = BooleanRefusal("Common", BooleanOp::Intersect,
                                           "Block", block, tool, bigger);
  CHECK(large.find("intersect 'Common' came out LARGER") != std::string::npos);

  // Inside out: the same block with its only shell reversed.
  const CADShape inverted(block.GetShape().Reversed());
  REQUIRE(inverted.Volume() < 0.0);
  const std::string inside = BooleanRefusal("Pocket", BooleanOp::Subtract,
                                            "Block", block, tool, inverted);
  CHECK(inside.find("INSIDE OUT") != std::string::npos);

  // Two solids, one of them reversed: the total is still positive (1200 -
  // 125), so only the per-shell check sees it.
  TopoDS_Compound pair;
  BRep_Builder builder;
  builder.MakeCompound(pair);
  builder.Add(pair, Box(20, 0, 0, 10, 10, 12).GetShape());
  builder.Add(pair, Box(40, 0, 0, 5, 5, 5).GetShape().Reversed());
  const std::string shell = BooleanRefusal("Pocket", BooleanOp::Union, "Block",
                                           block, tool, CADShape(pair));
  INFO(shell);
  CHECK(shell.find("oriented INWARD") != std::string::npos);

  // And a sound cut says nothing.
  const TopoDS_Shape drill =
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(5, 5, -1), gp_Dir(0, 0, 1)), 2.0,
                               12.0)
          .Shape();
  const CADShape drilled(BRepAlgoAPI_Cut(block.GetShape(), drill).Shape());
  CHECK(BooleanRefusal("Drill", BooleanOp::Subtract, "Block", block,
                       CADShape(drill), drilled)
            .empty());
}

// An added solid that bridges two bodies makes ONE of them. It used to fuse
// into the last body its bounding box overlapped and leave the other as a
// separate solid the new one overlapped: the chess blanks' necks strung four
// pawns together and the part came out as four pawns plus a fifth body
// running through three of them, weighing the overlap twice.
TEST_CASE("An add that bridges two bodies joins both, and only those",
          "[boolean][body-mode]") {
  const auto box = [](double x0, double y0, double z0, double dx, double dy,
                      double dz) {
    return CADShape(
        BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape());
  };
  std::vector<Body> bodies;
  bodies.push_back({"Left", box(0, 0, 0, 10, 10, 10), "", 1});
  bodies.push_back({"Right", box(20, 0, 0, 10, 10, 10), "", 2});
  bodies.push_back({"Apart", box(0, 40, 0, 10, 10, 10), "", 3});
  std::string warning, error;
  REQUIRE(ApplyBodyMode(bodies, box(5, 4, 4, 20, 2, 2), ElementMap{},
                        ExtrudeMode::Add, "Bridge", "", 9, &warning, &error));
  CHECK(error.empty());
  REQUIRE(bodies.size() == 2);
  // The joined body: both blocks and the 10 mm of bar between them, once.
  const Body& joined = bodies[0].name == "Apart" ? bodies[1] : bodies[0];
  CHECK(joined.shape.SolidCount() == 1);
  CHECK_THAT(joined.shape.Volume(),
             WithinRel(2 * 1000.0 + 10.0 * 2.0 * 2.0, 1e-6));
  const Body& apart = bodies[0].name == "Apart" ? bodies[0] : bodies[1];
  CHECK(apart.name == "Apart");
  CHECK_THAT(apart.shape.Volume(), WithinRel(1000.0, 1e-9));
}

// Two bodies that ALREADY overlap were made separate on purpose -- a cavity
// drawn as its own body inside a shell, to be subtracted from it later -- and
// an add that grows one of them must not merge them. The bridging rule above
// swallowed the cam cover's shell into the cavity a loft extended, and the
// cavity's end face, now inside the shell, was gone.
TEST_CASE("An add that grows one of two overlapping bodies leaves the other",
          "[boolean][body-mode]") {
  const auto box = [](double x0, double y0, double z0, double dx, double dy,
                      double dz) {
    return CADShape(
        BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape());
  };
  std::vector<Body> bodies;
  bodies.push_back({"Shell", box(0, 0, 0, 40, 10, 10), "", 1});
  bodies.push_back({"Cavity", box(2, 2, 2, 10, 6, 6), "", 2});
  std::string warning, error;
  // Extends the cavity along x, still inside the shell.
  REQUIRE(ApplyBodyMode(bodies, box(12, 2, 2, 20, 6, 6), ElementMap{},
                        ExtrudeMode::Add, "Grow", "", 9, &warning, &error));
  CHECK(error.empty());
  REQUIRE(bodies.size() == 2);
  CHECK(bodies[0].name == "Shell");
  CHECK_THAT(bodies[0].shape.Volume(), WithinRel(4000.0, 1e-9));
  CHECK(bodies[1].name == "Cavity");
  CHECK_THAT(bodies[1].shape.Volume(), WithinRel(30.0 * 36.0, 1e-6));
}
