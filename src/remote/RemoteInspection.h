#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// Inspection RPC handlers (section clip, interference, mass_properties, measure,
// list_elements) carved out of RemoteServer's HandleSemantic. Returns the
// result json if `method` is one of them, std::nullopt otherwise.
namespace remote {

std::optional<nlohmann::json> HandleInspectionRpc(ui::AppState& state,
                                                  Document& doc,
                                                  const std::string& method,
                                                  const nlohmann::json& params);

}  // namespace remote
