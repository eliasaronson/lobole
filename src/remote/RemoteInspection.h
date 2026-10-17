// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RemoteParking.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <memory>
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

// An exact `interference` between two NAMED sides waits instead of
// computing on the frame (docs/inspect/README.md#threads): the handler
// resolves the pair and deep-copies its bodies here, on the main thread, and
// the boolean and the distance run on an `interference` background job; the
// request is answered when it lands. A thread's helicoids cost seconds a
// pair, which held the frame past the watchdog. nullptr: dispatch it now (a
// sweep, mesh mode, or a request the handler refuses in its own words).
// `request` is the params as sent: what is asked is read off it without
// marking a key read, so the handler alone decides which keys it takes.
std::unique_ptr<Parking> ParkInspectionRequest(ui::AppState& state,
                                               const std::string& method,
                                               const nlohmann::json& request,
                                               const RpcParams& params);

// A printed diameter / a Thread on a printed part, on the wire
// (docs/tolerances/README.md#printed-parts). The Thread form is also what
// add_feature and modify_feature attach when they build a Thread on a
// printed part.
nlohmann::json PrintedDiameterJson(const ui::PrintedDiameter& d,
                                   const std::string& cls);
nlohmann::json PrintedThreadJson(const ui::PrintedThread& t);

} // namespace remote
