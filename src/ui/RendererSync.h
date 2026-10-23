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
enum class StillBackdrop : int; // defined in RayTracingRenderer.h (Studio = 0)
struct Ray;          // defined in FaceSelector.h

// Pushing the document at the renderers, and rendering a still.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// The whole document->renderer projection as ONE idempotent unit, in the
// order the steps depend on each other. Every step is version- or key-gated,
// so calling it when nothing changed is a handful of branches — which is what
// lets a caller that cannot know whether it is needed simply call it.
//
// It exists for --offscreen, which draws only when asked and therefore runs
// this LAZILY: the tick does not project, and everything that reads what the
// projection produces — a capture, and the RPCs that resolve an element to a
// render mesh — calls this first. The saving is not small: on a 200-body part
// a resync costs ~2-5 s (the tessellation cache does not survive a full
// regen), which an agent that never looks at the model used to pay on EVERY
// edit. The windowed loop draws every frame, so it still projects inline.
//
// A reader that forgets to call it does not fail loudly: `documentMeshes`
// still maps body index -> mesh id, for the PREVIOUS geometry. Ask
// RenderMeshesCurrent() rather than assuming.
void SyncRenderersToDocument(AppState& state);

// Are `documentMeshes` (and the picking data beside them) the projection of
// the CURRENT tab at its CURRENT version? False when nothing has been
// projected yet, when an edit has landed since, and always without a
// renderer. Volatile render ids must not be reported when this is false: the
// list still has the right SHAPE, so an index reads back a plausible id
// belonging to superseded geometry.
bool RenderMeshesCurrent(AppState& state);

// An assembly's section caps where a playback (motion or sequence) shows
// its parts, between resyncs: a cap moved with its part when the move keeps
// the section plane (a piston down a bore cut along it), cut again where
// the part now stands when it does not (a crank turning across it), and
// hidden while a sequence has not put its part in. The playbacks call it
// after pushing their poses; with no section it does nothing.
void FollowPlaybackCaps(AppState& state);

// Show a boolean-diff result ON the model: `missing` (in the reference and
// not in what you are looking at) in red, `extra` (beyond the reference) in
// green, both translucent. This is compare_geometry's `materialize` and the
// History panel's "Compare shape" — the answer to "where did it change?",
// which no single number can give.
//
// `summary` is the comparison's own words, shown with the overlay so it is
// never a mystery what the two colors are measured against. Setting a delta
// forces a renderer resync; passing empty shapes is the same as clearing.
void SetGeometryDelta(AppState& state, const CADShape& missing,
                      const CADShape& extra, const std::string& summary);

void ClearGeometryDelta(AppState& state);

// Rebuilds the delta overlay's instances, and drops it when it has stopped
// being true of what's on screen. Called from SyncDocumentRenderers.
void SyncGeometryDelta(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry +
// materials) and (re)builds its acceleration structures. On-demand for the
// offline render; returns false if RT is unavailable or there's no geometry.
// `groundPlane` adds a shadow-catcher quad under the scene (offline renders
// only).
bool SyncRayTracingScene(AppState& state, bool groundPlane = false);

// What stands behind a traced still and lights it — render_still's
// `backdrop` and, for the sky, where the sun stands
// (docs/rendering/still-render.md#the-sky). Value-initialized, it is the
// studio sweep.
struct StillBackground {
  ::StillBackdrop backdrop{};
  // Sky only, in degrees: azimuth counter-clockwise from world +X seen from
  // above, elevation above the ground plane. Unset = derived: the azimuth
  // from where the studio key light stands for this camera, the elevation
  // kDefaultSunElevationDeg. MakeStillDesc writes back what it used.
  std::optional<double> sunAzimuthDeg;
  std::optional<double> sunElevationDeg;
};
// The default sun's height. Not a model-scale quantity — an angle means the
// same thing on a watch and on a house — so a fixed value is right here: a
// mid-morning sun, high enough for short shadows that still model the form.
inline constexpr double kDefaultSunElevationDeg = 40.0;

// Builds the offline-render descriptor from the current app state (camera +
// shared IBL maps) for a `width`x`height` still with the given RNG seed.
// `outlines` is ANDed with the viewport's own outline mode, so a still draws
// edges exactly when the screen it came from does. A sky background fills in
// the sun it used; without its maps (no renderer) the descriptor has no
// irradiance, which every caller refuses.
::RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                            uint32_t seed, StillBackground& background,
                            bool outlines = true);

// Why a still cannot be taken of the active tab, or "" when it is a part or
// an assembly: a drawing or a CAM job tab has no scene, and saying "no
// geometry" about a model with 128 bodies sends the reader looking in the
// wrong place. Names the tab and its kind.
std::string StillTabRefusal(const AppState& state);

// Why the path-traced still cannot be made here, or "" when it can: no
// renderer (--headless), a lost device, or a device with no ray tracing —
// a software one (lavapipe) says so by name, since it is the machine and not
// a setting. Quick stills are unaffected (docs/rendering/still-render.md).
std::string PathTracerRefusal(const AppState& state);

// Full synchronous offline still: sync the scene, build ASes, trace `samples`
// samples, tonemap and write a PNG to `path`. Blocks until done. Returns false
// (with *error set) on any failure. Used by the render_still RPC; the UI modal
// drives BeginStill/RenderBatch itself for progress. `seed` fixes the RNG so a
// given (scene, samples, seed) is reproducible.
// `warning` (optional, out): non-empty when the still is a valid picture of
// nothing — the scene's bounds fall entirely outside the camera's frustum.
// Saved is not the same as good, and this is the difference.
bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, bool groundPlane, std::string* error,
                         std::string* warning = nullptr,
                         StillBackground* background = nullptr,
                         bool outlines = true);

// Advances an active AppState::renderJob by one step per call: a path trace
// by one sample batch (the non-blocking counterpart of RenderStillBlocking),
// a quick render by its capture and then polling its encode worker. Call once
// per UI frame on the main thread. No-op when no job is active. Updates
// job.done/progress/finished/success/error so the Render modal can show
// progress.
void PumpRenderJob(AppState& state);

} // namespace ui
