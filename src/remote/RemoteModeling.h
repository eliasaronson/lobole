// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Document;
namespace ui {
struct AppState;
}
namespace heavy {
struct Request;
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

class Parking;
// An align_import best_fit waits instead of fitting on the frame
// (docs/interchange/README.md#laying-a-scan-onto-a-body): the handler reads
// and checks every input and gathers the triangles here, the fit runs on an
// `align` job with progress and a cancel, and the request is answered --
// the placement committed as one undo step -- when it lands. nullptr:
// dispatch it now (another mode, or a request the handler refuses).
std::unique_ptr<Parking> ParkModelingRequest(ui::AppState& state,
                                             const std::string& method,
                                             const RpcParams& params);

// Sketch constraint kind names accepted by add_constraint(s), for describe.
std::vector<std::string> ConstraintKindNames();

// What `value` MEANS for each dimensional kind, for describe. A bare name
// list said `angle` existed and not one thing about the number beside it,
// which is how the same field came to be degrees through add_constraint and
// radians through add_feature. Kinds that take no value are omitted, so the
// map doubles as "which kinds are dimensions".
std::vector<std::pair<std::string, std::string>> ConstraintValueUnits();

// A heavy direct edit is tried off the frame before it is committed, so a
// refusal changes nothing, not even the revision
// (docs/scripting/rpc.md#a-refused-call-changes-nothing). When a top-level
// add_feature or modify_feature reaches one whose build no store holds, the
// handler throws this with the build to run, before any edit; the server
// parks the call on that build (ui::StartTrialBuild) and dispatches it
// again once it is done, when the trial answers from the store. Only while
// a HeavyTrialScope says the caller can park: inside a batch, or on the
// second dispatch, the edit is committed and built as before.
struct HeavyTrialNeeded {
  std::shared_ptr<heavy::Request> request;
};

class HeavyTrialScope {
public:
  explicit HeavyTrialScope(bool allowed);
  ~HeavyTrialScope();
  HeavyTrialScope(const HeavyTrialScope&) = delete;
  HeavyTrialScope& operator=(const HeavyTrialScope&) = delete;
  static bool Allowed();

private:
  bool m_was;
};

} // namespace remote
