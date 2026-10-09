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

namespace ui {
struct AppState;
}

// Drawing / sheet RPC handlers (create_drawing..) carved out of RemoteServer's
// HandleSemantic if-chain. Returns the result json if `method` is one of them,
// std::nullopt otherwise. Act on the active drawing tab / project.
namespace remote {

std::optional<nlohmann::json> HandleDrawingRpc(ui::AppState& state,
                                               const std::string& method,
                                               const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> DrawingMethodDocs();

// A drawing request that WAITS for its projection instead of projecting on
// the frame (docs/drawings/README.md#requests-that-wait): update_drawing,
// check_drawing, export_drawing and layout_balloons ask for the update on a
// `drawing` job and are answered when it lands; add_drawing_view and
// set_drawing_view make their change, let what it needs project the same
// way, and answer then; remove_drawing_view waits for a run in flight.
// nullptr: dispatch it now (nothing projects, or the handler will refuse it,
// in its own words). The server never parks on --headless, where the
// handlers' synchronous path holds no frame.
std::unique_ptr<Parking> ParkDrawingRequest(ui::AppState& state,
                                            const std::string& method,
                                            const RpcParams& params);

} // namespace remote
