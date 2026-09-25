#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "core/BodyMode.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
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

} // namespace

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
