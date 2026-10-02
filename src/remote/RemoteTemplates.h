#pragma once

// Feature templates over the RPC (docs/modeling/templates.md#rpcs): the
// project's library -- save, list, delete, export, import -- and the verbs
// on an instance in the active part -- update and unlink. Placing one is
// add_feature {type: "Template"} (RemoteModeling.cpp, AddTemplateFeature).

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>

namespace ui {
struct AppState;
}
class Document;

namespace remote {

std::optional<nlohmann::json> HandleTemplateRpc(ui::AppState& state,
                                                const std::string& method,
                                                const RpcParams& params);

// add_feature's Template branch: the request names a template of the
// library and gives every value; refused naming what is accepted, and an
// instance that does not build is taken back out and its error refused.
nlohmann::json AddTemplateFeature(ui::AppState& state, Document& doc,
                                  const nlohmann::json& feature);

std::span<const MethodDoc> TemplateMethodDocs();

} // namespace remote
