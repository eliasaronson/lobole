// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Where the active tab's studies live
// (docs/simulation/README.md#assembly-studies): on the PART, or on the
// ASSEMBLY. The record is the same fem::Study; what differs is who owns it, how
// an edit becomes an undo step (a command on a part, a full-state snapshot on
// an assembly), and what a result's staleness is measured against. The panel,
// the glyphs, the legend and the field overlay all read through this so there
// is one set of drawing code for both tabs.

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui {

struct AppState;

struct StudyHost {
  Document* part = nullptr;
  AssemblyDocument* asmDoc = nullptr;
  // What a study's expressions read: the part's symbols (which fall back to
  // the project's), or the project's on an assembly tab.
  const SymbolTable* symbols = nullptr;

  explicit operator bool() const {
    return part != nullptr || asmDoc != nullptr;
  }
  bool IsAssembly() const { return asmDoc != nullptr; }
  // Read-only: an edit is ModifyStudy, which a part records as a command
  // (and so as a saved change, Document::SaveStamp).
  const std::vector<fem::Study>& Studies() const {
    return part ? part->GetStudies() : asmDoc->GetStudies();
  }
  const fem::Study* Find(uint64_t id) const {
    return part ? part->FindStudy(id) : asmDoc->FindStudy(id);
  }
  // A run's (and a load drag's preview's) write access — Document::StudyForRun.
  fem::Study* FindForRun(uint64_t id) const {
    return part ? part->StudyForRun(id) : asmDoc->FindStudy(id);
  }
  const fem::StudyRuntime* Results(uint64_t id) const {
    return part ? part->StudyResults(id) : asmDoc->StudyResults(id);
  }
  uint64_t ResultsEpoch() const {
    return part ? part->StudyResultsEpoch() : asmDoc->StudyResultsEpoch();
  }
  uint64_t AllocateId() const {
    return part ? part->AllocateId() : asmDoc->AllocateId();
  }
  uint64_t Version() const {
    return part ? part->Version() : asmDoc->Version();
  }
  // `s` with every expression-driven field (a load's magnitude, a mesh
  // size) evaluated against the current variables, for DISPLAY: a study's
  // stored values are evaluated lazily, at its run, so a load typed as
  // "load_n" holds whatever its last run left there (0 before any run, or
  // when the run's answer was reused from a twin's). The panel, the
  // viewport's labels and a drag's starting value read this; the stored
  // study is not touched. A field that does not evaluate keeps its stored
  // value.
  fem::Study Shown(const fem::Study& s) const {
    fem::Study out = s;
    if (symbols != nullptr)
      (void)fem::EvaluateStudyExprs(out, *symbols, nullptr);
    return out;
  }
  fem::Load Shown(const fem::Load& l) const {
    fem::Study one;
    one.loads.push_back(l);
    return Shown(one).loads[0];
  }
  // The name of an occurrence of the assembly (its id as text when there is
  // no assembly, or no such occurrence).
  std::string OccurrenceName(uint64_t id) const {
    const Occurrence* occ = asmDoc ? asmDoc->FindById(id) : nullptr;
    return occ ? occ->name : std::to_string(id);
  }
};

// The active tab's host; false on a drawing or job tab. SimulationActions.cpp.
StudyHost StudyHostOf(AppState& state);
// Stale against the part, or against the assembly AND every part it read.
bool StudyStale(AppState& state, uint64_t studyId);
// One undoable mutation of one study, on either host.
void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate);
// A new study, undoable, on either host; returns its id.
uint64_t AddStudyOn(AppState& state, const std::string& name);
// The contact patches of the active ASSEMBLY study, planned on demand (the
// imprint is a boolean, so this is cached against the study's text and the
// documents' versions) and empty on a part tab. What the picture tints as
// "what is actually clamped".
const std::vector<fem::ContactPatch>& SimulationContacts(AppState& state);
// The glued seams of the active assembly study (S2), from the same plan.
const std::vector<fem::BondPatch>& SimulationBonds(AppState& state);

} // namespace ui
