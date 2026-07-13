#pragma once

#include "SketchRenderer.h" // Vertex3D, PrimVertex
#include "core/ElementMap.h" // ElementRef
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
struct RTStillDesc; // defined in RayTracingRenderer.h

namespace ui {

// Swaps in a new document (File > New / Open) with a fresh undo stack and
// forces the renderers to resync.
void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc);

// Multi-part projects (assemblies A1). EnsureParts makes the parts list
// non-empty (one slot for the active document) — call once after the initial
// document is created. AddPart appends a new empty part and switches to it.
// SetActivePart swaps the active document out to its slot and the target in.
// DeletePart removes a part (never the last). All resync the renderers.
void EnsureParts(AppState& state, const std::string& firstName = "Part 1");
size_t AddPart(AppState& state, const std::string& name);
void SetActivePart(AppState& state, size_t index);
bool DeletePart(AppState& state, size_t index);

// Save/open a project directory. A multi-part project writes a project.json
// index + one `parts/<name>/` directory each; a single-part project stays
// the flat `.dsgn` (index.json + features/) form for backward compatibility.
// Open detects project.json to load all parts, else loads one part.
bool SaveProject(AppState& state, const std::string& path, std::string* error);
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

// Frames the camera on the document's combined bounding box.
void FitCameraToDocument(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry + materials)
// and (re)builds its acceleration structures. On-demand for the offline render;
// returns false if RT is unavailable or there's no geometry. `groundPlane`
// adds a shadow-catcher quad under the scene (offline renders only).
bool SyncRayTracingScene(AppState& state, bool groundPlane = false);

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
                         uint32_t seed, bool groundPlane, std::string* error);

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
// Starts a preview of an offset datum plane (from XY, or from the selected
// datum if one is selected) — the first of the DatumPlane variants.
void CreateDatumPlane(AppState& state);
// Starts a preview of a two-point datum axis (along X by default).
void CreateDatumAxis(AppState& state);
// Starts a preview of a boolean combining the two most-recent body producers.
void CreateBoolean(AppState& state);
// Starts a preview of a mirror / linear pattern of the most-recent body.
void CreateMirror(AppState& state);
void CreatePattern(AppState& state);
// Starts a fillet/chamfer preview and arms edge picking: viewport clicks
// toggle B-Rep edges on the pending feature (M7).
void CreateFillet(AppState& state);
void CreateChamfer(AppState& state);
// Arms plane-picking for a new sketch.
void StartNewSketch(AppState& state);

// The stable ElementRef for a picked B-Rep edge (the body's createdBy + the
// edge's MappedName); nullopt when the edge has no stable name.
std::optional<ElementRef> EdgeElementRef(AppState& state, uint32_t meshId,
                                         uint32_t edgeIndex);

// Routes viewport hover/click to B-Rep edge picking while state.edgePick is
// active (fillet/chamfer edge collection). Returns true when it consumed the
// viewport input for this frame.
bool HandleEdgePickInput(AppState& state, bool overViewport);

// Appends edge highlight primitives while edge picking is active: every body
// edge dimmed, the hovered edge and the blend's picked edges emphasized.
void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out);

// The features in [0, beforeIndex) that produce a body (primitives, imports,
// extrude/revolve in NewBody mode, and earlier booleans), as {featureId,
// name}. Backs the boolean operand combos and the "Boolean" menu gate.
std::vector<std::pair<uint64_t, std::string>> BodyProducers(
    const Document& doc, size_t beforeIndex);

// Appends translucent quads for every DatumPlaneFeature in the document,
// tinted distinctly from the base planes; the hovered/selected/pending datum
// is brightened. Visible while picking a plane, or when showPlanes is on
// (datums are user reference geometry, so they don't auto-hide behind bodies).
void AppendDatumPlaneQuads(AppState& state, std::vector<Vertex3D>& quads);

// Appends a line segment for every DatumAxisFeature (same visibility rule as
// the datum planes), tinted distinctly; selected/pending is brightened.
void AppendDatumAxisLines(AppState& state, std::vector<Vertex3D>& lines);

// Draws each visible datum plane's and datum axis's name as a 2D label
// (foreground draw list). Call inside the ImGui frame, near DrawDimensions.
void DrawDatumLabels(AppState& state);

// When state.showElementNames is on, labels the hovered face with its stable
// element name (topological-naming debug overlay). No-op otherwise.
void DrawElementNameOverlay(AppState& state);

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
