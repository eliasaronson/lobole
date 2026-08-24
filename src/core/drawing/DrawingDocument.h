#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// A drawing document (workstream 13, docs/drawings/plan.md): one sheet holding
// projected views of a source part or assembly, plus driven dimensions
// measured off the projected geometry. A third DocKind alongside the part
// feature-list Document and the AssemblyDocument — a drawing neither executes
// features nor places occurrences; it projects other documents onto a sheet.
//
// The model below is the D1 spine. Views and dims are carried as first-class
// lists (so serialization is stable across the milestones that fill them), but
// projected geometry and measured dimension values are DERIVED, never stored —
// opening a drawing re-projects on demand. See the design doc §2.

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
  PatternCallout
};

// Title-block fields (a simple labelled table under the border). All cosmetic
// free text; `scale`/`date` are filled by the sheet/exporter when blank.
struct TitleBlock {
  std::string project;
  std::string document;
  std::string author;
  std::string date; // ISO yyyy-mm-dd; blank = stamped at export
  std::string notes;
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

private:
  Sheet m_sheet;
  std::vector<DrawingView> m_views;
  std::vector<DrawingDim> m_dims;
  uint64_t m_nextId = 1;
  uint64_t m_version = 0;
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
inline constexpr double kTitleBlockHeightMM = 32.0;

struct SheetChrome {
  glm::dvec2 extent;
  double margin = kSheetMarginMM;
  glm::dvec2 borderMin, borderMax;
  glm::dvec2 titleBlockMin, titleBlockMax;
};
SheetChrome ChromeOf(const Sheet& sheet);

// The orthographic view direction (the way the viewer looks) and the world
// vector that maps to sheet +Y (up), for a named orientation. World Z is up;
// a Front view maps world +X -> sheet +X and +Z -> sheet +Y. Passed straight
// to drawing::ProjectView. `up` for Front/Back/Left/Right is +Z; for Top/
// Bottom the sheet is a plan, so `up` is the world Y axis.
void OrientationAxes(ViewOrientation o, glm::dvec3& viewDir, glm::dvec3& up);

const char* SheetSizeToString(SheetSize size);
[[nodiscard]] bool SheetSizeFromString(const std::string& s, SheetSize& out);
const char* ViewOrientationToString(ViewOrientation o);
[[nodiscard]] bool ViewOrientationFromString(const std::string& s,
                                             ViewOrientation& out);
const char* DrawingDimKindToString(DrawingDimKind k);
[[nodiscard]] bool DrawingDimKindFromString(const std::string& s,
                                            DrawingDimKind& out);
// Every kind string, for describe — generated beside To/FromString so the
// introspection can't drift from the parser.
std::vector<const char*> DrawingDimKindNames();
