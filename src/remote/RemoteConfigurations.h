#pragma once

#include <nlohmann/json.hpp>

#include <string>

class Document;
namespace ui {
struct AppState;
}

// Configuration RPC handlers, carved out of RemoteServer's HandleSemantic
// if-chain (roadmap decision 7: split doc-level-object RPCs into modules rather
// than pile onto the 108-branch dispatch). The same pattern will absorb the
// study/drawing blocks over time.
namespace remote {

// Handles the configuration RPCs (add / remove / list / set_configuration,
// set_active_configuration). Returns true and fills `out` if `method` is one of
// them; false if it belongs elsewhere. Throws std::runtime_error on error.
// `doc` is the active part document (the caller guarantees it is non-null).
bool HandleConfigurationRpc(ui::AppState& state, Document& doc,
                            const std::string& method,
                            const nlohmann::json& params, nlohmann::json& out);

}  // namespace remote
