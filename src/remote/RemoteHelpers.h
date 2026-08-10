#pragma once

#include "core/Document.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <span>

// Small helpers shared by the RemoteServer RPC domain modules (RemoteModeling,
// RemoteInspection, RemoteView, ...). Each module is
// `remote::HandleXxxRpc(AppState&, [Document&], method, params, out&) -> bool`,
// carved out of RemoteServer::HandleSemantic (the RemoteConfigurations
// pattern).
namespace remote {

// One entry per RPC method for the describe endpoint. Each domain module
// keeps a static table ADJACENT to its dispatch if-chain (the drift defense:
// add the branch, add the row) exposed via XxxMethodDocs().
struct MethodDoc {
  const char* name; // exact RPC method literal
  const char* doc;  // one-line description (params in prose)
};

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}

// Compact regeneration outcome, appended as "regen" to every mutating RPC's
// response (and merged into get_state's regen block) so clients learn about
// failures from the call that caused them instead of having to poll:
//   {"ok": bool, "failed": [{"index", "name", "error",
//                            "after_earlier_failure"?}, ...]}
// Computed from the features' transient error state — regeneration has always
// completed synchronously by the time a handler builds its response.
inline nlohmann::json RegenSummaryJson(const Document& doc) {
  nlohmann::json failed = nlohmann::json::array();
  const auto& features = doc.GetFeatures();
  for (size_t i = 0; i < features.size(); i++) {
    const Feature* f = features[i].get();
    if (f->GetError().empty())
      continue;
    nlohmann::json entry = {
        {"index", i}, {"name", f->GetName()}, {"error", f->GetError()}};
    if (f->ErrorAfterEarlierFailure())
      entry["after_earlier_failure"] = true;
    failed.push_back(std::move(entry));
  }
  return {{"ok", failed.empty()}, {"failed", std::move(failed)}};
}

} // namespace remote
