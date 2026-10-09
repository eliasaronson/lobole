// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/cam/CamJob.h"
#include "core/cam/CamSetupSheet.h"

#include <cstdint>
#include <string>
#include <vector>

// A setup sheet per setup (docs/cam/setup.md#the-setup-sheet): what the job
// knows, gathered for the page core/cam/CamSetupSheet writes. One entry for
// the RPC (export_setup_sheet) and the Generate window's button.
namespace ui {

struct AppState;

// The program's file name Save G-code writes for `setup`: "<project> -
// <setup>.nc" beside the project (one per tool with split output: the
// first's name and "-T<n>" said). Empty for an unsaved project.
[[nodiscard]] std::string SetupProgramName(const AppState& state,
                                           const cam::Setup& setup);

// Everything the page says about `setup`, from `paths` -- its toolpaths in
// cutting order, none pending -- and the program they post to. False with
// `error` when the setup does not post (the sheet would describe a program
// that does not exist).
bool SetupSheetOf(AppState& state, const cam::Setup& setup,
                  const std::vector<cam::Toolpath>& paths,
                  const std::string& program, cam::SetupSheet& sheet,
                  std::string* error);

// The picture: the job tab showing `setup` -- the part where it sits in the
// stock, in its fixtures -- from the iso view, framed, through the quick
// still. The camera and the shown setup are put back (another setup's
// playback is closed). False, with the caption saying why, where there is no
// renderer.
bool SetupSheetPicture(AppState& state, const cam::Setup& setup,
                       cam::SetupSheet& sheet);

// All of it, written to `path` (a .pdf). `program` empty: SetupProgramName.
// `picture` false leaves the picture out. False with `error`; `warnings`
// gets what the sheet could not say (no picture, no renderer).
struct SetupSheetResult {
  bool picture = false;
  std::string program;
  double cycleMinutes = 0.0;
  size_t tools = 0, operations = 0;
  std::vector<std::string> warnings;
};
bool ExportSetupSheet(AppState& state, uint64_t setupId,
                      const std::vector<cam::Toolpath>& paths,
                      const std::string& path, const std::string& program,
                      bool picture, SetupSheetResult& result,
                      std::string* error);

// The Generate window's button, run from the frame (BackgroundJobs.cpp): the
// active setup's sheet beside the project, its path or why not left in
// `camLastPost`.
void PumpSetupSheet(AppState& state);

} // namespace ui
