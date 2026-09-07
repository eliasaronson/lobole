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

// Opening, saving, autosaving and recovering a project.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture-review-2026-09-06.md, stage 4.

namespace ui {

// Save/open a project directory. A multi-part project writes a project.json
// index + one `parts/<name>/` directory each; a single-part project stays
// the flat `.dsgn` (index.json + features/) form for backward compatibility.
// Open detects project.json to load all parts, else loads one part.
// Save the project. Refuses when the project on disk changed since we last
// read or wrote it (F8) — the write would replace someone else's work with
// no way back — unless `overwriteExternalChanges` says to do it anyway.
// Every save funnels through here: Ctrl+S, Save As, naming a version, and
// the save_project RPC, so this is the one guard that covers them all.
bool SaveProject(AppState& state, const std::string& path, std::string* error,
                 bool overwriteExternalChanges = false);

// Open `path` and lay every autosaved document over it (F10). The project
// itself comes from disk; each document with a sidecar newer than its saved
// form is replaced by the sidecar. All-or-nothing on purpose: a project half
// from one session and half from another is a state nobody can reason about.
//
// Leaves the project DIRTY — the recovered work has not been saved, and
// pretending otherwise would invite closing without a prompt.
bool RecoverProject(AppState& state, const std::string& path,
                    std::string* error = nullptr);

// What the GEOMETRY did between a version and now, for one part document
// (F11 layer 3). The recipe as it was is extracted from the revision,
// regenerated, and boolean-diffed against the part in front of you.
//
// This is where compare_geometry's machinery belongs and a merge's does not:
// a diff has two operands by construction. `Measure::CompareBodySets` does
// the booleans; this only finds the other operand.
//
// Also MATERIALIZES the result (SetGeometryDelta): the honest answer to
// "what changed?" is a shape, not a number, so the diff solids go straight
// on the model — red where material went away, green where it arrived.
GeometryChange CompareVersionGeometry(AppState& state,
                                      const std::string& revision,
                                      const std::string& documentDir);

// Show the user a changed item: switch to the document that holds it and
// select it, so the feature tree highlights the row (F11 G2 follow-on).
// `frame` also fits the camera to it, which is the double-click gesture —
// a single click should not move the view under someone reading a list.
//
// Returns false when there is nothing to show: the item was REMOVED by the
// change, or its document is gone. Saying so beats a click that silently
// does nothing.
bool RevealItem(AppState& state, const std::string& document,
                const std::string& documentKind, uint64_t itemId, bool frame);

// Autosave every DIRTY document of the project, one atomic file each (F10).
//
// Lives here rather than in Autosaver because only this layer knows what
// documents a project has. That is the bug it fixes: autosave was handed
// `state.document` — the ACTIVE TAB — so a multi-document project autosaved
// one part, and an assembly or drawing tab (where that pointer is null)
// autosaved nothing at all, silently.
//
// A project that IS one document keeps the single-file sidecar unchanged.
// Returns the number of documents written.
int AutosaveProject(AppState& state, Autosaver& autosaver, double now);

// Give the project a remote (F9 C2), and stop it committing built geometry
// (F9 C4) — for one person the baked bodies make opening a load instead of a
// rebuild, but on a shared project they are megabytes of binary pushed on
// every save, conflicting on every concurrent edit, and discarded either way
// because the chain hash decides at load. Saves and commits that change, so
// the remote's first push does not carry them.
bool AddProjectRemote(AppState& state, const std::string& name,
                      const std::string& url, std::string* error = nullptr);

// Record what the project directory at `path` looks like NOW, as the thing
// later saves compare against. Called wherever the app and the disk are known
// to agree: after LOADING one (we just read it), and after saving one
// (including the commit that save makes).
void StampDiskState(AppState& state, const std::string& path);

// Has someone else written to the project since StampDiskState? Walks the
// directory, so it is a save-time / poll-time question, not a per-frame one.
// False when there is no stamp to compare (a project never saved, or a Save
// As to a path we have not seen).
bool ProjectChangedOnDisk(AppState& state);

bool OpenProject(AppState& state, const std::string& path, std::string* error);

// Track the on-disk identity for dirty-state + autosave: call after every
// clean save (also clears the autosave sidecar) / open / new. "" = unsaved.
void MarkProjectSaved(AppState& state, const std::string& path,
                      const std::string& message = "save");

void MarkProjectOpened(AppState& state, const std::string& path);

// After loading a recovery sidecar: the document belongs at `path` but is
// deliberately dirty (unsaved recovered changes), so autosave keeps running
// and a save prompt is meaningful.
void MarkProjectRecovered(AppState& state, const std::string& path);

// True when any document in the project (every tab, not just the active one)
// has edits since its last clean save/open.
bool ProjectDirty(AppState& state);

// Window title: "<project name>[*] - Designer", * = ProjectDirty. The name is
// the project path's stem; "Untitled" before the first save.
std::string WindowTitle(AppState& state);

// Applies (and removes) the transient view rollback that "edit sketch" wants:
// the model stops just before the first feature built FROM the open sketch,
// so the solid the sketch produced is not sitting in front of the sketch —
// the rollback SolidWorks and Fusion do on Edit Sketch. Reconciles against
// sketchEdit.activeSketchId, so the seventeen places that enter or leave a
// sketch session (including teardown paths like a tab switch or an undo that
// removed the sketch) do not each have to remember to put the marker back.
// Call once per frame, BEFORE SyncDocumentRenderers — it can regenerate, and
// the projection that follows must see the bodies it settled on.
void SyncSketchViewRollback(AppState& state);

} // namespace ui
