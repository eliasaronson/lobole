// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/PrintAllowance.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Document;
class ThreadFeature;

namespace ui {
struct AppState;

// One side of a fit on a PRINTED part
// (docs/tolerances/README.md#printed-parts): the size the print should come out
// at, and what to model for it. Shared by check_fit, check_fits, and the
// tolerance editor, so all three say the same sentence.
struct PrintedDiameter {
  printfit::Process process;
  bool hole = true;               // a bore (the process shrinks it) or a shaft
  double basic = 0.0;             // the size the fit is at: what should PRINT
  double allowance = 0.0;         // signed, mm on the diameter
  double model = 0.0;             // basic + allowance: what to MODEL
  std::optional<double> modelled; // what the model measures, when measured
  std::string source;             // where the table row's number comes from
  // The class's band is finer than the process holds (IT width against the
  // process's ±repeatability) — said, with its source, not judged.
  std::string bandNote;
  // "H7 on a printed Ø8 bore (fdm petg): model Ø8.2"
  [[nodiscard]] std::string Sentence(const std::string& cls) const;
  // Modelled at what the allowance asks for (within a micrometre).
  [[nodiscard]] bool Compensated() const;
  // What `modelled` prints at.
  [[nodiscard]] double Prints() const;
};

// `basic` is the size the print should come out at; `modelled` the model's
// own diameter when it was measured. Refused (nullopt + why) for a process
// or size the table does not carry.
[[nodiscard]] std::optional<PrintedDiameter>
PrintedForBasic(const printfit::Process& p, bool hole, double basic,
                std::optional<double> modelled, const std::string& cls,
                std::string* why);
// From a measured model with no stated size: the basic is what the modelled
// diameter PRINTS at.
[[nodiscard]] std::optional<PrintedDiameter>
PrintedForModelled(const printfit::Process& p, bool hole, double modelled,
                   const std::string& cls, std::string* why);

// A Thread on a printed part: what it should be modelled at, and whether it
// is. The modelled-thread path's report (add_feature/modify_feature on a
// Thread, print_allowance, the Thread editor).
struct PrintedThread {
  uint64_t featureId = 0;
  std::string name, designation;
  bool external = false;
  bool resolved = false; // the face resolved (the overlay is valid)
  bool known = false;    // a table row, not a custom thread
  bool modeled = false;  // the groove is cut; a cosmetic thread prints nothing
  double pitch = 0.0;
  double allowance = 0.0;     // signed, on the diameter
  double modelledMajor = 0.0; // the major the feature cuts
  double printsMajor = 0.0;   // modelledMajor - allowance
  // Known threads only: the major the print should have (the table's) and
  // what to model for it, as the face a CUSTOM thread takes (a boss's face
  // is its major; a hole's wall its basic minor).
  double wantMajor = 0.0, modelMajor = 0.0, modelFace = 0.0;
  std::vector<std::string> notes;
  [[nodiscard]] std::string Sentence() const;
};
// nullopt when `doc` is not printed (or its process is not carried).
[[nodiscard]] std::optional<PrintedThread>
PrintedThreadFor(const Document& doc, const ThreadFeature& thread);

// One round joint of the active assembly, checked. Shared by the GUI and the
// `check_fits` RPC so a person and a script get the same verdict from the
// same walk — the arrangement CheckFasteners already uses.
struct FitCheck {
  uint64_t mateId = 0;
  std::string mateName;
  std::string mateType; // revolute | cylindrical | slider | fastened
  // pass | warn | fail | reported | unverified. Anything not fully
  // answerable is `unverified` WITH a reason and never a pass.
  std::string status;
  std::string reason;
  // Set only when the fit was computed (status != "unverified").
  bool computed = false;
  std::string holeOccurrence, shaftOccurrence;
  std::string holeClass, shaftClass;
  double size = 0.0;
  double holeMin = 0.0, holeMax = 0.0;
  double shaftMin = 0.0, shaftMax = 0.0;
  double minClearance = 0.0, maxClearance = 0.0;
  std::string kind; // clearance | transition | interference
  // A side on a printed part: the fit is at the size it PRINTS, and this says
  // what to model for it.
  std::optional<PrintedDiameter> holePrint, shaftPrint;

  // One line a person can act on, for the panel and for a log.
  [[nodiscard]] std::string Sentence() const;
};

// Every fit the active assembly declares. Empty (and harmless) off an
// assembly tab.
[[nodiscard]] std::vector<FitCheck> CheckFits(AppState& state);

// The tolerance editor inside the Measure tool window: ONE picked face is a
// tolerance on its size, TWO are a tolerance on the distance between them —
// which is exactly the shape of a Tolerance's ref/ref2, so measuring and
// tolerancing are the same gesture. Draws nothing off a part tab, or when the
// picks are not two named elements of this part.
void DrawTolerancePick(AppState& state);

// Every tolerance the active part carries, as a section of the Variables
// panel — its neighbours there are the other authored numbers that sit
// outside the regen chain.
void DrawTolerancesSection(AppState& state);

// How the active part is printed (docs/tolerances/README.md#printed-parts):
// a picker over the carried process/material pairs, the table's rows with
// their sources, and the part's threads with what to model for each. A
// section of the Variables panel beside the tolerances it qualifies.
void DrawPrintProcessSection(AppState& state);

} // namespace ui
