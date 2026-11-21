// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The studies of a part or an assembly moved into simulation documents
// (docs/simulation/README.md#moving-studies-into-a-simulation).
//
// NOT called on open yet: the app calls these from the step that turns the
// study panel into the simulation tab (docs/plans/simulation-document.md
// #stages), which is also the step that stops saving the part's and the
// assembly's own study lists. Until then a project keeps its studies where
// they are, and a simulation document beside them is only what a project
// already holding one brought with it.

#include "core/fem/SimDocument.h"

#include <cstdint>
#include <string>
#include <vector>

class AssemblyDocument;
class Document;

namespace fem {

// One simulation made from studies that lived on a document, and the name
// the project would list it under.
struct MigratedSimulation {
  std::string name;
  SimDocument sim;
};

// Every study of `part` (whose project document id is `partId`, its name
// `partName`), in one simulation of the part in its active configuration:
// ids kept (study, load), each study's own mesh size and refinements kept on
// the study as its overrides and the document's mesh left at auto, so each
// study is planned as it was (SimDocument::Effective) and its input hash is
// the one its saved result was solved at. Each saved result moves with its
// study, stale until RestampSimByInputs finds it current. None when the part
// has no studies.
std::vector<MigratedSimulation>
SimulationsFromPart(const Document& part, uint64_t partId,
                    const std::string& partName);

// The studies of `assembly` (project id `assemblyId`), one simulation per
// distinct participants table (roles live on the document, SimDocument::
// Roles): studies that agree on who takes part and how share one, in the
// order the first of each appears; each study's table is cleared, since the
// document's stands for it. Two tables are the same when they list the same
// occurrences in the same roles, sizes and friction, in any order. The
// studies are otherwise moved as SimulationsFromPart moves them. Named
// "<assembly> simulation", numbered from 2 past the first.
std::vector<MigratedSimulation>
SimulationsFromAssembly(const AssemblyDocument& assembly, uint64_t assemblyId,
                        const std::string& assemblyName);

// The simulation holding study `studyId` of the document `documentId`
// (a requirement's {document, study}: the study keeps its id through the
// move, and the simulation names the document as its subject). Null when
// none does.
const SimDocument*
SimulationHoldingStudy(const std::vector<const SimDocument*>& sims,
                       uint64_t documentId, uint64_t studyId);

} // namespace fem
