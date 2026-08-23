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

// Resolve a `part` RPC argument to its index in state.parts. Accepts a tab
// NAME, a stable document id, or a slot index — including any of those
// passed as a string, since a client that reads list_parts and echoes a
// field back should not have to know which kind it got. Throws a
// decision-complete error naming the existing tabs when nothing matches.
//
// One addressing rule for every tab-level RPC (rename_part, the folder
// RPCs): the alternative is each one growing its own subtly different copy.
size_t ResolveSlotIndex(ui::AppState& state, const nlohmann::json& sel);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> ProjectMethodDocs();

} // namespace remote
