// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A fault inside an OpenCASCADE signal scope is that call's failure, in
// every test case of the process and not only the first
// (cad/OccSignals.h).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "cad/OccSignals.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <STEPControl_Reader.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

[[gnu::noinline]] void NullWrite() {
  // The pointer itself volatile, so the compiler cannot assume it away.
  volatile int* volatile p = nullptr;
  *p = 1; // NOLINT: the point
}

std::optional<std::string> FaultInScope() {
  return occsig::RunCatchingOccSignals([] { NullWrite(); });
}

} // namespace

// Catch2 puts its own fault handlers in front of every run of a test case
// and puts back what it found when the run ends, so a handler armed once per
// thread was gone from the second run on, and the fault below killed
// designer_tests. Each SECTION is its own run of the case.
TEST_CASE("A fault in an OCC signal scope is caught in every run of a case",
          "[occ]") {
  SECTION("the first run") {
    const auto why = FaultInScope();
    REQUIRE(why.has_value());
    CHECK_THAT(*why, Catch::Matchers::ContainsSubstring("SIGSEGV"));
  }
  SECTION("a later run") {
    const auto why = FaultInScope();
    REQUIRE(why.has_value());
    CHECK_THAT(*why, Catch::Matchers::ContainsSubstring("SIGSEGV"));
  }
  SECTION("and another") {
    const auto why = FaultInScope();
    REQUIRE(why.has_value());
    CHECK_THAT(*why, Catch::Matchers::ContainsSubstring("SIGSEGV"));
  }
}

// The case that found it: BRepOffset_MakeOffset offsetting the top of an
// imported battery holder 1 mm out faults in BRepOffset_Inter2d (plain OCC
// 7.9 does too, outside the app). In a later run of a case it killed the
// process instead of failing the call.
static std::optional<std::string> OffsetBatteryHolderTop() {
  STEPControl_Reader reader;
  REQUIRE(reader.ReadFile((std::string(TestSourceDir()) +
                           "/tests/fixtures/step/"
                           "battery_holder_box_y_f130_orig.step")
                              .c_str()) == IFSelect_RetDone);
  reader.TransferRoots();
  const TopoDS_Shape shape = reader.OneShape();
  TopoDS_Face top;
  double topZ = -1e300;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Plane)
      continue;
    const double z = s.Plane().Location().Z();
    if (std::abs(s.Plane().Axis().Direction().Z()) > 0.99 && z > topZ) {
      topZ = z;
      top = f;
    }
  }
  REQUIRE_FALSE(top.IsNull());
  BRepOffset_MakeOffset maker;
  return occsig::RunCatchingOccSignals([&] {
    maker.Initialize(shape, 0.0, Precision::Confusion(), BRepOffset_Skin,
                     Standard_True, Standard_False, GeomAbs_Intersection,
                     Standard_False, Standard_False);
    maker.SetOffsetOnFace(top, 1.0);
    maker.MakeOffsetShape();
  });
}

// Not in the sanitized build: the fault long-jumps past the offset
// builder's destructors, and LeakSanitizer reports what they held. The null
// writes above are its case.
#if !defined(__SANITIZE_ADDRESS__)
TEST_CASE("A kernel fault in a later run is that call's failure", "[occ]") {
  SECTION("a first run arms the handlers") { CHECK(FaultInScope()); }
  SECTION("the offset faults in the second") {
    const auto why = OffsetBatteryHolderTop();
    REQUIRE(why.has_value());
    CHECK_THAT(*why, Catch::Matchers::ContainsSubstring("SIGSEGV"));
  }
}
#endif

TEST_CASE("A later test case still catches a fault", "[occ]") {
  const auto why = FaultInScope();
  REQUIRE(why.has_value());
  CHECK_THAT(*why, Catch::Matchers::ContainsSubstring("SIGSEGV"));
}

// OpenCASCADE keeps the handler stack a fault long-jumps through: one list
// for the whole process behind one mutex in OCCT 7.6 to 7.9, one per thread
// in OCCT 8.0 and with nix/patches/occt-error-handler-thread-local.patch.
// Either way a fault on one thread is that thread's failure, while every
// other thread is inside a scope of its own, and theirs carry on.
TEST_CASE("A fault on one thread is that thread's failure while others are "
          "in their own scopes",
          "[occ]") {
  constexpr int kThreads = 8;
  constexpr int kCalls = 200;
  std::vector<int> caught(kThreads, 0), clean(kThreads, 0), wrong(kThreads, 0);
  std::vector<std::thread> pool;
  for (int t = 0; t < kThreads; ++t)
    pool.emplace_back([&, t] {
      for (int i = 0; i < kCalls; ++i) {
        // Every other call faults; the rest open a nested scope and work.
        const bool fault = (i + t) % 2 == 0;
        bool innerFailed = false;
        const auto why = occsig::RunCatchingOccSignals([&] {
          if (fault) {
            NullWrite();
            return;
          }
          innerFailed = occsig::RunCatchingOccSignals([] {
                          volatile double x = 0.0;
                          for (int k = 0; k < 1000; ++k)
                            x = x + std::sqrt(static_cast<double>(k));
                        }).has_value();
        });
        if (fault && why && why->find("SIGSEGV") != std::string::npos)
          caught[t]++;
        else if (!fault && !why && !innerFailed)
          clean[t]++;
        else
          wrong[t]++;
      }
    });
  for (std::thread& th : pool)
    th.join();
  for (int t = 0; t < kThreads; ++t) {
    INFO("thread " << t);
    CHECK(caught[t] == kCalls / 2);
    CHECK(clean[t] == kCalls / 2);
    CHECK(wrong[t] == 0);
  }
}
