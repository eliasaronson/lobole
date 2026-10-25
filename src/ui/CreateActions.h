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

// Creating features: what each tool makes when the toolbar asks for one.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

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

// A named datum frame (v86, agent-scale plan G0), explicit at the part origin
// until edited; a mate anchors to it by name.
void CreateFrame(AppState& state);

void CreateCurve3D(AppState& state); // 3D curve (helix / fit-spline / poly)
void CreatePipe(AppState& state);    // pipe/tube along a path
void CreateSweep(AppState& state);   // sweep a profile along a path
void CreateLoft(AppState& state);    // loft through sections (optional guide)

// Menu/palette gating queries for the curve-driven tools.
size_t SketchFeatureCount(const Document& doc);

bool HasCurveOrSketch(const Document& doc);

// Starts a preview of a boolean combining the two most-recent body producers.
void CreateBoolean(AppState& state);

// Starts a preview of a mirror / linear pattern of the most-recent body.
void CreateMirror(AppState& state);

void CreatePattern(AppState& state);

// Starts a fillet/chamfer preview and arms edge picking: viewport clicks
// toggle B-Rep edges on the pending feature (M7).
void CreateFillet(AppState& state);

void CreateChamfer(AppState& state);

void CreateRib(AppState& state);

void CreateThread(AppState& state);

// A worm, or a worm wheel, cut from a cylindrical blank: pick its outside
// face (docs/modeling/worm-gears.md).
void CreateWorm(AppState& state);
void CreateWormWheel(AppState& state);

// Body-modifying tools whose parameters/slots already existed but that had no
// creation path outside the script API. Each opens the normal preview with
// the most recent body pre-targeted (Draft takes its body from the first face
// pick — it has no body slot); the required slots arm themselves.
void CreateShell(AppState& state);

void CreateDraft(AppState& state);

// The direct edits (docs/modeling/README.md#direct-edit): each opens its
// preview with the faces seeded from the selection, and refuses until they
// are picked. Offset's distance and Move's translation are derived from the
// face or body they act on (SizeFromFace, DefaultFaceMove).
void CreateDeleteFace(AppState& state);
void CreateOffsetFace(AppState& state);
void CreateMoveFace(AppState& state);
void CreateReplaceFace(AppState& state);

// A tenth of the smaller of the face's inset (list_elements' `inset_mm`: how
// far its inside reaches from its boundary) and its body's thinnest extent,
// rounded down to 1, 2 or 5 x 10^n: an offset that face can take. The face
// is resolved on the document's current bodies; `fallback` when it is not
// there.
double SizeFromFace(const AppState& state, const ElementRef& face,
                    double fallback);

// 1, 2 or 5 x 10^n at or below `raw` (> 0): a number a person would type.
// The one rounding every derived default here goes through.
[[nodiscard]] double RoundDown125(double raw);

// The thread a cylindrical face takes: the ISO metric coarse size whose
// major diameter (a boss) or tap drill (a hole) is nearest the face's
// diameter. Empty when the face is not a cylinder. Picking the face into a
// Thread tool sizes it with this.
[[nodiscard]] std::string ThreadForFace(const AppState& state,
                                        const ElementRef& face);

// Move Face's default: in the host face's plane (the face the picked ones
// sit in), along the body's largest extent in that plane, a tenth of it,
// rounded as SizeFromFace rounds. With no faces, along the body's largest
// extent. Zero when there is no body.
glm::dvec3 DefaultFaceMove(const AppState& state,
                           const std::vector<ElementRef>& faces);

void CreateSplit(AppState& state);

// Sheet metal (docs/sheetmetal/README.md). Each opens the normal preview; the
// defaults come from the document's sheet-metal rules, so none of them
// carries a literal millimetre of its own.
void CreateBaseFlange(AppState& state);

void CreateEdgeFlange(AppState& state);

void CreateFlatPattern(AppState& state);

// Unfold lays every bend of the newest body flat; Refold folds back the most
// recent Unfold that is still open.
void CreateUnfold(AppState& state);

void CreateRefold(AppState& state);

// A hem's return defaults to four thicknesses, the usual least a hemming die
// takes; a jog's height to the least its two bends allow plus one
// thickness, on the newest sketch that holds one line.
void CreateHem(AppState& state);

void CreateJog(AppState& state);

// Relieves every corner of the newest body where two bends cross.
void CreateCornerRelief(AppState& state);

// Opens a normal cut with no sketch chosen: the newest closed sketch is, as
// often as not, the one the blank was made from.
void CreateNormalCut(AppState& state);

// The plane a plane-referencing feature points at (a base plane, or an
// earlier DatumPlane by id), as the UI sees it: the datum's last regenerated
// output. nullopt when the id does not resolve. The regen path has
// ResolveParentPlane for the same job; this is its caller-side twin, for
// tools that must know where a plane IS while the user is picking it.
std::optional<SketchPlane>
ResolvePlaneForTool(AppState& state, uint64_t datumId, PlaneType basePlane);

// Re-derives a split's offset so the knife lands mid-body on its CURRENT
// plane. Called when the plane changes: an offset measured from the old
// plane means something else on the new one. Leaves a user-typed expression
// alone — that is theirs.
void RecenterSplitOffset(AppState& state, SplitFeature& split);

void CreateRefine(AppState& state);

// Arms plane-picking for a new sketch.
void StartNewSketch(AppState& state);

// Arms the Hole tool: pick a FLAT face (or plane) to drill into, which opens
// a point sketch on it with the Point tool ready; ✓/Enter then converts the
// centres into a Hole preview. A single planar face already selected skips
// the pick. No-op without a body (there would be nothing to cut).
void StartHoleTool(AppState& state);

// The features in [0, beforeIndex) that produce a body (primitives, imports,
// extrude/revolve in NewBody mode, and earlier booleans), as {featureId,
// name}. Backs the boolean operand combos and the "Boolean" menu gate.
std::vector<std::pair<uint64_t, std::string>> BodyProducers(const Document& doc,
                                                            size_t beforeIndex);

// Cosmetic thread overlays (M8 thread): dashed thread-depth circles at both
// ends of each threaded span + dashed crest lines — the drafting convention,
// drawn from the session overlay each ThreadFeature computes at regen.
void AppendThreadOverlays(AppState& state, std::vector<LineVertex3D>& lines);

} // namespace ui
