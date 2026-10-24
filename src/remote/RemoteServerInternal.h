// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The few helpers that more than one RemoteServer translation unit needs.
// Not a public interface: only src/remote includes this.
//
// RemoteServer.cpp was 2,947 lines and three subjects -- the socket, the
// dispatch, and the GUI-action queue. Splitting it
// (docs/architecture.md#the-remote-json-rpc) left exactly two helpers used by
// two of them, plus two the session owns and the dispatch reports in `ping`.

#include "core/io/Serialization.h"
#include "remote/InspectionContracts.h"
#include "remote/RemoteServer.h"
#include "remote/RpcContract.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <optional>
#include <set>
#include <string>
#include <string_view>
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

// The refusal for params `method`'s describe row does not take -- a key it
// does not list, or params that are not an object -- or nullopt when they are
// fine (or the method is unknown, or takes a free-form spec). Checked BEFORE
// the handler runs, for every way a request arrives: a refusal that comes
// after the handler has written a file is a refusal of something already
// done (docs/scripting/rpc.md#parameters).
//
// A ContractError: `what()` is the sentence, `diagnostic` the structured
// refusal (code unknown_argument, the key, the keys the row takes).
std::optional<remote::ContractError>
UndeclaredParamsError(const std::string& method, const nlohmann::json& params);

// The request schema of `method` -- an inspection contract or a typed
// MethodDoc row (docs/scripting/rpc.md#typed-contracts) -- or nullptr.
const nlohmann::json* RequestSchemaOf(const std::string& method);

// Refuses (ContractError) a request its schema does not accept; a method
// without one passes. add_feature and modify_feature also hold the spec in
// `feature` to its type's contract.
void ValidateRequestContract(const std::string& method,
                             const nlohmann::json& params);

// A feature type's whole contract column -- its `type`, the keys every spec
// may carry (kFeatureCommonContract) and the row's own typed fields -- and
// the schema built from it, or nullptr for a type no row names
// (docs/scripting/rpc.md#typed-feature-specs).
std::string FeatureContractColumn(const DocumentIO::FeatureTypeInfo& info);
const nlohmann::json* FeatureSchemaOf(const std::string& type);

// The vocabulary a typed row names as `@name`, or nullptr.
const remote::contract::Vocabulary* ContractVocabulary(std::string_view name);

// The active part's revision ({session, part_id, instance, version}), or
// null when the active tab is not a part.
nlohmann::json ActivePartRevision(ui::AppState& state);

// Where each of the active tab's undo histories stood before a call, so a
// refused call can be taken back whole: the part's command stack, and the
// snapshot histories of an assembly, a drawing and a CAM job. A refusal
// leaves the document as it was; without this, a handler that checked one
// thing after changing another left the change behind under an error.
struct EditMarks {
  const void* document = nullptr;
  const void* commands = nullptr;
  size_t commandDepth = 0;
  const void* assembly = nullptr;
  size_t assemblyUndo = 0;
  const void* drawing = nullptr;
  size_t drawingUndo = 0;
  const void* job = nullptr;
  size_t jobUndo = 0;
  // The project scope (variables, tables, templates): one cross-tab history,
  // whichever tab is active. Its redo is kept whole, because the first edit
  // a call makes clears it and a rollback has to put it back.
  size_t projectVarUndo = 0;
  std::vector<ui::ProjectVarSnapshot> projectVarRedo;
  // The other tabs that were saved as they stood, by slot id: a project
  // edit regenerates every part, which moves each one's stamp, so the
  // rollback that restores them stamps them saved again.
  std::vector<uint64_t> savedOtherSlots;
  // The other part tabs' own undo histories: an edit that names its part
  // (set_tolerance {part}) lands on that tab's stashed stack, not the active
  // one's. A stack made since the mark (null here) rolls back to empty.
  struct StashedStack {
    uint64_t slot = 0;
    const void* document = nullptr;
    const void* commands = nullptr;
    size_t depth = 0;
  };
  std::vector<StashedStack> stashedStacks;
  // Whether the active tab was saved as it stood: a rollback restores the
  // content, and a restore moves the version on (so no cache serves the
  // state it replaced) -- which would read as an unsaved edit nobody made.
  int activePart = -1;
  uint64_t activeStamp = 0;
  uint64_t lastSaved = 0;
};
EditMarks MarkEdits(const ui::AppState& state);

// Undo everything pushed since `marks` on the tab that is still the one
// marked, and drop it from redo. Returns what was taken back, in words ("2
// edit(s) undone and the job restored"), or "" when nothing had changed.
std::string RollBackEdits(ui::AppState& state, const EditMarks& marks);

// A parameter of the wrong TYPE, said the way a refusal says things: which
// method, what each key it was handed actually is, and where to read what
// each one takes. nlohmann's own message ("type must be string, but is
// boolean") names none of those, so a caller who guessed `ssao: true` for a
// parameter that takes off|half|full learns nothing from it.
// A string anywhere in `params` holding a NUL byte, refused before any
// handler runs (remote/RpcErrors.h, FirstNulString).
std::optional<remote::ContractError>
NulStringError(const std::string& method, const nlohmann::json& params);

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
