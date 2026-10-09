// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>

namespace ui {
struct AppState;
}

// Design requirements (docs/requirements/README.md): add_requirement,
// set_requirement, remove_requirement, list_requirements and
// check_requirements, and the clearance verbs that are now requirements
// (add_assertion, list_assertions, remove_assertion, verify_assertions).
// Returns the result json when `method` is one of them, std::nullopt
// otherwise.
namespace remote {

std::optional<nlohmann::json> HandleRequirementsRpc(ui::AppState& state,
                                                    const std::string& method,
                                                    const RpcParams& params);

std::span<const MethodDoc> RequirementMethodDocs();

// Every requirement's row with its state, as list_requirements answers it.
nlohmann::json RequirementRows(ui::AppState& state);

// Starts a check of every requirement as a job (the panel's Check). Returns
// the job id, 0 when every requirement was already current and nothing had
// to run.
uint64_t StartRequirementsCheck(ui::AppState& state, bool recompute);

} // namespace remote
