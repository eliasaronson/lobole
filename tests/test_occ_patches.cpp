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
#include "core/DirectEdit.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/Feature.h"
#include "core/features/DraftFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/ShellFeature.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Standard_ErrorHandler.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

bool Patched(const char* name) { return occpatch::Carries(name); }

// This thread's CPU seconds: the clock directedit::MakeDeadline counts.
// Elsewhere the process's, which is the same for a single-threaded builder.
double ThreadCpuSeconds() {
#if defined(__linux__)
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec / 1e9;
#endif
  return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

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

// generator/toolmakers_clamp's M3 retaining screw (action_matrix_full_3):
// BRepOffset_MakeOffset::BuildOffsetByArc registered its offset faces in the
// order of a map hashed on shape address, so the same shell came back valid
// on one copy and invalid on the next, and the valid ones differed in their
// volume's last digits (upstream's OCCT#1600). Every face of the screw,
// opened by the action matrix's wall, on copies at new addresses with a
// different amount of heap held before each: one answer per face.
TEST_CASE("A shell of the clamp's screw answers alike on every copy",
          "[occ][shell][determinism]") {
  REQUIRE_PATCH("occt-offset-arc-bind-order");
  const Body base = Fixture("shell-arc-bind-order.brep");
  const TopoDS_Shape& shape = base.shape.GetShape();
  int faces = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    faces++;
  REQUIRE(faces > 0);
  std::vector<std::unique_ptr<Feature>> none;
  std::vector<std::vector<char>> held;
  for (int face = 0; face < faces; face++) {
    const std::string name = *base.elementMap.NameOf(face);
    std::set<std::string> answers;
    for (int copy = 0; copy < 24; copy++) {
      held.emplace_back(64 * (copy * 7 % 13 + 1));
      std::vector<Body> bodies{base};
      bodies[0].shape = CADShape(BRepBuilderAPI_Copy(shape, false).Shape());
      ShellFeature shell("Sh", 1, {ElementRef{1, name}}, 0.05);
      RegenContext ctx{bodies, none, 0};
      if (shell.Execute(ctx)) {
        char volume[64];
        std::snprintf(volume, sizeof volume, "built, %.17g mm3",
                      bodies[0].shape.Volume());
        answers.insert(volume);
      } else {
        answers.insert("refused: " + shell.GetError());
      }
    }
    std::string seen;
    for (const std::string& a : answers)
      seen += "\n  " + a;
    INFO("face " << face << " (" << name << "):" << seen);
    CHECK(answers.size() == 1);
  }
}

// golden/v146-worm, a Shell of 0.02 mm (the Shell tool's default, a tenth
// of its thinnest wall) opening its face 1: from about 3 s in, one edge/face
// intersection of two neighbouring offset faces ran for one to three minutes
// without asking the progress indicator, so the Shell's budget was overrun
// by as much (nix/patches/upstream/offset-inter3d-progress.md). A deadline
// inside that stretch is now heard there: the longest silence measured is
// about 1 s, under load. Unpatched, a 15 s deadline returned after 170 to
// 270 s.
TEST_CASE("A thin shell of the worm stops when its deadline passes",
          "[occ][shell][slow]") {
  REQUIRE_PATCH("occt-offset-inter3d-progress");
  const Body worm = Fixture("shell-worm-thin-wall.brep");
  TopTools_ListOfShape open;
  int k = 0;
  for (TopExp_Explorer ex(worm.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), k++)
    if (k == 1)
      open.Append(ex.Current());
  REQUIRE(open.Extent() == 1);
  // Inside the silent stretch on any machine this runs on: it starts within
  // a few seconds and lasts most of a minute even on a quiet one.
  const double deadline = 15.0;
  const Handle(Message_ProgressIndicator) stop =
      directedit::MakeDeadline(deadline);
  // Timed on the deadline's own clock, this thread's CPU: on the wall a
  // loaded machine stretches the 15 CPU seconds before it expires too, and
  // a contended run read 24 s "late" that had stopped on time.
  const double t0 = ThreadCpuSeconds();
  BRepOffsetAPI_MakeThickSolid maker;
  maker.MakeThickSolidByJoin(worm.shape.GetShape(), open, -0.02, 2.0e-4,
                             BRepOffset_Skin, false, false, GeomAbs_Arc, false,
                             stop->Start());
  const double took = ThreadCpuSeconds() - t0;
  CHECK(directedit::Expired(stop));
  CHECK_FALSE(maker.IsDone());
  INFO("stopped " << took - deadline << " CPU s after its deadline");
  // Ten times the longest silence measured, and a tenth of what an unheard
  // deadline overran by.
  CHECK(took < deadline + 10.0);
}

// Random design 17's body, a 0.002 mm shell opening its blend face (face 9):
// the arc join's intersection of two offset faces failed, and
// BRepOffset_Tool::Inter3D read the failed pave filler's section blocks,
// which have no edge, and faulted at address 0. It now throws.
TEST_CASE("A thin shell whose offset faces do not intersect fails without a "
          "fault",
          "[occ][shell]") {
  REQUIRE_PATCH("occt-offset-inter3d-progress");
  const Body body = Fixture("shell-inter3d-failed.brep");
  TopTools_ListOfShape open;
  int k = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), k++)
    if (k == 9)
      open.Append(ex.Current());
  REQUIRE(open.Extent() == 1);
  const unsigned faults = occsig::FaultCount();
  BRepOffsetAPI_MakeThickSolid maker;
  const auto why = occsig::RunCatchingOccSignals([&] {
    maker.MakeThickSolidByJoin(body.shape.GetShape(), open, -0.002, 2.0e-5,
                               BRepOffset_Skin, false, false, GeomAbs_Arc,
                               false);
  });
  REQUIRE(why);
  CHECK_THAT(*why, ContainsSubstring("Inter3D"));
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

