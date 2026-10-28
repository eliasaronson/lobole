// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace drawing {
struct ViewProjection; // core/drawing/ViewProjection.h
}

// A drawing document (docs/drawings/README.md): one sheet holding
// projected views of a source part or assembly, plus driven dimensions
// measured off the projected geometry. A third DocKind alongside the part
// feature-list Document and the AssemblyDocument — a drawing neither executes
// features nor places occurrences; it projects other documents onto a sheet.
//
// Views and dims are carried as first-class lists. Measured dimension values
// are DERIVED, never stored; projected geometry is derived too, and kept
// only as each view's saved projection (SavedProjections, below), which
// opening installs when its inputs still hash the same.

// Sheet paper size. Landscape/portrait is a separate flag; these are the
// portrait short×long extents in millimetres (ISO A-series + US Letter).
enum class SheetSize { A4, A3, A2, A1, A0, Letter };

// A named orthographic looking direction for a base view; projected children
// fold from a base view rather than carrying an orientation of their own.
enum class ViewOrientation { Front, Back, Left, Right, Top, Bottom, Iso };

// The driven dimension kinds — the six that carry over from sketch dimensions
// (SketchFeature.h ConstraintKind), measured here rather than solved, plus
// the v53 callouts: HoleCallout derives its text from the HoleFeature that
// drilled the picked circle; PatternCallout counts 2+ picked circles and
// reports their uniform pitch. Still driven — the text is computed from the
// model, never authored.
enum class DrawingDimKind {
  Distance,
  DistanceX,
  DistanceY,
  Radius,
  Diameter,
  Angle,
  HoleCallout,
  PatternCallout,
  // A leader naming the standard part it points at: "ISO 4762 M6x50", read
  // out of the occurrence's own Fastener feature. Only on an ASSEMBLY view —
  // a part view has no occurrence, so there is nothing to name.
  FastenerCallout
};

// Title-block fields (a simple labelled table under the border). All cosmetic
// free text; `material` and `scale` are filled at export when blank, from the
// sheet's first view (its part's material, its scale), so a sheet says what
// it is made of and how big it is drawn without anybody typing either --
// and a grade the material library does not carry ("6082-T6") is typed
// here (v147).
struct TitleBlock {
  std::string project;
  std::string document;
  std::string author;
  std::string date; // ISO yyyy-mm-dd; blank = stamped at export
  // General notes: the tolerance standard unwritten sizes are held to, the
  // edge breaks. Printed in the block's NOTES row.
  std::string notes;
  std::string material; // v147; blank = the first view's part's material
  std::string scale;    // v147; blank = the first view's scale ("1:2")
};

struct Sheet {
  SheetSize size = SheetSize::A4;
  bool landscape = true;
  TitleBlock titleBlock;
};

// A reference from a dimension to a piece of PROJECTED geometry, resolved
// against a view's re-projected curves on every update (design doc §3). A
// sharp/smooth edge is named by its source 3D edge's ElementRef; a silhouette
// by its generating face's ElementRef. For assembly sources the ref is
// occurrence-qualified. `end` picks a vertex: 0 = whole curve, 1|2 = endpoints.
struct ProjRef {
  uint64_t viewId = 0;
  uint64_t occurrenceId =
      0;              // 0 = part source; else the occurrence in an assembly
  ElementRef edge;    // set for sharp/smooth edges
  ElementRef silFace; // set for silhouettes (the generating face)
  int end = 0;        // 0 = whole edge, 1|2 = an endpoint vertex
  // The curve's point furthest toward one side of the sheet: "left",
  // "right", "top" or "bottom"; empty = `end` decides (v134). The top of a
  // turned piece is the interior of its outline curve -- no end and no
  // midpoint of anything -- and is what a height dimension measures to.
  std::string extreme;
};

// Whether `s` is a ProjRef extreme a file or a caller may name ("" too).
[[nodiscard]] bool IsProjRefExtreme(const std::string& s);

// A section view's cut (v147, docs/drawings/README.md#section-views): the
// plane through `point` square to the view's own direction. What lies
// between the reader and the plane is taken away, the faces the cut exposes
// are hatched, and every other view of the same source that sees the plane
// edge-on draws it as a cutting-plane line lettered `label` at both ends.
struct SectionSpec {
  bool on = false;
  glm::dvec3 point{0.0}; // world mm (the source document's frame)
  std::string label;     // "A" -> "SECTION A-A"
};

// A detail view (v151, docs/drawings/README.md#detail-views): a circle on a
// PARENT view, drawn again at the detail's own scale. The detail projects
// what its parent projects -- source, orientation, rotation, section, hidden
// occurrences, configuration -- and keeps what lies inside the circle. The
// circle is a world point (the source's frame, like a section's point) and a
// radius in model mm, so it stays on its feature when the parent moves or is
// rescaled.
struct DetailSpec {
  bool on = false;
  uint64_t parentViewId = 0;
  glm::dvec3 center{0.0}; // world mm
  double radius = 0.0;    // model mm
  std::string label;      // "B" -> "DETAIL B"
};

