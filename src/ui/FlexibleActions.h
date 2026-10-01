#pragma once
// Flexible sub-assemblies in the GUI (v129,
// docs/assemblies/README.md#flexible-sub-assemblies): the panel and the RPC
// make a placement flexible, choose its configuration, name its inner mates
// and select its moving parts through these, so the two cannot drift apart.
#include "core/AssemblySolve.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui {

// Why `occ` cannot be made FLEXIBLE in the active assembly, or empty: the
// unit's own refusals (assembly::FlexibleRefusal), and one more only the
// project can see -- the active assembly is itself placed flexibly somewhere,
// and only one level moves.
std::string FlexibleRefusalHere(AppState& state, const Occurrence& occ);

// Make a placed sub-assembly flexible or rigid: one undo step, then a solve.
// Refused (false, `why` filled, nothing changed and no undo step) when it
// cannot move. Rigid again drops the poses it kept. `report` receives the
// solve's report when given.
[[nodiscard]] bool
SetOccurrenceFlexible(AppState& state, uint64_t occId, bool flexible,
                      std::string* why,
                      assembly::SolveReport* report = nullptr);

// Which configuration of what it places a placement uses (0 = that
// document's active one): one undo step, re-solved when it places a part
// (its faces, which its mates anchor on, change) or is flexible (its
// mechanism runs under it). Refused for an id that document does not have,
// and for a part configuration the part does not build in, saying what
// fails. `solved` says whether it solved.
[[nodiscard]] bool SetOccurrenceConfiguration(
    AppState& state, uint64_t occId, uint64_t configurationId, std::string* why,
    assembly::SolveReport* report = nullptr, bool* solved = nullptr);

// What a placement of a part is made of: a material slug, or "" for its
// part's own (v135). One undo step. Refused for a sub-assembly placement
// and for a slug this build does not know, naming the known ones.
[[nodiscard]] bool SetOccurrenceMaterial(AppState& state, uint64_t occId,
                                         const std::string& material,
                                         std::string* why);

// A mate that turns or slides, as a study driver or a relation side names it:
// {path, mate} -- this document's own (empty path) or one inside a flexible
// placement (path = [placement], mate = the sub-assembly's id).
struct DofMateChoice {
  std::vector<uint64_t> path;
  uint64_t mate = 0;
  std::string label; // "crank" or "Pump / camshaft"
  bool rotation = false;
  bool offset = false;
};
// Every such mate in the active assembly: its own first, then each flexible
// placement's, in document order.
std::vector<DofMateChoice> DofMateChoices(AppState& state);
// The label a {path, mate} reads as ("Pump / camshaft"), "(missing)" when it
// names nothing.
std::string MateLabelAt(AppState& state, const std::vector<uint64_t>& path,
                        uint64_t mate);

// A connector's world frame for a mate named by path: an inner mate's
// connector is in its sub-assembly's space, placed by the flexible
// placement's own pose of the occurrence it is on.
std::optional<glm::dmat4>
MateConnectorWorldAt(AppState& state, const std::vector<uint64_t>& path,
                     const Connector& c);

// The sub-assembly a FLEXIBLE placement runs, or null (rigid, a part, not
// loaded).
const AssemblyDocument* FlexibleSub(AppState& state,
                                    const Occurrence& placement);
// Its occurrences that MOVE (the ones the placement keeps a pose for), in the
// sub-assembly's order.
std::vector<const Occurrence*>
MovingInnerOccurrences(AppState& state, const Occurrence& placement);
// Whether `inner` is one of them.
[[nodiscard]] bool IsMovingInner(AppState& state, const Occurrence& placement,
                                 uint64_t inner);
// Its mates that turn or slide (the ones the tree lists).
std::vector<const Mate*> InnerDofMates(AppState& state,
                                       const Occurrence& placement);
// The sub-assembly mate whose placed (b) side is `inner`: the one a drag of
// that part moves. null when none, or it has no degree of freedom.
const Mate* InnerMateDriving(AppState& state, const Occurrence& placement,
                             uint64_t inner);

// Selecting a MOVING PART of a flexible placement: an Occurrence item whose
// id is the placement and whose innerPath is {inner}. It replaces the
// occurrence selection as SelectOccurrence does. SelectedOccurrenceId(s)
// skip it, so Delete, Hide and the unit's gizmo never act on the whole
// placement for a click on one of its parts.
void SelectInnerOccurrence(AppState& state, uint64_t placement, uint64_t inner);
// {placement, inner} of the selected moving part, or {0, 0}.
std::pair<uint64_t, uint64_t> SelectedInnerOccurrence(const AppState& state);
[[nodiscard]] bool IsInnerOccurrenceSelected(const AppState& state,
                                             uint64_t placement,
                                             uint64_t inner);

// Where a moving inner occurrence is DRAWN: the placement's display matrix
// (explode, playback) times its pose in the placement, displaced by its own
// explode step.
glm::dmat4 InnerDisplayMatrix(AppState& state, const Occurrence& placement,
                              const Occurrence& inner);

} // namespace ui
