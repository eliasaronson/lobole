#pragma once

#include "core/Document.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <span>
#include <string>

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
//                            "after_earlier_failure"?}, ...],
//    "warnings": [{"index", "name", "warning"}, ...]}
// `ok` reflects errors only — a warning means the feature built a valid
// result that is likely not what was intended (the silent no-op cut), and
// scripts should surface it without treating the edit as failed.
// Computed from the features' transient error state — regeneration has always
// completed synchronously by the time a handler builds its response.
// V2 digest: per-body kernel numbers (volume, bbox, disjoint-solid count)
// attached to every geometry-mutating response — the "measure" rung of the
// execute -> measure -> render loop, without a follow-up RPC. Bodies key on
// created_by (stable across regeneration) so DigestDeltaJson can say what an
// edit actually did: net volume change, bodies added, bodies removed. A
// zero-delta add/cut or a body that silently split into two lumps is visible
// in the same call that caused it.
inline nlohmann::json BodyDigestJson(const Document& doc) {
  nlohmann::json arr = nlohmann::json::array();
  for (const Body& b : doc.GetBodies()) {
    glm::vec3 mn(0.0f), mx(0.0f);
    b.shape.GetBoundingBox(mn, mx);
    arr.push_back({{"name", b.name},
                   {"created_by", b.createdBy},
                   {"volume", b.shape.Volume()},
                   {"bbox_min", Vec3ToJson(mn)},
                   {"bbox_max", Vec3ToJson(mx)},
                   {"solids", b.shape.SolidCount()}});
  }
  return arr;
}

inline nlohmann::json DigestDeltaJson(const nlohmann::json& before,
                                      const nlohmann::json& after) {
  auto total = [](const nlohmann::json& bodies) {
    double v = 0.0;
    for (const auto& b : bodies)
      v += b.at("volume").get<double>();
    return v;
  };
  auto names = [](const nlohmann::json& bodies) {
    std::map<uint64_t, std::string> out;
    for (const auto& b : bodies)
      out[b.at("created_by").get<uint64_t>()] = b.at("name").get<std::string>();
    return out;
  };
  const auto pre = names(before), post = names(after);
  nlohmann::json added = nlohmann::json::array(),
                 removed = nlohmann::json::array();
  for (const auto& [id, name] : post)
    if (pre.find(id) == pre.end())
      added.push_back(name);
  for (const auto& [id, name] : pre)
    if (post.find(id) == post.end())
      removed.push_back(name);
  return {{"volume", total(after) - total(before)},
          {"added", std::move(added)},
          {"removed", std::move(removed)}};
}

// Digest + delta in the shape mutating handlers attach: capture
// BodyDigestJson(doc) BEFORE the edit, pass it here after.
inline nlohmann::json DigestJson(const nlohmann::json& before,
                                 const Document& doc) {
  nlohmann::json bodies = BodyDigestJson(doc);
  nlohmann::json delta = DigestDeltaJson(before, bodies);
  return {{"bodies", std::move(bodies)}, {"delta", std::move(delta)}};
}

inline nlohmann::json RegenSummaryJson(const Document& doc) {
  nlohmann::json failed = nlohmann::json::array();
  nlohmann::json warnings = nlohmann::json::array();
  const auto& features = doc.GetFeatures();
  for (size_t i = 0; i < features.size(); i++) {
    const Feature* f = features[i].get();
    if (!f->GetWarning().empty()) {
      warnings.push_back(
          {{"index", i}, {"name", f->GetName()}, {"warning", f->GetWarning()}});
    }
    if (f->GetError().empty())
      continue;
    nlohmann::json entry = {
        {"index", i}, {"name", f->GetName()}, {"error", f->GetError()}};
    if (f->ErrorAfterEarlierFailure())
      entry["after_earlier_failure"] = true;
    failed.push_back(std::move(entry));
  }
  return {{"ok", failed.empty()},
          {"failed", std::move(failed)},
          {"warnings", std::move(warnings)}};
}

} // namespace remote
