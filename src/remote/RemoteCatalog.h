#pragma once
#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <optional>
namespace ui {
struct AppState;
}
namespace remote {
std::optional<nlohmann::json> HandleCatalogRpc(ui::AppState& state,
                                               const std::string& method,
                                               const RpcParams& params);
std::span<const MethodDoc> CatalogMethodDocs();
} // namespace remote