// A view on the sheet: a base view of a source document in a named
// orientation, or an orthographically-folded child kept aligned to its base.
struct DrawingView {
  uint64_t id = 0;
  std::string name;
  uint64_t sourceDocId = 0; // stable project-document id (part OR assembly)
  ViewOrientation orientation = ViewOrientation::Front;
  uint64_t baseViewId = 0; // 0 = base view; else the base this folds from
  double scale = 1.0;      // drawing:model ratio (1:2 -> 0.5)
  std::string
      scaleExpr; // optional expression driving `scale` (empty = literal)
  glm::dvec2 position{0.0}; // sheet mm; a child moves only along its fold axis
  bool showHidden = true;
  // "exact" (the default, and the deliverable) or "fast" — polygonal HLR, a
  // LOOK at a sheet that would otherwise take a minute and a half a view.
  // A string rather than the enum so it serializes and refuses by name like
  // every other enum on the wire (docs/drawings/README.md#projection-hlr).
  std::string quality = "exact";
  // What to leave OFF this view, by id: occurrences of an assembly source,
  // or the creating features of a part source's bodies (a body name is
  // resolved to its feature when the view is set). Empty = draw everything.
  //
  // Exact HLR is priced by surface complexity: the single-cylinder engine is
  // 47 s a view, and 0.0 s with its two valve springs hidden — seventeen of
  // its nineteen bodies project instantly. Leaving the sweeps off a sheet is
  // the difference between a real dimensionable drawing and no drawing at all
  // (generators/README.md#single-cylinder-and-air-cooled-engines).
  //
  // On a part it is the ground under a house or a fixture beside a casting:
  // a body the model needs that this sheet does not
  // (docs/drawings/README.md#leaving-bodies-off-a-view).
  std::vector<uint64_t> hidden;
  // Which configuration of a PART source this view draws; 0 = whatever the
  // part has active. A sheet names its configuration so a flat-pattern view
  // shows the blank whichever variant the model tab is on — the drawing of
  // the blank and the drawing of the folded part are two views of one part
  // (docs/sheetmetal/README.md#dxf-and-drawings).
  uint64_t configuration = 0;
  // Turns the drawn view on the sheet, degrees counter-clockwise about its
  // own view direction (v134). A part modelled lying along X -- a turned
  // chess piece -- draws upright as its front view turned 90, with no
  // rotated copy of the model.
  double rotationDeg = 0.0;
  // A section view: cut by a plane square to its view direction (v147).
  SectionSpec section;
  // A detail view of another view (v151); its own position is where the
  // circle's centre lands on the sheet.
  DetailSpec detail;
  // An exploded view of an ASSEMBLY source (vNEXT): the id of one of its
  // named exploded views, each occurrence drawn displaced by its step at
  // full factor, whatever the model tab's explode slider says. 0 = as
  // assembled. The sheet's, not the model's: one sheet draws the assembly
  // whole and exploded side by side
  // (docs/drawings/README.md#exploded-views).
  uint64_t explodedView = 0;
};

// A driven dimension: measured from projected geometry, read-only, re-measured
// on view update. `refs` holds one or two ProjRefs per `kind`.
struct DrawingDim {
  uint64_t id = 0;
  DrawingDimKind kind = DrawingDimKind::Distance;
  uint64_t viewId = 0;
  std::vector<ProjRef> refs;
  glm::dvec2 labelOffset{0.0}; // sheet mm, the sketch labelOffset pattern
};

// A free text note on the sheet ("6x Ø4.4 on Ø175.8 BC", finish, material)
// — the class of annotation no dimension kind covers. Optional leader: when
// `leader` is set, a line runs from the resolved geometry (a picked edge or
// circle in `viewId`, resolved like a dim ref) to the text.
// What a note's text is generated from, if anything. A table note carries
// only a caption in `text`; its rows come from the source of `viewId` each
// time it is resolved, so it cannot go stale against the part.
//
// Bom (v147) is the parts list of an ASSEMBLY view: one row per part the
// view draws (its `hidden` occurrences are left out, so the load a head
// carries is not listed as part of the head), drawn as a ruled table whose
// bottom-right corner is the note's position -- where it stands on the title
// block.
enum class NoteTable { None, Bends, Bom };

