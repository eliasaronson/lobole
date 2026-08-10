#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace ui {
struct AppState;
}

// Project IO / checkpoint / history / export-import RPC handlers
// (new_project..) carved out of RemoteServer's HandleSemantic if-chain. Returns
// the result json if `method` is one of them, std::nullopt otherwise.
namespace remote {

std::optional<nlohmann::json> HandleProjectRpc(ui::AppState& state,
                                               const std::string& method,
                                               const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> ProjectMethodDocs();

} // namespace remote
