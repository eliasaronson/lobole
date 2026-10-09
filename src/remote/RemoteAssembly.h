// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace ui {
struct AppState;
}

// Assembly RPC handlers (add_occurrence..) carved out of RemoteServer's
// HandleSemantic if-chain. Returns the result json if `method` is one of them,
// std::nullopt otherwise. Act on the active assembly tab / project.
namespace remote {

std::optional<nlohmann::json> HandleAssemblyRpc(ui::AppState& state,
                                                const std::string& method,
                                                const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> AssemblyMethodDocs();

} // namespace remote
