#pragma once

// The few helpers that more than one RemoteServer translation unit needs.
// Not a public interface: only src/remote includes this.
//
// RemoteServer.cpp was 2,947 lines and three subjects -- the socket, the
// dispatch, and the GUI-action queue. Splitting it (stage 8a of
// docs/architecture-review-2026-09-06.md) left exactly two helpers used by
// two of them, plus two the session owns and the dispatch reports in `ping`.

#include "remote/RemoteServer.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <set>
#include <string>
#include <vector>

// The mode this server was started in.
const char* ModeName(RemoteMode mode);

// Whether the binary on disk is still the one this process is running. A
// rebuild REPLACES the file; a process already running keeps the old inode,
// serves happily, and answers questions about a build nobody can look at any
// more -- which cost this project two diagnosis detours in one day.
bool BinaryReplaced();

// "unknown parameter 'x' for method m; did you mean 'y'?" -- the suggestion is
// an edit distance over the method's accepted keys, which is what makes a typo
// fixable from the message alone.
std::string UnknownParamError(const std::string& method,
                              const std::vector<std::string>& unknown,
                              const std::set<std::string>& accepted);

// A parameter of the wrong TYPE, said the way a refusal says things: which
// method, what each key it was handed actually is, and where to read what
// each one takes. nlohmann's own message ("type must be string, but is
// boolean") names none of those, so a caller who guessed `ssao: true` for a
// parameter that takes off|half|full learns nothing from it.
std::string TypeErrorMessage(const std::string& method,
                             const nlohmann::json& params,
                             const std::set<std::string>& seen,
                             const std::string& what);

// Points the camera at a named standard view. The semantic `view` RPC and the
// GUI-action path both need it, and a second copy would let the two disagree
// about what "front" means.
bool ApplyStandardView(ui::AppState& state, const std::string& name, bool fit,
                       const glm::vec3* boxMin = nullptr,
                       const glm::vec3* boxMax = nullptr);

// The one live server, owned by RemoteSession.cpp. The reusable test the
// engine runs takes no argument, so the queued action is reached through it.
extern RemoteServer* g_RemoteServerInstance;
