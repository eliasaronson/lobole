#pragma once

#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// Inspection RPC handlers (section clip, interference, mass_properties, measure,
// list_elements) carved out of RemoteServer's HandleSemantic. Returns the
// result json if `method` is one of them, std::nullopt otherwise. `doc` may be
// null on an assembly tab: interference then runs against occurrences, and
// list_elements/measure still work when given an explicit `part` param.
namespace remote {

std::optional<nlohmann::json> HandleInspectionRpc(ui::AppState& state,
                                                  Document* doc,
                                                  const std::string& method,
                                                  const RpcParams& params);

}  // namespace remote
