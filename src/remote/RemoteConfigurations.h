#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <string>

class Document;
class AssemblyDocument;
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
                            const std::string& method, const RpcParams& params,
                            nlohmann::json& out);

// The same config RPCs on an assembly tab: for an assembly a configuration's
// override set is suppressed OCCURRENCE ids (`suppressed_occurrences`). Undo is
// the assembly snapshot history, not the command stack.
bool HandleAssemblyConfigurationRpc(ui::AppState& state,
                                    AssemblyDocument& asmDoc,
                                    const std::string& method,
                                    const RpcParams& params,
                                    nlohmann::json& out);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> ConfigurationMethodDocs();

} // namespace remote
