#pragma once

#include <cstdint>

#include "core/Document.h"
#include "core/Receipt.h"
#include "remote/RpcParams.h"

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
namespace ui {
struct AppState;
}

namespace remote {

// Resolve an optional `part` RPC param — a slot NAME, or a stable PartSlot id
// as a number or a numeric string, falling back to the slot index — to that
// part's document, WITHOUT switching the active tab. Throws naming what exists
// when nothing matches.
//
// The app holds exactly one live document: a PartSlot's `doc` is moved into
// AppState on activation and moved back when the tab leaves, so "the active
// part" is real in the document layer and most RPCs are gated on it. But a
// READ does not need it — `ui::PartDocById` reaches any slot whether it is
// active or stashed — and activating to read is not free: a tab switch clears
// the selection, the hidden-body set, the measure session, simulation mode and
// the explode state (ui::ResetActiveTabUiState). An agent reading a part from
// the human's running instance would silently wipe all of it.
//
// So every read-only RPC that can take `part` should
// (docs/scripting/rpc.md#reads-name-their-part).
[[nodiscard]] Document* ResolvePartParam(ui::AppState& state,
                                         const nlohmann::json& part);

// WHICH part a read-only question is about: the one it NAMES, or the tab you
// are on. A question that names its part has no business caring what tab is
// active — a bend audit over sixteen parts ran from an assembly and was
// refused sixteen times, so it walked the tabs to ask. Throws, naming the
// ways to say which, when there is neither a name nor an active part.
[[nodiscard]] const Document& SubjectPart(ui::AppState& state, Document* active,
                                          const RpcParams& params);

// A part's revision: {session, part_id, instance, version} -- this server
// process, the part's stable slot id, the document instance (a reopen or a
// replacement is a new one) and its regeneration count. What
// geometry_snapshot binds its references to, get_state reports, and a
// guarded write's `expect_revision` is compared against
// (docs/scripting/rpc.md#the-revision-guard).
[[nodiscard]] nlohmann::json PartRevision(const Document& part,
                                          uint64_t partId);

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
  // So it is also what the dispatcher refuses an unknown key against,
  // BEFORE the handler runs (UndeclaredParamsError,
  // docs/scripting/rpc.md#a-refused-call-changes-nothing): a key it does not
  // list is refused while refusing still means nothing happened.
  //
  // Kept true by `tests/rpc_params_gate.py`, which reads the handlers with
  // tools/rpc_params_scan.py and fails when a row and its branch disagree in
  // either direction — so a new key without a row, or a row naming a key
  // nobody reads, is a failing test rather than a stale line.
  //
  // A row may TYPE its keys, and is then a contract
  // (docs/scripting/rpc.md#typed-contracts): `name?:type` per key, `?` for
  // optional, types int/number/bool/string/object/null/any/revision, `T[]`
  // or `T[N]`, unions with `|`, literals 'x', a describe vocabulary @name,
  // ranges int>=0 / number>0. The dispatcher validates such a request before
  // the handler runs, describe publishes it as `request_schema`, and
  // tools/generate_rpc_contracts.py types the SDK from it
  // (remote/RpcContract.h). A typed row types every key.
  const char* params;

  // What the DISPATCHER needs to know about this method, in the same row as
  // the method itself.
  //
  // Before this, a name was a key into four separate sets -- kNoPartDoc (166
  // entries), kNotInBatch (31), kDeferrable (10) and IsUiAction -- none of them
  // next to a handler, so adding a method meant remembering four places that
  // no test compared. That is the shape of every gate-list bug this project
  // has already paid for (docs/architecture.md#the-remote-json-rpc).
  //
  // A flag is stated only where it is NOT the default, so most rows are
  // unchanged: needing a part tab is the minority (41 of 230), and reaching
  // outside the document rarer still.
  uint8_t flags = 0;

  // The top-level keys of the RESPONSE, in the same comma-separated shape
  // `params` uses, with nesting shown where a caller would otherwise reach
  // for the wrong level: `total{mass_g,volume}`, `bodies[]{name,volume}`.
  //
  // Every refusal this project makes is good: a wrong key is named, a
  // missing field is named, an unknown enum lists the accepted set. That is
  // the parameter contract, and `rpc_params_gate` keeps it true. The RESULT
  // contract had nowhere to live, so it lived in prose when it lived at all
  // — and a caller that guesses wrong does not get a refusal, it gets a
  // KeyError three calls later. `mass_properties` reads "per body and
  // total", which makes the total look like `r["mass_g"]` when it is
  // `r["total"]["mass_g"]`; `list_elements` nests under `bodies` even when
  // one body was asked for by index; `list_parts` returns drawings too.
  //
  // REQUIRED for every list_*, get_* and check_* method — the family an
  // agent PARSES rather than merely calls — and gated by
  // `tests/rpc_returns_gate.py`. A mutating verb's answer is its `regen`
  // summary and its row says so already, so most rows leave this empty.
  const char* returns = "";
};

