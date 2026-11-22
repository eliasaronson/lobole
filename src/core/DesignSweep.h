// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The design sweep (docs/requirements/README.md#design-sweep): the model
// regenerated over ranges of its variables, on copies, to find where it
// stops building or stops meeting its requirements.
//
// A design RANGE is not a tolerance. A tolerance is variation about a
// nominal the model already has and drives no geometry; a range is over the
// variables themselves, and every value in it is a different nominal the
// model is regenerated at. A sweep's points are configurations nobody saved.
//
// This file is the planning, the walk over the points and what a point
// records. It knows no AppState: the app (src/remote/RemoteRequirements.cpp)
// checks the request, writes each part it rebuilds to its saved form with a
// copy of the project variables and tables, and runs this on a worker, which
// reads them back as ReadProject's parallel load does -- so no OCC shape is
// shared with the open document.

#include "core/Document.h"
#include "core/LookupTable.h"
#include "core/Requirements.h"
#include "core/fem/SimDocument.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sweep {

inline constexpr int kDefaultSamples = 5;
inline constexpr int kMaxSamples = 33;
inline constexpr int kMaxRefine = 8;
inline constexpr int kMaxCornerVariables = 8;
inline constexpr size_t kDefaultMaxPoints = 64;
inline constexpr size_t kMaxPointsCeiling = 256;

struct Variable {
  std::string name;
  double min = 0.0, max = 0.0;
  uint64_t part = 0;  // 0: a project variable; else the part whose local one
  double value = 0.0; // its value now: the base every other point varies from
};

struct Spec {
  std::vector<Variable> variables;
  int samples = kDefaultSamples; // per variable, ends included
  bool corners = false;          // the box's corners and its centre too
  int refine = 0;                // bisections of each bracket found
};

// The points a spec plans, in walk order: one chunk per variable (its
// samples, evenly spaced, the others at their values), then -- corners --
// one of the box's 2^n corners and its centre. Each point is every swept
// variable's value, in `variables` order.
struct Chunk {
  int variable = -1; // -1: the corners chunk
  std::vector<std::vector<double>> points;
};
std::vector<Chunk> PlanChunks(const Spec& spec);

// What a spec costs at most: every sample and corner, and `refine`
// bisections on each side of each variable.
size_t PlannedCount(const Spec& spec);

// One part the sweep rebuilds: its slot id, its name, and its saved form.
struct PartCopy {
  uint64_t id = 0;
  std::string name;
  std::filesystem::path file;
};

// A study requirement a sweep measures (docs/requirements/README.md
// #design-sweep): at every point its simulation's study is planned on the
// rebuilt part, meshed and solved, unless a run of the same inputs is in
// the cache.
struct StudyMeasure {
  req::Requirement requirement;
  uint64_t part = 0; // the rebuilt part's slot id: the simulation's subject
  uint64_t study = 0;
  // The simulation as it is now, without its results (its meshes kept,
  // shared): each worker solves on a clone of its own.
  std::shared_ptr<const fem::SimDocument> sim;
};

// What a study solved at a sweep's point answers, by its run's input hash
// (fem::RunInputKey of fem::SimStudyInputKey): a rerun of the sweep, or a
// point whose inputs a stored result was solved at, solves nothing. The
// app keeps one for the session; workers read and write it under `mutex`.
struct StudyCache {
  std::mutex mutex;
  std::map<uint64_t, std::shared_ptr<const fem::StudyRuntime>> byHash;
  static constexpr size_t kCapacity = 64;
  std::shared_ptr<const fem::StudyRuntime> Find(uint64_t hash);
  void Put(uint64_t hash, std::shared_ptr<const fem::StudyRuntime> rt);
};

struct Inputs {
  Spec spec;
  std::vector<Parameter> projectParameters;
  std::vector<lookuptable::Table> tables;
  std::vector<PartCopy> parts;
  // Measured at every point: `expression`, and `mass`, `volume`,
  // `section_area`, `body_count` and `bbox` of a rebuilt part. The app lists
  // the rest as not swept.
  std::vector<req::Requirement> requirements;
  // Measured at every point too, each by a solve (or a cached one), under
  // `studyBudgetS` of solving in all: once it is spent, a point's study
  // reads unresolved, saying so, and the answer says `budget_spent`.
  std::vector<StudyMeasure> studies;
  double studyBudgetS = 0.0;
  double convergeAutoS = 0.0; // the converge_auto_s preference
  std::shared_ptr<StudyCache> studyCache;
  int holdMs = 0; // tests: how long each point waits first, cancellable
};

// What a running sweep reports, written by its workers and read by the
// app's main thread.
struct Progress {
  std::atomic<bool> cancel{false};
  std::atomic<size_t> done{0};
  std::mutex mutex;
  std::string label; // "cam_d = 27.5 (7 of 15)"; under `mutex`
};

// Runs the sweep on the calling thread and the workers it starts (at most
// parallel::Workers(), each walking its chunks in order on its own copy, so
// the regen cache re-executes only what the variable reaches). Cancel stops
// it between points and keeps what was measured. Returns {points, base,
// first_failure, cancelled}; throws when a copy cannot be read back.
nlohmann::json Run(const Inputs& in, Progress& progress);

// The value written into a copy for a swept variable: a number the
// evaluator reads back exactly.
std::string ValueText(double v);

} // namespace sweep
