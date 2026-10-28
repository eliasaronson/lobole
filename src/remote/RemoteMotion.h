// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ui {
struct AppState;
}

// Motion study RPCs (docs/assemblies/README.md#motion-studies): define a study
// on the active assembly, run it (synchronously or as a background job), read
// the collision table, scrub a pose, export the animation. Returns the result
// json when `method` is one of them, std::nullopt otherwise.
namespace remote {

std::optional<nlohmann::json> HandleMotionRpc(ui::AppState& state,
                                              const std::string& method,
                                              const RpcParams& params);

std::span<const MethodDoc> MotionMethodDocs();

// check_followers (docs/assemblies/relations.md#the-follower-check): per
// contact relation, over the study's stored run. Throws a refusal; the
// result is {study, speed_rpm?, relations: [...]}. `relations` empty means
// every contact relation in the active assembly.
nlohmann::json CheckFollowers(ui::AppState& state, uint64_t study,
                              const std::vector<uint64_t>& relations,
                              std::optional<double> speedRpm,
                              std::optional<double> maxPressureDeg);

} // namespace remote
