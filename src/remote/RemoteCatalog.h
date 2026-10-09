// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
