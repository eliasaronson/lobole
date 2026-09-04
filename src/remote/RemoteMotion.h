#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace ui {
struct AppState;
}

// Motion study RPCs (docs/agent-scale-plan.md, G2): define a study on the
// active assembly, run it (synchronously or as a background job), read the
// collision table, scrub a pose, export the animation. Returns the result
// json when `method` is one of them, std::nullopt otherwise.
namespace remote {

std::optional<nlohmann::json> HandleMotionRpc(ui::AppState& state,
                                              const std::string& method,
                                              const RpcParams& params);

std::span<const MethodDoc> MotionMethodDocs();

} // namespace remote
