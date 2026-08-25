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
  const char* doc;  // one-line description (prose: what it does, and why)
  // The accepted TOP-LEVEL parameter keys, comma-separated, "" for none, or
  // "*" when the object is a free-form spec the handler reads opaquely
  // (`p.raw()`). This is what `describe` hands an agent so it does not have
  // to guess a key name, and it is the half `RpcParams` cannot supply:
  // SeenKeys() knows the exact vocabulary but only by RUNNING the handler.
  //
  // Kept true by `tests/rpc_params_gate.py`, which reads the handlers with
  // tools/rpc_params_scan.py and fails when a row and its branch disagree in
  // either direction — so a new key without a row, or a row naming a key
  // nobody reads, is a failing test rather than a stale line.
  const char* params;
};

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}

// Decision-complete identity errors (error-audit round): the first
// rejection carries the measured state and the known vocabulary, so the
// correction needs no discovery round-trip.
inline std::string KnownList(const std::vector<std::string>& known,
                             size_t cap = 10) {
  if (known.empty())
    return "none exist yet";
  std::string out;
  const size_t shown = std::min(known.size(), cap);
  for (size_t i = 0; i < shown; i++)
    out += (i ? ", " : "") + known[i];
  if (known.size() > shown)
    out += ", +" + std::to_string(known.size() - shown) + " more";
  return out;
}

inline std::string IndexError(const char* what, long long given, size_t count,
                              const char* discovery) {
  std::string msg = std::string(what) + " index " + std::to_string(given) +
                    " out of range — there " + (count == 1 ? "is " : "are ") +
                    std::to_string(count) + (count == 1 ? " " : " ") + what +
                    (count == 1 ? "" : "s");
  if (count > 0)
    msg += " (0.." + std::to_string(count - 1) + ")";
  return msg + "; " + discovery;
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

// Attach the regeneration summary to a response. The `doc` guard is why this
// is a helper rather than a bare assignment: an assembly or drawing tab has no
// active PART document, so a handler reachable from those tabs must not
// dereference one — and the handlers that forgot the guard were not the
// problem, the ones that forgot the whole block were.
//
// The gap this closes: `undo` and `set_active_configuration` used to return a
// success with a plausible body count and volume while a feature had actually
// failed — for a 20 mm box whose fillet broke, the reported volume was exactly
// 20^3, i.e. the UNFILLETED box. Nothing in the response said so, so a script
// built its next edit on a broken model. `regen.ok` is now in the response
// that caused the failure, which is the contract this header already
// described.
inline void AttachRegen(nlohmann::json& out, const Document* doc);

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

inline void AttachRegen(nlohmann::json& out, const Document* doc) {
  if (doc != nullptr)
    out["regen"] = RegenSummaryJson(*doc);
}

} // namespace remote
