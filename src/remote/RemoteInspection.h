// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/MeasureInsertion.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteParking.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

class Document;
namespace Measure {
struct SealReport;
}
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

// check_seal's answer for a report (docs/inspect/README.md#seals): sealed,
// least_width_mm (the widest closed ring's narrowest; 0 when none closes),
// rings[], open[] (arcs with no contact), the axis and how it was found.
nlohmann::json SealJson(const Measure::SealReport& r);

// check_insertion's reading of everything but its two sides
// (docs/inspect/README.md#insertion), shared with the `insertion`
// requirement: the axis (`mate`, `axis`, or the one mate joining a and b),
// `entry`, and the options. Every parameter is checked before anything
// runs; a refusal names what is accepted.
struct InsertionCall {
  glm::dvec3 point{0.0}, dir{0.0, 0.0, 1.0};
  std::string axisFrom;
  Measure::InsertionOptions opt;
};
InsertionCall ReadInsertionCall(ui::AppState& state, const nlohmann::json& p,
                                uint64_t a, uint64_t b);
nlohmann::json InsertionJson(const Measure::InsertionReport& r,
                             const std::string& axisFrom);

} // namespace remote
