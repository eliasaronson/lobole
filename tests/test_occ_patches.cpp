// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the OpenCASCADE patches fix (nix/patches/, nix/patches/upstream/),
// through our own calls on the bodies that found them, and what a fault the
// signal scope still catches does to the document
// (docs/modeling/README.md#a-caught-kernel-fault).
//
// A patch's test skips where the kernel does not carry it: CI's stock Ubuntu
// jobs link apt's OpenCASCADE, where the fuzz/30 fillet never returns. The
// list comes from the install (cad/OccPatches.h).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "cad/OccPatches.h"
#include "cad/OccSignals.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/Feature.h"
#include "core/features/DraftFeature.h"
#include "core/features/FilletFeature.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

bool Patched(const char* name) { return occpatch::Carries(name); }

#define REQUIRE_PATCH(name)                                                    \
  do {                                                                         \
    if (!Patched(name))                                                        \
      SKIP("this OpenCASCADE does not carry nix/patches/" name ".patch");      \
  } while (false)

// A body read from tests/fixtures/occ/, named as a primitive is: faces and
// edges in the explorer's and MapShapes' order, the ElementMap convention.
Body Fixture(const char* file) {
  TopoDS_Shape shape;
  BRep_Builder b;
  REQUIRE(
      BRepTools::Read(shape,
                      (fs::path(TestSourceDir()) / "tests/fixtures/occ" / file)
                          .string()
                          .c_str(),
                      b));
  Body body;
  body.name = "Body";
  body.createdBy = 1;
  body.shape = CADShape(shape);
  topo::NamePrimitiveElements(body.shape, "1", body.elementMap);
  return body;
}

[[gnu::noinline]] void NullWrite() {
  volatile int* volatile p = nullptr;
  *p = 1; // NOLINT: the point
}

// A feature whose kernel call faults: the fault caught by the signal scope,
// as a draft builder's SIGSEGV was. A Box underneath, so the document can
// hash and copy it as one.
class FaultingFeature : public BoxFeature {
public:
  FaultingFeature() : BoxFeature("Faults", glm::dvec3(0.0), 1, 1, 1) {}
  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FaultingFeature>(*this);
  }
  bool Execute(RegenContext& ctx) override {
    (void)ctx;
    if (const auto why = occsig::RunCatchingOccSignals([] { NullWrite(); })) {
      SetError("failed inside OpenCASCADE (" + *why + ")");
      return false;
    }
    return true;
  }
};

} // namespace

