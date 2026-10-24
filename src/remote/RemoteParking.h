// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A request that WAITS instead of computing on the frame
// (docs/architecture.md#requests-that-wait). The server asks each domain,
// before it dispatches a request, whether answering it now would hold the
// main thread on work a background job does better; a domain that says so
// hands back what to poll, the request is parked, and the frame and every
// other client's requests carry on until the poll answers it. The CAM
// requests wait on their toolpath passes (remote/RemoteCam.h), the drawing
// ones on a projection (remote/RemoteDrawings.h), an exact interference of
// two named sides on its boolean (remote/RemoteInspection.h), a synchronous
// requirements check or design sweep on its job
// (remote/RemoteRequirements.h).

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace ui {
struct AppState;
}

namespace remote {

class RpcParams;

class Parking {
public:
  virtual ~Parking() = default;
  enum class Next {
    Wait,     // still waiting; ask again next tick
    Dispatch, // everything is cached: run the ordinary handler now
    Answer,   // `answer` is the response
  };
  // One tick, on the main thread. A refusal (a cancelled pass, a failed
  // verification) is thrown, the way a handler throws one. An `Answer` must
  // have read every parameter the request carries, as a handler does: the
  // server refuses the unknown ones against what was read.
  virtual Next Poll(ui::AppState& state, const RpcParams& params,
                    nlohmann::json& answer) = 0;
};

// set_settings {gpu_device} on an instance that renders: applied, then
// answered once the frame boundary has rebuilt the device onto it (or found
// it need not). remote/RemoteDispatch.cpp.
std::unique_ptr<Parking> ParkSettingsRequest(ui::AppState& state,
                                             const std::string& method,
                                             RpcParams& params);

} // namespace remote
