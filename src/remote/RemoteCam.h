#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

class Document;
namespace ui {
struct AppState;
}

// CAM RPC handlers (docs/cam/README.md): setups, tools, operations, toolpath
// generation and posting. Returns the result json if `method` is one of them,
// std::nullopt otherwise.
//
// Deliberately NOT here: anything that runs a machine. This interface writes
// a file and answers questions about it; sending it to a controller is the
// user's decision, made in their sender, after they have looked at it.
namespace remote {

std::optional<nlohmann::json> HandleCamRpc(ui::AppState& state, Document* doc,
                                           const std::string& method,
                                           const RpcParams& params);

// One row per RPC this module dispatches (for the describe endpoint).
std::span<const MethodDoc> CamMethodDocs();

// A CAM request that WAITS instead of computing on the frame
// (docs/cam/README.md#requests-that-wait). cam_toolpath, cam_post,
// cam_verify and cam_simulate read toolpaths whose passes can take minutes;
// run inline they held the main thread for all of it. Parked, the passes cut
// on the "cam" worker (and cam_verify's simulation on a "cam-verify" one)
// while the frame and every other client's requests carry on, and the
// server answers the request when what it waits on has landed -- with the
// response the synchronous handler gives, because it IS that handler (or,
// for cam_verify, the same three steps it runs).
class CamParking {
public:
  virtual ~CamParking() = default;
  enum class Next {
    Wait,     // still waiting; ask again next tick
    Dispatch, // everything is cached: run the ordinary handler now
    Answer,   // `answer` is the response
  };
  // One tick, on the main thread. A refusal (a cancelled pass, a failed
  // verification) is thrown, the way a handler throws one.
  virtual Next Poll(ui::AppState& state, const RpcParams& params,
                    nlohmann::json& answer) = 0;
};

// Parks `method` when answering it now would compute on the frame, and
// returns what to poll; nullptr means "dispatch it now" -- not a CAM request
// that waits, or one whose passes are all cached, or one the handler will
// refuse (the refusal is the handler's, so it stays word for word).
std::unique_ptr<CamParking> ParkCamRequest(ui::AppState& state,
                                           const std::string& method,
                                           const RpcParams& params);

} // namespace remote