// The columns above, as a bitmask.
enum MethodFlag : uint8_t {
  // No flag applies. Spelled rather than written as a bare 0, because a row
  // that carries `returns` has to say something in the flags column.
  NoFlags = 0,
  // Refuses unless the active tab is a PART. Most methods do not care -- they
  // act on the project, an assembly, a drawing or a job -- so this is stated
  // rather than assumed.
  NeedsPart = 1u << 0,
  // Reaches outside the document (a file, a git tree, the window, the tab), so
  // a batch cannot roll it back: undoing the document would leave the world
  // half-changed and the response would be a lie about it.
  NotInBatch = 1u << 1,
  // Its RESULT does not depend on freshly built geometry, so a batch can defer
  // the regeneration past it. Everything else flushes first, which is what
  // keeps a measure or a list_elements inside a batch honest.
  Deferrable = 1u << 2,
  // Goes on the ImGui queue instead of running here: ImGui state is only
  // coherent inside the frame.
  UiAction = 1u << 3,
};

// A receipt on the wire. Attached by every RPC that returns a VERDICT, so a
// caller cannot mistake "could not check" for "clean" (core/Receipt.h).
//
// `standing` is the roll-up and `summary` says it in words, because a caller
// who reads neither the claims nor the flags still sees UNVERIFIED in the one
// field they do read.
inline nlohmann::json ReceiptJson(const receipt::Receipt& r) {
  nlohmann::json claims = nlohmann::json::array();
  for (const receipt::Claim& c : r.Claims()) {
    nlohmann::json j = {
        {"claim", c.name},
        {"verdict", receipt::ToString(c.verdict)},
        {"basis", receipt::ToString(c.basis)},
        {"oracle", {{"id", c.oracle.id}, {"version", c.oracle.version}}}};
    if (c.value)
      j["value"] = *c.value;
    if (!c.unit.empty())
      j["unit"] = c.unit;
    if (!c.note.empty())
      j["note"] = c.note;
    claims.push_back(std::move(j));
  }
  return {{"schema", receipt::kSchema},
          {"standing", receipt::ToString(r.Roll())},
          {"summary", receipt::Summary(r)},
          {"claims", std::move(claims)}};
}

inline void AttachReceipt(nlohmann::json& out, const receipt::Receipt& r) {
  out["receipt"] = ReceiptJson(r);
}

inline nlohmann::json Vec3ToJson(const glm::dvec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}

// WHERE an overlap is, in the one vocabulary every clash report speaks —
// `interference` (named pair and sweep), `contact_report` and
// `run_motion_study`: the intersection solid's volume-weighted centre and
// its world bounds, in millimetres. A pair of names is a guessing game on an
// assembly carrying eight of each; the standing sweep on the PES pump took
// both occurrences' boxes to find one.
inline void AddOverlapSite(nlohmann::json& j, const glm::dvec3& centroid,
                           const glm::dvec3& lo, const glm::dvec3& hi) {
  j["centroid_mm"] = {centroid.x, centroid.y, centroid.z};
  j["bbox_min_mm"] = {lo.x, lo.y, lo.z};
  j["bbox_max_mm"] = {hi.x, hi.y, hi.z};
}

// All a MESH check knows of where: the region the two sides' world boxes
// share, which holds the overlap somewhere inside it. Named for what it is,
// so it cannot be read as the overlap's own bounds.
inline void AddOverlapBound(nlohmann::json& j, const glm::dvec3& lo,
                            const glm::dvec3& hi) {
  j["within_min_mm"] = {lo.x, lo.y, lo.z};
  j["within_max_mm"] = {hi.x, hi.y, hi.z};
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
    glm::dvec3 mn(0.0), mx(0.0);
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
    // A face appearance whose face is gone (v164): the feature built fine,
    // but a face the caller painted is no longer painted — said here, in the
    // response to the edit that lost it, not left for a get_state poll.
    for (const std::string& name : f->UnresolvedFaceLooks())
      warnings.push_back(
          {{"index", i},
           {"name", f->GetName()},
           {"warning", "face appearance on '" + name +
                           "' names no face of its body any more; it is "
                           "kept, unpainted — clear it with set_appearance "
                           "{feature_id, element} or repaint another face"},
           {"face_appearance", name}});
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

// The flags of a method, or 0 for one we do not know. Defined in
// RemoteDispatch.cpp, which owns the tables; RemoteUiActions.cpp asks it
// whether a method belongs on the ImGui queue.
uint8_t MethodFlagsOf(const std::string& method);

} // namespace remote