// occt-error-handler-thread-local.patch: every OCC_CATCH_SIGNALS scope
// links a Standard_ErrorHandler into a stack, and OCCT 7.9 keeps ONE stack
// for every thread, behind one mutex. A boolean opens thousands of scopes
// (two per function evaluation of its curve-on-surface tolerance check), so
// query booleans on 22 threads spent more time queued in the kernel's futex
// than computing: the strandbeest's leg-pair study took 148 s wall and
// 1468 s of system time on 22 threads, 85 s on 8.
//
// COST (CPU time; docs/ui/testing.md#tests-that-time-things): a scope's CPU
// on every core at once against its CPU on one. Per thread the stack is the
// thread's own, so the two are alike; behind the one mutex each scope pays
// for the queue: 13 us of CPU a scope on 22 threads against 37 ns on one
// unpatched, 17 ns against 11 ns patched. The bound leaves room for the
// E-cores: a hybrid CPU's low-power core spends about 3.3 times a P-core's
// CPU on the same work, and all threads land on both kinds where one lands
// on either. Load only lowers the contention it measures, so a busy machine
// can pass the unpatched kernel, never fail the patched one.
TEST_CASE("Signal scopes on every thread cost what one thread's do", "[occ]") {
  REQUIRE_PATCH("occt-error-handler-thread-local");
  const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
  constexpr long kScopes = 400000;
  const auto scopes = [] {
    for (long i = 0; i < kScopes; ++i) {
      Standard_ErrorHandler handler; // what OCC_CATCH_SIGNALS declares
      (void)handler;
    }
  };
  const auto cpuPerScope = [&](unsigned n) {
    const std::clock_t t0 = std::clock();
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; ++t)
      pool.emplace_back(scopes);
    for (std::thread& th : pool)
      th.join();
    return double(std::clock() - t0) / CLOCKS_PER_SEC / double(n * kScopes);
  };
  const double one = cpuPerScope(1);
  const double all = cpuPerScope(threads);
  INFO(threads << " threads: " << 1e9 * all << " ns of CPU a scope, against "
               << 1e9 * one << " ns on one");
  CHECK(all < 8.0 * one);
}
