// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The studies of the simulation tab in front (or the one an RPC names):
// its SimDocument and its subject, a part or an assembly
// (docs/simulation/README.md#running-a-simulations-study). The panel, the
// glyphs, the legend and the field overlay all read through this, so a part
// subject and an assembly subject share one set of drawing code.

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/SimDocument.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui {

struct AppState;

struct StudyHost {
  // The simulation's document. Its subject is read, never edited, through
  // the host: `subjectAssembly` names its occurrences.
  fem::SimDocument* sim = nullptr;
  const AssemblyDocument* subjectAssembly = nullptr;
  // A part subject's document, live (its tab may be behind): what a face, a
  // body, a datum or a material is read from. Never edited through here.
  const Document* subjectPart = nullptr;
  // The simulation tab's slot id: what a run, a mesh report and a status
  // are keyed by (0 off a simulation).
  uint64_t simSlotId = 0;
  // What a study's expressions read: its part subject's symbols (which
  // fall back to the project's), or the project's on an assembly subject.
  const SymbolTable* symbols = nullptr;

  explicit operator bool() const { return sim != nullptr; }
  // The part whose geometry the studies stand on: a part subject. Null on
  // an assembly.
  const Document* GeometryPart() const { return subjectPart; }
  // The assembly whose occurrences the studies name: an assembly subject.
  const AssemblyDocument* Assembly() const { return subjectAssembly; }
  // The studies stand on an assembly's occurrences: loads name an
  // occurrence, roles are listed.
  bool OnAssembly() const { return subjectAssembly != nullptr; }
  // A study's participants table: the simulation's roles.
  const std::vector<fem::Participant>& Roles(const fem::Study&) const {
    return sim->Roles();
  }
  // Read-only: an edit is ModifyStudy, one undo step on the simulation.
  // Every accessor below wants a host that is true.
  const std::vector<fem::Study>& Studies() const { return sim->GetStudies(); }
  const fem::Study* Find(uint64_t id) const { return sim->FindStudy(id); }
  // A run's (and a load drag's preview's) write access
  // (SimDocument::StudyForRun).
  fem::Study* FindForRun(uint64_t id) const { return sim->StudyForRun(id); }
  const fem::StudyRuntime* Results(uint64_t id) const {
    return sim->StudyResults(id);
  }
  uint64_t ResultsEpoch() const { return sim->StudyResultsEpoch(); }
  uint64_t AllocateId() const { return sim->AllocateId(); }
  uint64_t Version() const { return sim->Version(); }
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
    const Occurrence* occ =
        subjectAssembly ? subjectAssembly->FindById(id) : nullptr;
    return occ ? occ->name : std::to_string(id);
  }
};

// For the length of an RPC on a simulation that need not be in front:
// StudyHostOf answers with that simulation, so every helper that reads the
// studies through the host (a mesh report, a check, an export) reads the
// one the call named. Nests, and puts back what it found. 0 = none.
class StudyHostOverride {
public:
  explicit StudyHostOverride(uint64_t simSlotId);
  ~StudyHostOverride();
  StudyHostOverride(const StudyHostOverride&) = delete;
  StudyHostOverride& operator=(const StudyHostOverride&) = delete;

private:
  uint64_t m_prev = 0;
};
uint64_t StudyHostOverrideSlot();

// The host of the simulation tab in front (or the StudyHostOverride's);
// false on any other tab. SimulationActions.cpp.
StudyHost StudyHostOf(AppState& state);
// Stale against the study and the subject documents it read (SimStudyStale).
bool StudyStale(AppState& state, uint64_t studyId);
// One undoable mutation of one study (SimUndoGuard); `desc` names nothing a
// snapshot keeps.
void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate);
// A new study, undoable; returns its id (0 off a simulation).
uint64_t AddStudyOn(AppState& state, const std::string& name);
// The contact patches of the active study of a simulation of an ASSEMBLY,
// planned on demand (the imprint is a boolean, so this is cached against the
// study's text and the subject's stamps) and empty on a part subject. What
// the picture tints as "what is actually clamped".
const std::vector<fem::ContactPatch>& SimulationContacts(AppState& state);
// The glued seams of the active assembly study (S2), from the same plan.
const std::vector<fem::BondPatch>& SimulationBonds(AppState& state);

} // namespace ui
