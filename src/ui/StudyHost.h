// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Where the active tab's studies live
// (docs/simulation/README.md#assembly-studies): on the PART, on the
// ASSEMBLY, or in a SIMULATION document (its tab;
// docs/simulation/README.md#running-a-simulations-study). The record is the
// same fem::Study; what differs is who owns it, how an edit becomes an undo
// step (a command on a part, a full-state snapshot on an assembly), and what a
// result's staleness is measured against. The panel, the glyphs, the legend and
// the field overlay all read through this so there is one set of drawing code
// for both tabs.

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
  Document* part = nullptr;
  AssemblyDocument* asmDoc = nullptr;
  // A simulation tab's document. Its subject is read, never edited, through
  // the host: `subjectAssembly` names its occurrences.
  fem::SimDocument* sim = nullptr;
  const AssemblyDocument* subjectAssembly = nullptr;
  // A part subject's document, live (its tab may be behind): what a face, a
  // body, a datum or a material is read from. Never edited through here.
  const Document* subjectPart = nullptr;
  // The simulation tab's slot id: what a run, a mesh report and a status
  // are keyed by (0 off a simulation).
  uint64_t simSlotId = 0;
  // What a study's expressions read: the part's symbols (which fall back to
  // the project's), or the project's on an assembly tab; on a simulation,
  // its subject's.
  const SymbolTable* symbols = nullptr;

  explicit operator bool() const {
    return part != nullptr || asmDoc != nullptr || sim != nullptr;
  }
  // The studies are an ASSEMBLY's own (not a simulation's of one).
  bool IsAssembly() const { return asmDoc != nullptr; }
  bool IsSimulation() const { return sim != nullptr; }
  // The part whose geometry the studies stand on: the part tab's own, or a
  // simulation's part subject. Null on an assembly.
  const Document* GeometryPart() const { return part ? part : subjectPart; }
  // The assembly whose occurrences the studies name: the tab's own, or a
  // simulation's assembly subject.
  const AssemblyDocument* Assembly() const {
    return asmDoc ? asmDoc : subjectAssembly;
  }
  // The studies stand on an assembly's occurrences (its own or as a
  // simulation's subject): loads name an occurrence, roles are listed.
  bool OnAssembly() const { return Assembly() != nullptr; }
  // A study's participants table: a simulation's roles are its document's.
  const std::vector<fem::Participant>& Roles(const fem::Study& s) const {
    return sim ? sim->Roles() : s.participants;
  }
  // Read-only: an edit is ModifyStudy, which a part records as a command
  // (and so as a saved change, Document::SaveStamp).
  const std::vector<fem::Study>& Studies() const {
    return sim    ? sim->GetStudies()
           : part ? part->GetStudies()
                  : asmDoc->GetStudies();
  }
  const fem::Study* Find(uint64_t id) const {
    return sim    ? sim->FindStudy(id)
           : part ? part->FindStudy(id)
                  : asmDoc->FindStudy(id);
  }
  // A run's (and a load drag's preview's) write access — Document::StudyForRun.
  fem::Study* FindForRun(uint64_t id) const {
    return sim    ? sim->StudyForRun(id)
           : part ? part->StudyForRun(id)
                  : asmDoc->FindStudy(id);
  }
  const fem::StudyRuntime* Results(uint64_t id) const {
    return sim    ? sim->StudyResults(id)
           : part ? part->StudyResults(id)
                  : asmDoc->StudyResults(id);
  }
  uint64_t ResultsEpoch() const {
    return sim    ? sim->StudyResultsEpoch()
           : part ? part->StudyResultsEpoch()
                  : asmDoc->StudyResultsEpoch();
  }
  uint64_t AllocateId() const {
    return sim    ? sim->AllocateId()
           : part ? part->AllocateId()
                  : asmDoc->AllocateId();
  }
  uint64_t Version() const {
    return sim ? sim->Version() : part ? part->Version() : asmDoc->Version();
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
    const AssemblyDocument* a = asmDoc ? asmDoc : subjectAssembly;
    const Occurrence* occ = a ? a->FindById(id) : nullptr;
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

// The active tab's host; false on a drawing or job tab. SimulationActions.cpp.
// On a simulation tab it is the simulation (`sim`), with neither `part` nor
// `asmDoc`: what reads a part's or an assembly's own studies checks those.
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
