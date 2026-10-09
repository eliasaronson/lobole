// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

// The tabs -- which document is live, and adding, duplicating, deleting
// and foldering them -- plus the per-tab undo for the kinds that keep
// their own history.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Swaps in a new document (File > New / Open) with a fresh undo stack and
// forces the renderers to resync.
void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc);

// Multi-document projects (assemblies A1/A2). EnsureParts makes the parts list
// non-empty (one slot for the active document) — call once after the initial
// document is created. AddPart/AddAssembly append a new empty tab of that kind
// and switch to it. SetActivePart swaps the active document out to its slot
// and the target in. DeletePart removes a tab (never the last). All resync the
// renderers.
void EnsureParts(AppState& state, const std::string& firstName = "Part 1");

size_t AddPart(AppState& state, const std::string& name);

size_t AddAssembly(AppState& state, const std::string& name);

size_t AddDrawing(AppState& state, const std::string& name);

size_t AddJob(AppState& state, const std::string& name);

// The bodies the VIEWPORT is showing: a part tab's document bodies, or a job
// tab's parts laid out. Anything that turns a picked mesh back into geometry
// has to ask this rather than `state.document`, which is null on a job tab —
// asking the document there returned nothing, so a click on a job selected
// nothing at all and said nothing about it.
const std::vector<Body>& ViewportBodies(AppState& state);

// Appends a copy of the tab at `index` — same kind, same contents, a new
// stable id and a name nothing else has taken ("Part 1 copy") — and switches
// to it. Returns the new tab's index, or SIZE_MAX if `index` names no tab.
//
// The copy is independent: a part's features are cloned and regenerated, and
// its undo history is NOT copied (that history describes the original). What
// the copy still POINTS at is unchanged — a copied assembly places the same
// parts, a copied drawing draws the same source document — and nothing
// pointing at the original is repointed at the copy.
size_t DuplicateDocument(AppState& state, size_t index);

void SetActivePart(AppState& state, size_t index);

// Closes a tab. Refused (with the reason in `why`) for the last tab, or
// while any assembly still places this part or assembly — a dangling
// reference is not something to create on purpose.
bool DeletePart(AppState& state, size_t index, std::string* why = nullptr);

// Why that tab cannot be deleted, or empty when it can. The menu asks BEFORE
// offering, so a refusal is a disabled item with a reason on it rather than a
// live one that does nothing.
[[nodiscard]] std::string WhyPartUndeletable(AppState& state, size_t index);

// Tab-bar folders (display grouping only — storage stays flat, ids and
// references are unaffected). Folders live in state.tabFolders and may be
// empty; CreateFolder registers a name, MoveToFolder reassigns one tab
// ("" = top level, registering the folder as needed), RenameFolder renames
// the registry entry and every member (renaming onto an existing folder
// merges them), DissolveFolder unregisters and moves members to the top
// level. All mark the project meta-dirty.
void CreateFolder(AppState& state, const std::string& folder);

void MoveToFolder(AppState& state, size_t index, const std::string& folder);

void RenameFolder(AppState& state, const std::string& from,
                  const std::string& to);

void DissolveFolder(AppState& state, const std::string& folder);

// The document id (PartSlot::id) for a part name, or 0 if no such part.
// Occurrences reference parts by this stable id, never by name.
uint64_t PartIdByName(const AppState& state, const std::string& name);

// Drawing-edit undo (snapshot-based, the assembly pattern; workstream 13).
// Call SnapshotDrawing before mutating the active drawing's sheet/views/dims.
void SnapshotDrawing(AppState& state);

bool UndoDrawing(AppState& state);

bool RedoDrawing(AppState& state);

// Job undo (docs/cam/setup.md#undo): a full snapshot, the drawing's
// pattern. SnapshotJob BEFORE a mutation; PushJobUndo when the pre-edit copy
// is already in hand (the guard below). Undo restores the copy, re-generates
// the toolpaths and re-verifies, and bumps the version past the current one
// so every cache keyed on it drops.
void SnapshotJob(AppState& state);

void PushJobUndo(AppState& state, const cam::JobDocument& before);

bool UndoJob(AppState& state);

bool RedoJob(AppState& state);

// The one way the CAM panel's forty-nine edit sites and every mutating RPC
// get onto the undo stack without each saying so: hold a copy of the job at
// construction; at destruction -- every return path -- push the copy if the
// job was edited in between. `coalesce`: while an ImGui item is still active
// (a drag), hold the copy back and push it once when the item lets go, so a
// drag is one undo step and not sixty.
//
// "Edited" is `state.jobEdits` moving (MarkJobEdited), not the job's content:
// the panel's guards run every frame, and serializing the whole job twice a
// frame to compare was 8% of an idle frame (docs/cam/gui.md#undo).
// `compareContent` is for the RPC guard, which runs once per call and wraps
// a hundred handlers: there the content IS compared, so a handler cannot
// forget to say it changed something.
//
// Under the GUI suite (SetJobEditAudit) every guard compares the content
// too, and an edit that did not bump the counter is counted
// (JobEditAuditMisses) -- the suite fails on one, since that edit would
// silently not undo.
class JobUndoGuard {
public:
  explicit JobUndoGuard(AppState& state, bool coalesce = false,
                        bool compareContent = false);
  ~JobUndoGuard();
  JobUndoGuard(const JobUndoGuard&) = delete;
  JobUndoGuard& operator=(const JobUndoGuard&) = delete;

private:
  AppState& m_state;
  bool m_coalesce;
  bool m_compareContent;
  const cam::JobDocument* m_job = nullptr;
  uint64_t m_edits = 0;
  std::optional<cam::JobDocument> m_before;
  std::string m_fingerprint; // only when content is compared
};

// The job changed: bumps `state.jobEdits` (what JobUndoGuard records on) and
// marks the project unsaved. Every job edit calls this -- a panel field, a
// menu verb, a pick, a drag.
void MarkJobEdited(AppState& state);

// The GUI suite's audit of the rule above: while on, every JobUndoGuard also
// compares the job's content, and counts an edit that moved it without
// moving the counter.
void SetJobEditAudit(bool on);
[[nodiscard]] int JobEditAuditMisses();
[[nodiscard]] std::string JobEditAuditLastMiss();
void ResetJobEditAudit();

// The tab index of the slot with a stable id (nullopt when gone) — the
// in-context banner's Return button and RPCs resolve slots through this.
std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId);

} // namespace ui
