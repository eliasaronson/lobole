// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PrintCheckActions.h"

#include "core/Document.h"
#include "core/PrintAllowance.h"

#include <stdexcept>

namespace ui {

PrintCheckRun RunPrintCheck(const Document& part,
                            const std::set<uint64_t>* only,
                            const PrintCheckAsk& ask) {
  PrintCheckRun run;
  std::vector<const Body*> bodies;
  const auto& all = part.GetBodies();
  for (size_t i = 0; i < all.size(); ++i) {
    // Derived reference bodies are other parts' geometry: never printed,
    // as never exported.
    if (all[i].reference || all[i].shape.GetShape().IsNull())
      continue;
    if (only != nullptr && !only->contains(all[i].createdBy))
      continue;
    bodies.push_back(&all[i]);
    run.bodyIndex.push_back(static_cast<int>(i));
  }
  if (bodies.empty())
    throw std::runtime_error(
        all.empty() ? "the part has no bodies to print"
                    : "only reference bodies (derived ghosts): nothing that "
                      "prints");

  printcheck::Options& o = run.options;
  if (ask.up)
    o.up = *ask.up;
  const auto& proc = part.PrintProcess();
  if (proc)
    run.process = printfit::Label(*proc);

  // A limit the caller states, else the process's, else none: an item with
  // no limit is skipped, never judged against a number nobody chose.
  bool overhang = true, bridge = true;
  if (ask.maxOverhangDeg) {
    o.maxOverhangDeg = *ask.maxOverhangDeg;
    run.overhangFrom = "asked";
  } else if (proc && printfit::MaxOverhangDeg(*proc) > 0.0) {
    o.maxOverhangDeg = printfit::MaxOverhangDeg(*proc);
    run.overhangFrom = "process";
    run.overhangSource = printfit::MaxOverhangSource(*proc);
  } else {
    overhang = false;
  }
  if (ask.maxBridgeMm) {
    o.maxBridgeMm = *ask.maxBridgeMm;
    run.bridgeFrom = "asked";
  } else if (proc && printfit::MaxBridgeMm(*proc) > 0.0) {
    o.maxBridgeMm = printfit::MaxBridgeMm(*proc);
    run.bridgeFrom = "process";
    run.bridgeSource = printfit::MaxBridgeSource(*proc);
  } else {
    bridge = false;
  }
  o.overhangAllowanceMm2 = ask.overhangAllowanceMm2.value_or(0.0);
  if (ask.minGapMm) {
    o.gapSideMm = *ask.minGapMm;
    run.gapFrom = "asked";
  } else if (proc) {
    if (const printfit::Table* t = printfit::Find(*proc);
        t != nullptr && printfit::SmallestGapSideMm(*t) > 0.0) {
      o.gapSideMm = printfit::SmallestGapSideMm(*t);
      run.gapFrom = "process";
    }
  }
  // The chords export_stl meshes this print at (ui::ExportPartStl).
  if (ask.stlDeflectionMm) {
    o.stlDeflectionMm = *ask.stlDeflectionMm;
    run.deflectionFrom = "asked";
  } else if (proc && printfit::StlDeflectionMm(*proc) > 0.0) {
    o.stlDeflectionMm = printfit::StlDeflectionMm(*proc);
    run.deflectionFrom = proc->profile && proc->profile->stlDeflectionMm > 0.0
                             ? "profile"
                             : "gap";
  }

  // The air a face needs over another piece (printfit::MinAirMm): stated,
  // else the print's; a process that is not FDM has none, and says so.
  std::string noAir;
  if (ask.minAirMm) {
    o.minAirMm = *ask.minAirMm;
    run.airFrom = "asked";
  } else if (proc && printfit::MinAirMm(*proc) > 0.0) {
    o.minAirMm = printfit::MinAirMm(*proc);
    run.airFrom = printfit::MinAirFrom(*proc);
    run.airSource = printfit::MinAirSource(*proc);
  } else if (proc) {
    noAir = printfit::MinAirSource(*proc) + "; state min_air_mm";
  }

  // The build volume is the printer's, so only a profile states one
  // (v179): the built-in tables are a process, not a printer, and nothing
  // guesses one.
  if (proc && proc->profile && proc->profile->bedMm) {
    const auto& b = *proc->profile->bedMm;
    o.bedMm = glm::dvec3(b[0], b[1], b[2]);
    o.bedExcludeMm = proc->profile->bedExcludeMm; // vNEXT
  } else if (proc && proc->profile) {
    o.bedNote = "print profile '" + proc->profile->name +
                "' states no bed_mm, the printer's build volume (Settings, "
                "Print profiles; or save_print_profile)";
  } else if (proc) {
    o.bedNote = "the part is printed on the built-in " + run.process +
                " table, which is no printer's: print it with a profile "
                "that states bed_mm";
  } else {
    o.bedNote = "the part has no print process, so no printer to fit "
                "(set_print_process with a profile that states bed_mm)";
  }

  run.report = printcheck::Check(bodies, o);
  const std::string unprinted =
      "the part has no print process: set_print_process, or state ";
  if (!overhang) {
    run.report.overhang.status = printcheck::Status::Skipped;
    run.report.overhang.note = unprinted + "max_overhang_deg";
    // The worst angle stands on its own; an area past a limit nobody chose
    // does not.
    run.report.overhang.limitDeg = 0.0;
    run.report.overhang.areaMm2 = 0.0;
    run.report.overhang.faces.clear();
  }
  if (!bridge) {
    run.report.bridges.status = printcheck::Status::Skipped;
    run.report.bridges.note = unprinted + "max_bridge_mm";
    run.report.bridges.limitMm = 0.0;
  }
  if (!o.minAirMm)
    run.report.stacking.note = noAir.empty() ? unprinted + "min_air_mm" : noAir;
  return run;
}

} // namespace ui
