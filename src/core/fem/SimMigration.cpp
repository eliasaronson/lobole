// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/SimMigration.h"

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h" // StudyRuntime

#include <algorithm>
#include <tuple>

namespace fem {

namespace {

// A study moved as it is: its ids, its own mesh size and refinements (now
// overrides of the document's auto mesh), its result beside it.
void MoveStudy(SimDocument& sim, Study study, const StudyRuntime* result) {
  const uint64_t id = study.id;
  sim.RestoreStudy(std::move(study));
  if (result != nullptr)
    sim.RestoreStudyResults(id, *result);
}

// The table as a sorted list of what each row says, so two tables listing
// the same rows in another order compare equal.
using Row = std::tuple<uint64_t, int, double, std::string, double>;
std::vector<Row> TableOf(const std::vector<Participant>& table) {
  std::vector<Row> rows;
  for (const Participant& p : table)
    rows.emplace_back(p.occurrenceId, static_cast<int>(p.role),
                      p.meshSize.value, p.meshSize.expr, p.friction);
  std::sort(rows.begin(), rows.end());
  return rows;
}

SimDocument NewSimulation(SubjectKind kind, uint64_t docId,
                          const std::string& name) {
  SimDocument sim;
  sim.SetSubject({kind, docId, 0, name});
  // The document's mesh stays auto: every study keeps the size it had, as
  // an override, so it is planned and hashed as it was.
  return sim;
}

} // namespace

std::vector<MigratedSimulation>
SimulationsFromPart(const Document& part, uint64_t partId,
                    const std::string& partName) {
  std::vector<MigratedSimulation> out;
  if (part.GetStudies().empty())
    return out;
  MigratedSimulation m{partName + " simulation",
                       NewSimulation(SubjectKind::Part, partId, partName)};
  for (const Study& s : part.GetStudies())
    MoveStudy(m.sim, s, part.StudyResults(s.id));
  out.push_back(std::move(m));
  return out;
}

std::vector<MigratedSimulation>
SimulationsFromAssembly(const AssemblyDocument& assembly, uint64_t assemblyId,
                        const std::string& assemblyName) {
  std::vector<MigratedSimulation> out;
  std::vector<std::vector<Row>> tables;
  for (const Study& s : assembly.GetStudies()) {
    const std::vector<Row> table = TableOf(s.participants);
    size_t k = 0;
    while (k < tables.size() && tables[k] != table)
      ++k;
    if (k == tables.size()) {
      tables.push_back(table);
      std::string name = assemblyName + " simulation";
      if (k > 0)
        name += " " + std::to_string(k + 1);
      MigratedSimulation m{
          std::move(name),
          NewSimulation(SubjectKind::Assembly, assemblyId, assemblyName)};
      m.sim.SetRoles(s.participants);
      out.push_back(std::move(m));
    }
    Study moved = s;
    moved.participants.clear(); // the document's roles stand for it
    MoveStudy(out[k].sim, std::move(moved), assembly.StudyResults(s.id));
  }
  return out;
}

const SimDocument*
SimulationHoldingStudy(const std::vector<const SimDocument*>& sims,
                       uint64_t documentId, uint64_t studyId) {
  for (const SimDocument* sim : sims)
    if (sim != nullptr && sim->Subject().docId == documentId &&
        sim->FindStudy(studyId) != nullptr)
      return sim;
  return nullptr;
}

} // namespace fem
