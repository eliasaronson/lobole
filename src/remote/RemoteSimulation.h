#pragma once

#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// FEM study RPC handlers, carved out of RemoteServer's HandleSemantic if-chain
// (the RemoteConfigurations precedent). Returns the result json if `method` is
// one of the simulation RPCs (add_study..run_study, enter/exit_simulation,
// set_result_display), std::nullopt otherwise. Throws std::runtime_error on
// error. `doc` is the active part document (caller guarantees non-null).
namespace remote {

std::optional<nlohmann::json> HandleSimulationRpc(ui::AppState& state,
                                                  Document& doc,
                                                  const std::string& method,
                                                  const RpcParams& params);

}  // namespace remote
