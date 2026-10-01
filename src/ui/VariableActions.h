#pragma once

#include "cad/MeshExport.h" // MeshExport::Item
#include "core/AssemblySolve.h"
#include "core/ElementMap.h"               // ElementRef
#include "core/drawing/DrawingDimension.h" // drawing::ResolvedDim
#include "core/io/Autosaver.h"
#include "render/SketchRenderer.h" // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
class SketchFeature;
class SplitFeature;
class FilletFeature; // defined in core/features/FilletFeature.h
struct RTStillDesc;  // defined in RayTracingRenderer.h
struct Ray;          // defined in FaceSelector.h

// Project-wide variables, and the global undo router.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Re-evaluate the project table into state.projectSymbols and regenerate every
// part against it (wiring each slot's Document to the project symbols first).
// Call after any project-var edit or structural change (open/new/add part).
void SyncProjectScope(AppState& state);

// Project-variable edits, undoable across tabs (projectVarHistory + regen-all).
// Set returns false only for an invalid name. Remove is refused (returns false,
// fills outRefs) while any part references it. Rename rewrites references in
// every part; false if newName is invalid/taken or oldName is absent.
bool SetProjectVariable(AppState& state, const std::string& name,
                        const std::string& expression,
                        const std::string& comment);

bool RemoveProjectVariable(AppState& state, const std::string& name,
                           std::vector<std::string>* outRefs);

bool RenameProjectVariable(AppState& state, const std::string& oldName,
                           const std::string& newName);

// Human-readable references to a project variable across ALL parts (+ project
// params), for delete-refusal messages.
std::vector<std::string> ProjectVarReferences(AppState& state,
                                              const std::string& name);

const Parameter* FindProjectParameter(const AppState& state,
                                      const std::string& name);

// Project tables (docs/variables/README.md#tables), on the same cross-tab
// undo as the variables. Set adds or replaces the table of that name and
// returns "" -- or the refusal (lookuptable::Validate's) and changes
// nothing. Remove is refused while any expression reads the table, naming
// them, or when there is no such table, naming the ones there are.
[[nodiscard]] std::string SetProjectTable(AppState& state,
                                          lookuptable::Table table);
[[nodiscard]] std::string RemoveProjectTable(AppState& state,
                                             const std::string& name);
[[nodiscard]] std::vector<std::string>
ProjectTableReferences(AppState& state, const std::string& name);

bool UndoProjectVars(AppState& state);

bool RedoProjectVars(AppState& state);

// Global Undo/Redo: route Ctrl+Z / Edit>Undo to whichever source — the active
// tab's stack or the cross-tab project-var stack — was most recently mutated.
// Use these at the dispatch sites instead of the per-tab stacks directly.
bool CanGlobalUndo(const AppState& state);

bool CanGlobalRedo(const AppState& state);

void GlobalUndo(AppState& state);

void GlobalRedo(AppState& state);

} // namespace ui
