// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A heavy feature builds off the frame
// (docs/ui/long-operations.md#heavy-features). Regeneration stays on the main
// thread; a document that opts in (Document::SetBuildsOffFrame) stops at a
// heavy feature its caches miss and leaves a Request, which the UI hands to a
// `feature` job. The job runs the feature's own Execute on deep copies of its
// inputs and files the Result here, keyed by the feature's chain hash, where
// the regeneration that follows -- and every copy, undo or reopen in the
// session -- finds it.
//
// Nothing in this header touches the UI: the store and the request are pure
// data, and Run is what the worker calls.

#include "core/Configuration.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/PrintAllowance.h"
#include "core/SheetMetal.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heavy {

// A feature whose estimated build reaches this many seconds builds off the
// frame: fifteen frames at 60 Hz, a hold anyone sees.
inline constexpr double kOffFrameS = 0.25;

// Whether `f`, acting on `input`, builds off the frame on a document that
// builds heavy features so (Feature::MayBuildOffFrame, BuildCostS).
[[nodiscard]] bool BuildsOffFrame(const Feature& f,
                                  const std::vector<Body>& input);

// The evaluation a build produced: the bodies after the feature and the
// feature as its Execute left it -- exactly a regen cache entry.
struct Result {
  size_t hash = 0;
  std::vector<Body> bodiesAfter;
  std::shared_ptr<const Feature> executed;
};

// What a stopped regeneration hands over: everything the feature's Execute
// reads, copied so the document may change while the build runs. `input`
// still shares its shapes with the document; Detach (main thread) makes them
// the job's own.
struct Request {
  size_t index = 0;        // the feature's position when it was asked for
  uint64_t featureId = 0;  // ... and its identity
  std::string featureName; // for the job's label
  size_t hash = 0;         // its chain hash: the result's key
  double estimateS = 0.0;  // Feature::BuildCostS on `input`
  std::unique_ptr<Feature> feature;
  // Clones of every feature it references (VisitFeatureRefs), in history
  // order: what RegenContext::FindById searches.
  std::vector<std::unique_ptr<Feature>> refs;
  std::vector<Body> input;
  // The shapes `input` had before Detach, by body index: a body the build
  // leaves untouched gets its document shape back, so the renderers do not
  // tessellate a copy of what they already hold.
  std::vector<CADShape> original;
  SymbolTable symbols;
  std::optional<Configuration> configuration;
  std::optional<sheet::SheetMetalRules> sheetMetal;
  std::optional<printfit::Process> print;
  glm::dvec3 printUp{0.0, 0.0, 1.0};
  bool detached = false;
};

// Deep-copies the request's input bodies (CADShape::DetachedUnlocated, put
// back at their locations). MAIN THREAD: the copy reads the shapes the frame
// meshes.
void Detach(Request& request);

// Builds the request on the calling thread (a worker), with `cancel` and
// `fraction` reachable from the builders' progress indicators (Interrupt,
// below). nullopt when the build was cancelled; a feature that fails is a
// Result whose `executed` carries the error, as a regeneration would leave
// it.
std::optional<Result> Run(Request& request, const std::atomic<bool>* cancel,
                          std::atomic<double>* fraction);

// The build's cancel and progress, for the code it runs: the direct-edit
// builders' deadline (directedit::MakeDeadline) reads Cancelled() at every
// progress point and writes its fraction. Thread-local, set by Run.
bool Cancelled();
void ReportFraction(double f);

// The process-wide store of finished builds, keyed by chain hash -- the hash
// of every feature up to and including the built one, which is what makes
// it a content key across documents (a copy, a trial, a reopen). Bounded:
// the oldest out. Thread-safe.
std::optional<Result> Find(size_t hash);
void Put(Result result);
void ForgetAll(); // tests
size_t StoreSize();

// The test knob (tests only): every feature that may build off the frame
// does, whatever its estimate, and the worker first sleeps `delayMs` --
// polling its cancel -- so a test can watch it build and cancel it. Also set
// by LOBOLE_TEST_HEAVY_FORCE=1 and LOBOLE_TEST_HEAVY_DELAY_MS.
struct TestOptions {
  bool force = false;
  int delayMs = 0;
};
void SetTestOptions(const TestOptions& options);
TestOptions GetTestOptions();

} // namespace heavy
