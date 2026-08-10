#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

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

} // namespace remote
