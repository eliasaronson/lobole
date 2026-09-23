#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// CAM RPC handlers (docs/cam/README.md): setups, tools, operations, toolpath
// generation and posting. Returns the result json if `method` is one of them,
// std::nullopt otherwise.
//
// Deliberately NOT here: anything that runs a machine. This interface writes
// a file and answers questions about it; sending it to a controller is the
// user's decision, made in their sender, after they have looked at it.
namespace remote {

std::optional<nlohmann::json> HandleCamRpc(ui::AppState& state, Document* doc,
                                           const std::string& method,
                                           const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> CamMethodDocs();

} // namespace remote
