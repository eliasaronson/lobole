// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A printed body's slicer settings, its printed mass and the print job
// (docs/tolerances/README.md#print-jobs): one place that resolves what a
// body prints with, so mass_properties, the study panel, export_print_job
// and the Print process section cannot disagree.

#include "core/PrintSettings.h"
#include "core/PrintedMass.h"
#include "core/io/PrintJob3mf.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

class Document;
struct Body;
namespace printfit {
struct Process;
}

namespace ui {

struct AppState;

// What a body prints with. A plate's piece is a Derived copy, and its
// settings are its SOURCE part's (that part's, under its override for the
// source body), between the plate part's own and the plate's override for
// the piece:
//   plate part < source part < source body < plate body.
// The print direction is never carried from the source: a piece's is its
// plate's (+Z, the bed's) unless the plate says otherwise.
struct BodyPrint {
  printset::Settings settings;
  // The part's process, else (a Derived copy on an unprinted part) its
  // source's; null when neither is printed.
  const printfit::Process* process = nullptr;
  const Document* source = nullptr; // the source part of a Derived copy
};
[[nodiscard]] BodyPrint PrintOf(AppState& state, const Document& part,
                                const Body& body);

// The printed estimate of `body` (core/PrintedMass.h); nullopt with `why`.
[[nodiscard]] std::optional<printmass::Result>
PrintedMassOf(AppState& state, const Document& part, const Body& body,
              std::string* why);

// The filament a body is printed in: its material when that is a filament
// (pla, petg, tpu), else -- a body with no material -- the process's.
// nullopt with `why` for a material that is not a filament: what the mass
// weighs must be what the slicer is loaded with.
[[nodiscard]] std::optional<std::string>
FilamentOf(const Body& body, const printfit::Process* process,
           std::string* why);
// The slicer's filament_type of a filament slug ("PLA"), or "".
[[nodiscard]] std::string SlicerFilamentType(const std::string& filament);

struct JobPlateAsk {
  Document* part = nullptr;
  std::string name; // the plate's name in the job
};

struct JobBuilt {
  printjob::Job job;
  nlohmann::json summary; // the RPC's answer, less the path
};

// Every check export_print_job makes, then the job. Refused (unexpected,
// naming what) before anything is written: a plate whose part is not
// printed or has nothing to print, plates of different printers or layers,
// pieces of more than one filament (a job is one spool), a piece whose
// settings or filament do not resolve, a plate that fails bed_fit (the
// profile's bed and keep-out zones, positional), more than one plate
// without a project config, or a config that is incomplete or for another
// printer, layer or filament.
[[nodiscard]] std::expected<JobBuilt, std::string>
BuildPrintJob(AppState& state, const std::vector<JobPlateAsk>& plates,
              const std::optional<nlohmann::json>& projectConfig,
              const std::string& jobName);

} // namespace ui