// How a note is drawn (v147). A balloon is the item number of the part its
// leader points at, in a circle, with a dot on the part: on an assembly view
// with an empty text, the number is the part's row in the sheet's parts list
// of that assembly (else the view's own), so the two cannot disagree. A datum
// is a datum feature symbol: the letter in a box, the leader ending in a filled
// triangle on the feature.
//
// Fcf (v151) is a feature control frame (ISO 1101): the tolerance symbol,
// the zone and the datums it is measured from, drawn as the ruled frame with
// a leader to the feature -- or stacked with a dimension's value when
// `attachDim` names one, which is how a size's position tolerance is given.
enum class NoteStyle { Text, Balloon, Datum, Fcf };

// The ISO 1101 geometric characteristics (ASME Y14.5 draws the same
// symbols). Which take a datum, a Ø zone or a material modifier is
// GeoSymbolRules' answer.
enum class GeoSymbol {
  Straightness,
  Flatness,
  Circularity,
  Cylindricity,
  ProfileLine,
  ProfileSurface,
  Parallelism,
  Perpendicularity,
  Angularity,
  Position,
  Coaxiality,
  Symmetry,
  CircularRunout,
  TotalRunout
};

// A material condition on the tolerance: maximum (M) or least (L).
enum class MaterialModifier { None, Mmc, Lmc };

struct FeatureControlFrame {
  GeoSymbol symbol = GeoSymbol::Position;
  double tolerance = 0.0;    // model mm, the zone's width or diameter
  bool diameterZone = false; // Ø: a cylindrical zone
  MaterialModifier modifier = MaterialModifier::None;
  // Datum letters, primary first; each names a `datum` note on the sheet.
  std::vector<std::string> datums;
};

struct DrawingNote {
  uint64_t id = 0;
  std::string text;
  glm::dvec2 pos{0.0}; // text anchor, sheet mm
  uint64_t viewId = 0; // 0 = free note; else the leader's view
  ProjRef leader;      // unset = no leader line
  NoteTable table = NoteTable::None;
  NoteStyle style = NoteStyle::Text;
  FeatureControlFrame fcf; // style Fcf only (v151)
  // A frame stacked with this dimension's value instead of standing at
  // `pos` with a leader (v151). 0 = not attached.
  uint64_t attachDim = 0;
};

// The document. Views and dims share one never-reused id counter (the
// AssemblyDocument rule: an id names a thing unambiguously across the file).
class DrawingDocument {
public:
  const Sheet& GetSheet() const { return m_sheet; }
  Sheet& GetSheet() { return m_sheet; }

  const std::vector<DrawingView>& GetViews() const { return m_views; }
  std::vector<DrawingView>& GetViews() { return m_views; }
  size_t ViewCount() const { return m_views.size(); }

  // Appends a view, assigning a fresh stable id (the caller-supplied id is
  // overwritten). Returns the assigned id.
  uint64_t AddView(DrawingView view);
  bool RemoveView(uint64_t id); // also removes dims that reference it
  DrawingView* FindView(uint64_t id);
  const DrawingView* FindView(uint64_t id) const;

  const std::vector<DrawingDim>& GetDims() const { return m_dims; }
  std::vector<DrawingDim>& GetDims() { return m_dims; }
  size_t DimCount() const { return m_dims.size(); }

  uint64_t AddDim(DrawingDim dim);
  bool RemoveDim(uint64_t id);

  const std::vector<DrawingNote>& GetNotes() const { return m_notes; }
  std::vector<DrawingNote>& GetNotes() { return m_notes; }
  size_t NoteCount() const { return m_notes.size(); }
  uint64_t AddNote(DrawingNote note);
  bool RemoveNote(uint64_t id);
  DrawingDim* FindDim(uint64_t id);
  const DrawingDim* FindDim(uint64_t id) const;

  // Loader hook: pin the id counter past the highest restored id so future
  // adds never collide. No-op if `next` is not greater than the counter.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId)
      m_nextId = next;
  }
  uint64_t NextId() const { return m_nextId; }

  // Monotonic change counter (the Document/AssemblyDocument pattern) so the
  // renderer can skip resyncing an unchanged sheet.
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

  // Each view's projection as it is saved with the drawing
  // (docs/drawings/README.md#saved-projections): read from the directory
  // form's views/<id>-<slug>.projection.{json,bin} and written back to it.
  // Derived, like a study's result: it is not part of the drawing's content
  // or its Version, and the single-file form carries none. Whether one still
  // answers its view is its input hash's to say, which the projecting job
  // checks before drawing it; a view of none, or of a stale one, projects.
  using ProjectionMap =
      std::map<uint64_t, std::shared_ptr<const drawing::ViewProjection>>;
  const ProjectionMap& SavedProjections() const { return m_projections; }
  std::shared_ptr<const drawing::ViewProjection>
  SavedProjection(uint64_t viewId) const {
    const auto it = m_projections.find(viewId);
    return it == m_projections.end() ? nullptr : it->second;
  }
  // Null forgets the view's.
  void SetSavedProjection(uint64_t viewId,
                          std::shared_ptr<const drawing::ViewProjection> p) {
    if (p)
      m_projections[viewId] = std::move(p);
    else
      m_projections.erase(viewId);
  }

