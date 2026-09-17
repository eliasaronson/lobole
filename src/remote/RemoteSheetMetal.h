#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <span>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// Sheet-metal RPC handlers (docs/sheetmetal/plan.md), a domain module of its
// own like the configuration one: the rules are a document-level object, not
// a feature, so they need verbs rather than an add_feature dialect.
namespace remote {

// Handles the sheet-metal RPCs. Returns true and fills `out` if `method` is
// one of them; false if it belongs elsewhere. Throws std::runtime_error on
// error. `doc` is the active part document (non-null, guaranteed by caller).
bool HandleSheetMetalRpc(ui::AppState& state, Document& doc,
                         const std::string& method, const RpcParams& params,
                         nlohmann::json& out);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> SheetMetalMethodDocs();

} // namespace remote
