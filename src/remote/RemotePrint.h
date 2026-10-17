// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <span>
#include <string>

class Document;
struct Body;
namespace ui {
struct AppState;
}

// A printed part's slicer settings and its print job
// (docs/tolerances/README.md#slicer-settings, #print-jobs): set_print_settings,
// get_print_settings and export_print_job. A domain module of its own,
// beside the print process's verbs in RemoteInspection.
namespace remote {

// Returns true and fills `out` if `method` is one of this module's.
// `doc` is the active part, or null. Throws std::runtime_error on error.
bool HandlePrintRpc(ui::AppState& state, Document* doc,
                    const std::string& method, const RpcParams& params,
                    nlohmann::json& out);

std::span<const MethodDoc> PrintMethodDocs();

// mass_properties' `printed` for one body of `part`: the estimate
// {mass_g, volume_mm3, shell_mm3, core_mm3, solid_mm3, center_of_mass_mm,
// inertia, layers, pixel_mm, settings, method}, or {why} when there is none
// (the part not printed, or a setting unstated). Null for a part that is
// not printed at all.
nlohmann::json PrintedMassJson(ui::AppState& state, const Document& part,
                               const Body& body, double density);

} // namespace remote
