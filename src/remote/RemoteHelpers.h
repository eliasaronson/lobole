#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// Small helpers shared by the RemoteServer RPC domain modules (RemoteModeling,
// RemoteInspection, RemoteView, ...). Each module is
// `remote::HandleXxxRpc(AppState&, [Document&], method, params, out&) -> bool`,
// carved out of RemoteServer::HandleSemantic (the RemoteConfigurations
// pattern).
namespace remote {

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}

} // namespace remote
