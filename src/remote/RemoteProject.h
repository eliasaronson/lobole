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

// Refuse a tab name that already exists. Two tabs answering to one name make
// name-addressing ambiguous and every listing confusing — a died-and-retried
// script once minted two identical assemblies with nothing said.
void RequireFreshTabName(ui::AppState& state, const std::string& name);

// "Part N" that is actually free: the parts.size()+1 default collided after
// a deletion, and with duplicate names refused the default must never refuse.
std::string FreshDefaultTabName(ui::AppState& state, const char* stem);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> ProjectMethodDocs();

} // namespace remote
