// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// compare_scan and set_scan_deviation
// (docs/inspect/README.md#how-far-a-body-is-from-a-scan): read and refused
// on the frame, the comparison parked on a `deviation` job
// (remote/RemoteParking.h), answered when it lands. Their MethodDoc rows are
// the inspection table's (remote/RemoteInspection.cpp), which dispatches
// here.

#include "remote/RemoteParking.h"
#include "remote/RpcParams.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

class Document;

namespace ui {
struct AppState;
}

namespace remote {

// The answer of `m` when it is one of the two, on this thread; nullopt for
// any other method.
std::optional<nlohmann::json> HandleScanCompareRpc(ui::AppState& state,
                                                   Document* doc,
                                                   const std::string& m,
                                                   const RpcParams& p);

// The two parked on a `deviation` job; null for any other method, and for a
// call the handler refuses (it refuses again, in its own words).
std::unique_ptr<Parking> ParkScanCompareRequest(ui::AppState& state,
                                                const std::string& m,
                                                const RpcParams& p);

} // namespace remote
