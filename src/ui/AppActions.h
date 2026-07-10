#pragma once

#include "SketchRenderer.h" // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SketchPlane;
struct RTStillDesc; // defined in RayTracingRenderer.h

namespace ui {

// Swaps in a new document (File > New / Open) with a fresh undo stack and
// forces the renderers to resync.
void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc);

// Frames the camera on the document's combined bounding box.
void FitCameraToDocument(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry + materials)
// and (re)builds its acceleration structures. On-demand for the offline render;
// returns false if RT is unavailable or there's no geometry.
bool SyncRayTracingScene(AppState& state);

// Builds the offline-render descriptor from the current app state (camera +
// shared IBL maps) for a `width`x`height` still with the given RNG seed.
::RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                            uint32_t seed);

// Full synchronous offline still: sync the scene, build ASes, trace `samples`
// samples, tonemap and write a PNG to `path`. Blocks until done. Returns false
// (with *error set) on any failure. Used by the render_still RPC; the UI modal
// drives BeginStill/RenderBatch itself for progress. `seed` fixes the RNG so a
// given (scene, samples, seed) is reproducible.
bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, std::string* error);

// Advances an active AppState::renderJob by one sample batch per call (the
// non-blocking counterpart of RenderStillBlocking). Call once per UI frame on
// the main thread. No-op when no job is active. Updates job.done/finished/
// success/error so the Render modal can show progress.
void PumpRenderJob(AppState& state);

// Imports a STEP file as an undoable ImportFeature and fits the camera.
void LoadSTEPFile(AppState& state, const std::string& filename);

// Shared feature-creation actions (menu + toolbar call these). Each starts a
// preview the user accepts/cancels (see PreviewFeature).
void CreateBox(AppState& state);
void CreateSphere(AppState& state);
void CreateCylinder(AppState& state);
// Arms plane-picking for a new sketch.
void StartNewSketch(AppState& state);

// Whether the default planes should render this frame (visible by default,
// auto-hidden once the part has bodies, always shown while picking/editing).
bool EffectivePlaneVisibility(const AppState& state);

// Translucent default-plane quads (Front/Top/Right); hoveredPlane 0/1/2 is
// brightened, -1 for none. Empty when show is false.
std::vector<Vertex3D> BuildPlaneQuads(bool show, int hoveredPlane);

// Appends a translucent fill quad for one plane (e.g. the active sketch plane).
void AppendPlaneQuad(const SketchPlane& plane, const glm::vec3& color,
                     float half, std::vector<Vertex3D>& quads);

// Appends grid lines for a sketch's work plane (minor/major cells + colored
// U/V axes), so it is clear what surface you are drawing on.
void AppendSketchGrid(const SketchPlane& plane, std::vector<Vertex3D>& lines);

// Appends the world-origin gizmo: three arrows (X red / Y green / Z blue).
void AppendOriginGizmo(std::vector<Vertex3D>& lines);

// Highlighted line geometry for the sketch currently being edited (empty if
// none). Rendered as a per-frame overlay on top of the static sketch lines.
// Holds the rubber-band preview + gizmo/grid; entity lines and vertex dots go
// through BuildActiveSketchPrimitives instead.
std::vector<Vertex3D> BuildActiveSketchOverlay(AppState& state,
                                               uint64_t activeSketchId);

// Thick anti-aliased entity lines + constant-size vertex dots for the active
// sketch, with hover/selection emphasis. Empty if not editing a sketch.
std::vector<PrimVertex> BuildActiveSketchPrimitives(AppState& state,
                                                    uint64_t activeSketchId);

// Base-color thin lines for every sketch except the active one (which renders
// through BuildActiveSketchPrimitives). Rebuilt per-frame.
std::vector<Vertex3D> BuildStaticSketchLines(AppState& state,
                                             uint64_t activeSketchId);

} // namespace ui
