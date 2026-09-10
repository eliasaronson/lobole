#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

class Document;
namespace ui {
struct AppState;
}

// Modeling / feature-tree RPC handlers (select_feature..list_variables) carved
// out of RemoteServer's HandleSemantic if-chain. Returns the result json if
// `method` is one of them, std::nullopt otherwise. All act on the active part
// document (caller guarantees non-null).
namespace remote {

std::optional<nlohmann::json> HandleModelingRpc(ui::AppState& state,
                                                Document& doc,
                                                const std::string& method,
                                                const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> ModelingMethodDocs();

// Sketch constraint kind names accepted by add_constraint(s), for describe.
std::vector<std::string> ConstraintKindNames();

// What `value` MEANS for each dimensional kind, for describe. A bare name
// list said `angle` existed and not one thing about the number beside it,
// which is how the same field came to be degrees through add_constraint and
// radians through add_feature. Kinds that take no value are omitted, so the
// map doubles as "which kinds are dimensions".
std::vector<std::pair<std::string, std::string>> ConstraintValueUnits();

} // namespace remote