// Random design 30's body, a Fillet r0.125 of the edge of a bore beside its
// seam: BRepFilletAPI_MakeFillet::Add never returned (ReorderFaces walked
// round the vertex for ever). ctest gives this case a timeout.
TEST_CASE("The fuzz/30 fillet is refused instead of never returning",
          "[occ][fillet]") {
  REQUIRE_PATCH("occt-fillet-reorder-faces-loop");
  const Body body = Fixture("fillet-reorder-faces-loop.brep");
  for (const EdgeBlendOp op : {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
    ElementMap map;
    std::string error;
    const CADShape out = topo::EdgeBlendNamed(body.shape, body.elementMap, {50},
                                              op, 0.125, 9, map, &error);
    CHECK_FALSE(out.IsValid());
    CHECK_THAT(error, ContainsSubstring("tangent chain"));
  }
  // And refused before it is committed: the edges are only added.
  const std::vector<Body> bodies{body};
  const Document doc;
  const FilletFeature fillet("Round", {{1, *body.elementMap.EdgeNameOf(50)}},
                             0.125);
  CHECK_THAT(fillet.WhyRefusedOn(bodies, doc),
             ContainsSubstring("tangent chain"));
}

namespace {
// The Draft the action matrix made: `faces` (explorer indices) of the body,
// 2 degrees about the world plane `neutral` at the body's low side.
std::string DraftError(Body body, const std::vector<int>& faces,
                       PlaneType neutral) {
  std::vector<ElementRef> refs;
  for (int f : faces)
    refs.push_back({1, *body.elementMap.NameOf(f)});
  std::vector<Body> bodies{body};
  std::vector<std::unique_ptr<Feature>> none;
  DraftFeature draft("Draft", 1, refs, 2.0, neutral);
  RegenContext ctx{bodies, none, 0};
  CHECK_FALSE(draft.Execute(ctx));
  return draft.GetError();
}
} // namespace

TEST_CASE("A draft past a sphere's pole fails without a fault",
          "[occ][draft]") {
  // reference/38-subtract-sphere: Draft_Modification::Perform dereferenced
  // the 3D curve of the pole's degenerated edge, which has none.
  REQUIRE_PATCH("occt-draft-null-curve-and-parallel");
  const unsigned faults = occsig::FaultCount();
  const std::string why =
      DraftError(Fixture("draft-null-curve.brep"), {1, 2, 3, 4}, PlaneType::XZ);
  CHECK_THAT(why, ContainsSubstring("draft failed"));
  CHECK_THAT(why, !ContainsSubstring("OSD_"));
  CHECK(occsig::FaultCount() == faults);
}

TEST_CASE("A draft whose edge runs parallel to a face fails without throwing",
          "[occ][draft]") {
  // reference/37-union: an Extrema_ExtCS that found the curve parallel had
  // a distance and no points, and Perform read the first point.
  REQUIRE_PATCH("occt-draft-null-curve-and-parallel");
  const std::string why = DraftError(Fixture("draft-parallel-extrema.brep"),
                                     {0, 3, 4, 5}, PlaneType::YZ);
  CHECK_THAT(why, ContainsSubstring("draft failed"));
  CHECK_THAT(why, !ContainsSubstring("Standard_OutOfRange"));
}

TEST_CASE("The modelled thread's end-cap fillet fails without a fault",
          "[occ][fillet]") {
  // reference/25-modelled-thread: TopOpeBRepBuild's CorrectUnclosedWire
  // removed edges from the wire it was iterating.
  REQUIRE_PATCH("occt-topopebrep-unclosed-wire-iterator");
  const unsigned faults = occsig::FaultCount();
  Body body = Fixture("fillet-unclosed-wire.brep");
  const std::string edge = *body.elementMap.EdgeNameOf(13);
  std::vector<Body> bodies{body};
  std::vector<std::unique_ptr<Feature>> none;
  FilletFeature fillet("Round", {{1, edge}}, 0.184391);
  RegenContext ctx{bodies, none, 0};
  CHECK_FALSE(fillet.Execute(ctx));
  CHECK_THAT(fillet.GetError(), !ContainsSubstring("OSD_"));
  CHECK(occsig::FaultCount() == faults);
}

TEST_CASE("A caught kernel fault leaves the earlier bodies as they were and "
          "drops the regen cache",
          "[occ][regen]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(0.0), 40, 30, 20));
  const auto snapshot = [&] {
    const Body& b = doc.GetBodies().front();
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi, true);
    std::vector<double> t;
    for (TopExp_Explorer ex(b.shape.GetShape(), TopAbs_EDGE); ex.More();
         ex.Next())
      t.push_back(BRep_Tool::Tolerance(TopoDS::Edge(ex.Current())));
    return std::make_tuple(b.shape.Volume(), lo, hi, t);
  };
  const auto before = snapshot();
  REQUIRE(doc.RegenCacheSize() > 0);
  const unsigned faults = occsig::FaultCount();
  doc.AddFeature(std::make_unique<FaultingFeature>());
  const Feature* f = doc.GetFeature(1);
  CHECK(occsig::FaultCount() == faults + 1);
  CHECK_THAT(f->GetError(), ContainsSubstring("OSD_SIGSEGV"));
  CHECK(doc.KernelFaulted(f->GetId()));
  // Nothing from before the fault is served from the cache again.
  CHECK(doc.CachedBodiesAfter(0) == nullptr);
  doc.RemoveFeature(1); // what undo does
  CHECK(snapshot() == before);
}
