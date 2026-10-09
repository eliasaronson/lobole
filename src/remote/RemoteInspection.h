// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
struct PrintedDiameter;
struct PrintedThread;
} // namespace ui

// Inspection RPC handlers (section clip, interference, mass_properties,
// measure, list_elements) carved out of RemoteServer's HandleSemantic. Returns
// the result json if `method` is one of them, std::nullopt otherwise. `doc` may
// be null on an assembly tab: interference then runs against occurrences, and
// list_elements/measure still work when given an explicit `part` param.
namespace remote {

std::optional<nlohmann::json> HandleInspectionRpc(ui::AppState& state,
                                                  Document* doc,
                                                  const std::string& method,
                                                  const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> InspectionMethodDocs();

// A printed diameter / a Thread on a printed part, on the wire
// (docs/tolerances/README.md#printed-parts). The Thread form is also what
// add_feature and modify_feature attach when they build a Thread on a
// printed part.
nlohmann::json PrintedDiameterJson(const ui::PrintedDiameter& d,
                                   const std::string& cls);
nlohmann::json PrintedThreadJson(const ui::PrintedThread& t);

} // namespace remote