private:
  Sheet m_sheet;
  std::vector<DrawingView> m_views;
  std::vector<DrawingDim> m_dims;
  std::vector<DrawingNote> m_notes;
  uint64_t m_nextId = 1;
  uint64_t m_version = 0;
  ProjectionMap m_projections;
};

// Portrait short/long sheet extents in millimetres.
glm::dvec2 SheetExtentMM(SheetSize size);

// The sheet's fixed furniture, computed once from the Sheet: the ORIENTED
// paper extent (landscape swap applied), the ISO border margin, and the
// title-block rect pinned to the border's bottom-right corner. The single
// source the viewport, the SVG/PDF exporters, and the validator share —
// change a constant here and every consumer (and check_drawing) agrees.
inline constexpr double kSheetMarginMM = 10.0;
inline constexpr double kTitleBlockWidthMM = 160.0;
// Eight 6 mm rows: project, document, material, scale, author, date, units,
// notes.
inline constexpr double kTitleBlockHeightMM = 48.0;

struct SheetChrome {
  glm::dvec2 extent;
  double margin = kSheetMarginMM;
  glm::dvec2 borderMin, borderMax;
  glm::dvec2 titleBlockMin, titleBlockMax;
};
SheetChrome ChromeOf(const Sheet& sheet);

// The orthographic view direction (the way the viewer looks) and the world
// vector that maps to sheet +Y (up), for a named orientation. Derived from
// conv::kNamedViews (core/Conventions.h) so a drawing's "front" is the same
// front as the view cube's and the `view` RPC's. Passed straight to
// drawing::ProjectView.
void OrientationAxes(ViewOrientation o, glm::dvec3& viewDir, glm::dvec3& up);
// A view's own axes: its orientation's, turned by its rotationDeg. Every
// projection of a view goes through this, not OrientationAxes.
void ViewAxes(const DrawingView& view, glm::dvec3& viewDir, glm::dvec3& up);

const char* SheetSizeToString(SheetSize size);
[[nodiscard]] bool SheetSizeFromString(const std::string& s, SheetSize& out);
const char* ViewOrientationToString(ViewOrientation o);
[[nodiscard]] bool ViewOrientationFromString(const std::string& s,
                                             ViewOrientation& out);
const char* NoteTableToString(NoteTable t);
[[nodiscard]] bool NoteTableFromString(const std::string& s, NoteTable& out);
// The refusal both the loader and the RPC say, written once: two copies of a
// list is how one of them ends up naming a value the other does not accept.
[[nodiscard]] std::string UnknownNoteTable(const std::string& given);
const char* NoteStyleToString(NoteStyle s);
[[nodiscard]] bool NoteStyleFromString(const std::string& s, NoteStyle& out);
[[nodiscard]] std::string UnknownNoteStyle(const std::string& given);
const char* GeoSymbolToString(GeoSymbol s);
[[nodiscard]] bool GeoSymbolFromString(const std::string& s, GeoSymbol& out);
[[nodiscard]] std::string UnknownGeoSymbol(const std::string& given);
const char* MaterialModifierToString(MaterialModifier m);
[[nodiscard]] bool MaterialModifierFromString(const std::string& s,
                                              MaterialModifier& out);
[[nodiscard]] std::string UnknownMaterialModifier(const std::string& given);
// What a frame may carry, by its symbol (ISO 1101): whether a datum is
// required, allowed or refused, and whether a Ø zone or a material modifier
// means anything for it.
struct GeoSymbolRule {
  enum class Datums { None, Optional, Required } datums = Datums::Optional;
  bool diameterZone = false;
  bool modifier = false;
};
[[nodiscard]] GeoSymbolRule GeoSymbolRules(GeoSymbol s);
// Empty when `fcf` is a frame ISO 1101 can write; else why not, naming
// what the symbol does take. Datum letters are checked for shape here
// (one capital, not I, O or Q); whether the sheet HAS them is the
// drawing's question.
[[nodiscard]] std::string ValidateFcf(const FeatureControlFrame& fcf);
// Empty when `letter` can name a datum feature: one capital letter, not I,
// O or Q (ISO 5459 leaves those out: they read as 1, 0 and 0).
[[nodiscard]] std::string ValidateDatumLetter(const std::string& letter);
const char* DrawingDimKindToString(DrawingDimKind k);
[[nodiscard]] bool DrawingDimKindFromString(const std::string& s,
                                            DrawingDimKind& out);
// Every kind string, for describe — generated beside To/FromString so the
// introspection can't drift from the parser.
std::vector<const char*> DrawingDimKindNames();
