#pragma once

// STEP export (X4, docs/interchange/step-export-plan.md): the one place that
// turns a document into what CADKernel::SaveSTEPModel writes. The RPC and
// the Export dialog both come through here, so a file carries the same
// names, colours and structure whichever way it was asked for. (The tube
// shop writes single members, so it builds its one-product model itself.)

#include "cad/CADKernel.h"
#include "ui/AppState.h"

#include <cstddef>
#include <expected>
#include <string>

class Document;

namespace ui {

struct StepExportCounts {
  size_t bodies = 0;    // placed body shapes (an occurrence of a 2-body part
                        // counts 2): what the file holds, solid by solid
  size_t products = 0;  // distinct parts written
  size_t instances = 0; // placed parts (1 for a part export)
};

// A part: one product named `name`, one body product per non-reference body
// (only `bodyIndex`'s when >= 0; it indexes the non-reference bodies, as
// get_state lists them). Refuses a part with nothing to write.
std::expected<CADKernel::StepExportModel, std::string>
BuildPartStepModel(const Document& part, const std::string& name,
                   int bodyIndex = -1);

// The ACTIVE assembly, as its tree: every part once, placed by reference;
// every sub-assembly once per (document, configuration), nested. Hidden and
// suppressed occurrences are left out, as the viewport leaves them out.
std::expected<CADKernel::StepExportModel, std::string>
BuildAssemblyStepModel(AppState& state);

// Build + write. `part` null exports the active assembly; otherwise that part,
// named after its tab.
std::expected<StepExportCounts, std::string>
ExportStep(AppState& state, const Document* part, int bodyIndex,
           const std::string& path);

// What a model holds, for the RPC's reply and the tests.
StepExportCounts CountStepModel(const CADKernel::StepExportModel& model);

} // namespace ui
