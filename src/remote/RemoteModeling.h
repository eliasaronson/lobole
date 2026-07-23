#pragma once

#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// Modeling / feature-tree RPC handlers (select_feature..list_variables) carved
// out of RemoteServer's HandleSemantic if-chain. Returns the result json if
// `method` is one of them, std::nullopt otherwise. All act on the active part
// document (caller guarantees non-null).
namespace remote {

std::optional<nlohmann::json> HandleModelingRpc(ui::AppState& state,
                                                Document& doc,
                                                const std::string& method,
                                                const RpcParams& params);

}  // namespace remote
