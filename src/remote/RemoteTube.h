// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

// Tube-bending RPC handlers (docs/tube/README.md), a domain module like the
// sheet-metal one and for the same reason: the rules are a document-level
// object rather than a feature, so they need verbs.
namespace remote {

// Handles the tube RPCs. Returns true and fills `out` if `method` is one of
// them; false if it belongs elsewhere. Throws std::runtime_error on error.
// `doc` is the active part document (non-null, guaranteed by caller).
bool HandleTubeRpc(ui::AppState& state, Document* doc,
                   const std::string& method, const RpcParams& params,
                   nlohmann::json& out);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> TubeMethodDocs();

} // namespace remote
