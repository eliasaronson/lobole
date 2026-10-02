#include "ui/DrawingAnnotate.h"

#include "core/drawing/DrawingDimension.h"  // NoteGraphics, DimLabelBox
#include "core/drawing/DrawingProjection.h" // ProjectPoint
#include "ui/DocumentSlots.h"               // SnapshotDrawing, SlotIndexById
#include "ui/DrawingActions.h"
#include "ui/DrawingSheet.h"
#include "ui/ExprInput.h"
#include "ui/PanelLayout.h"    // LeftLabel
#include "ui/PreviewFeature.h" // BeginToolWindowFrame
#include "ui/Theme.h"          // AccentColor

#include "IconsFontAwesome6.h"

#include "imgui.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {

namespace {

using NoteKind = AppState::DrawingNoteTool::Kind;
using DetailDrag = AppState::DetailTool::Drag;

constexpr double kPi = 3.14159265358979323846;
const ImU32 kAccentInk = IM_COL32(80, 170, 255, 255);
const ImVec4 kRefusal(0.90f, 0.40f, 0.40f, 1.0f);
const ImVec4 kMissing(0.85f, 0.75f, 0.40f, 1.0f);

const GeoSymbol kSymbols[] = {
    GeoSymbol::Straightness,   GeoSymbol::Flatness,
    GeoSymbol::Circularity,    GeoSymbol::Cylindricity,
    GeoSymbol::ProfileLine,    GeoSymbol::ProfileSurface,
    GeoSymbol::Parallelism,    GeoSymbol::Perpendicularity,
    GeoSymbol::Angularity,     GeoSymbol::Position,
    GeoSymbol::Coaxiality,     GeoSymbol::Symmetry,
    GeoSymbol::CircularRunout, GeoSymbol::TotalRunout};
const MaterialModifier kModifiers[] = {
    MaterialModifier::None, MaterialModifier::Mmc, MaterialModifier::Lmc};

// A distance on screen, in sheet mm at the current zoom: pick tolerances
// are pixels, as everywhere else.
double PxToMM(const AppState& state, double px) {
  return state.sheetView.scale > 0.0 ? px / state.sheetView.scale : px;
}

bool PressedOnSheet(const AppState& state) {
  const ImGuiIO& io = ImGui::GetIO();
  return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt &&
         !state.sheetView.panning;
}

void HighlightCurve(ImDrawList* dl, AppState& state,
                    const drawing::SheetCurve& c) {
  for (size_t i = 1; i < c.points.size(); ++i)
    dl->AddLine(SheetToScreen(state, c.points[i - 1]),
                SheetToScreen(state, c.points[i]), kAccentInk, 2.6f);
}

void SheetCircle(ImDrawList* dl, AppState& state, glm::dvec2 c, double r,
                 ImU32 col, float px) {
  constexpr int kSteps = 72;
  for (int i = 0; i < kSteps; ++i) {
    const double a0 = 2.0 * kPi * i / kSteps, a1 = 2.0 * kPi * (i + 1) / kSteps;
    dl->AddLine(
        SheetToScreen(state, c + r * glm::dvec2(std::cos(a0), std::sin(a0))),
        SheetToScreen(state, c + r * glm::dvec2(std::cos(a1), std::sin(a1))),
        col, px);
  }
}

// The curve a ref names on its view's projection, or null.
const drawing::SheetCurve* CurveOfRef(AppState& state, const ProjRef& ref) {
  const auto it = state.drawingRenders.find(ref.viewId);
  if (it == state.drawingRenders.end())
    return nullptr;
  for (const drawing::SheetCurve& c : it->second.curves) {
    if (c.sourceId != ref.occurrenceId)
      continue;
    if (ref.edge.IsSet() && c.edge.IsSet() &&
        c.edge.featureId == ref.edge.featureId && c.edge.name == ref.edge.name)
      return &c;
    if (ref.silFace.IsSet() && c.silFace.IsSet() &&
        c.silFace.featureId == ref.silFace.featureId &&
        c.silFace.name == ref.silFace.name)
      return &c;
  }
  return nullptr;
}

bool IsRound(const drawing::SheetCurve& c) {
  return c.closed && c.conicRadius > 0.0;
}

// What a picked feature measures, model mm: a whole circle's diameter, any
// other curve's length -- what a general tolerance is looked up by.
double FeatureSizeMM(AppState& state, const drawing::SheetCurve& c,
                     uint64_t viewId) {
  const DrawingView* v = state.drawing->FindView(viewId);
  const double s = v != nullptr && v->scale > 0.0 ? v->scale : 1.0;
  double len = 0.0;
  if (IsRound(c))
    len = 2.0 * c.conicRadius;
  else
    for (size_t i = 1; i < c.points.size(); ++i)
      len += glm::length(c.points[i] - c.points[i - 1]);
  return len / s;
}

// The note the session would add, as it stands.
DrawingNote CandidateNote(const AppState& state) {
  const auto& t = state.drawingNoteTool;
  DrawingNote n;
  n.style = t.kind == NoteKind::Fcf ? NoteStyle::Fcf : NoteStyle::Datum;
  if (t.kind == NoteKind::Datum)
    n.text = t.letter;
  else
    n.fcf = t.fcf;
  if (t.dimId != 0) {
    n.attachDim = t.dimId;
  } else if (t.targeted) {
    n.viewId = t.ref.viewId;
    n.leader = t.ref;
  }
  n.pos = t.pos;
  return n;
}

void DeriveTolerance(AppState& state) {
  auto& t = state.drawingNoteTool;
  if (!t.toleranceTyped)
    t.fcf.tolerance = DefaultGeoTolerance(t.fcf.symbol, t.featureMM);
}

void TargetCurve(AppState& state, const drawing::SheetCurve& c,
                 uint64_t viewId) {
  auto& t = state.drawingNoteTool;
  t.ref = RefOfCurve(c, viewId);
  t.dimId = 0;
  t.targeted = true;
  t.featureMM = FeatureSizeMM(state, c, viewId);
  if (t.kind == NoteKind::Fcf && !t.symbolChosen) {
    // A bore is located; a face is flat. Both stand without a datum, so the
    // frame a pick opens on is one the sheet can take as it is.
    t.fcf.symbol = IsRound(c) ? GeoSymbol::Position : GeoSymbol::Flatness;
    t.fcf.diameterZone = IsRound(c);
  }
  DeriveTolerance(state);
  t.pos = AutoNotePosition(state, CandidateNote(state));
}

void TargetDim(AppState& state, uint64_t dimId) {
  auto& t = state.drawingNoteTool;
  const DrawingDim* dim = state.drawing->FindDim(dimId);
  if (dim == nullptr)
    return;
  t.dimId = dimId;
  t.ref = ProjRef{};
  t.targeted = true;
  const drawing::SheetCurve* c =
      dim->refs.empty() ? nullptr : CurveOfRef(state, dim->refs.front());
  t.featureMM = c != nullptr ? FeatureSizeMM(state, *c, dim->viewId) : 0.0;
  if (!t.symbolChosen) {
    // A frame under a size is that feature's location: a bore's in a Ø
    // zone.
    const bool round = dim->kind == DrawingDimKind::Diameter ||
                       dim->kind == DrawingDimKind::Radius ||
                       dim->kind == DrawingDimKind::HoleCallout ||
                       dim->kind == DrawingDimKind::PatternCallout;
    t.fcf.symbol = GeoSymbol::Position;
    t.fcf.diameterZone = round;
  }
  DeriveTolerance(state);
}

// The dimension whose value is under `cursor`, or 0.
uint64_t DimUnderCursor(AppState& state, glm::dvec2 cursor, double tol) {
  for (const DrawingDim& dim : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dim);
    if (!rd.resolved)
      continue;
    glm::dvec2 mn, mx;
    drawing::DimLabelBox(rd, mn, mx);
    if (cursor.x >= mn.x - tol && cursor.x <= mx.x + tol &&
        cursor.y >= mn.y - tol && cursor.y <= mx.y + tol)
      return dim.id;
  }
  return 0;
}

void HandleNoteTool(ImDrawList* dl, AppState& state) {
  auto& t = state.drawingNoteTool;
  ImGuiIO& io = ImGui::GetIO();
  const glm::dvec2 cursor = ScreenToSheet(state, io.MousePos);
  const double tol = PxToMM(state, 6.0);

  // The symbol as it would be added, in the accent colour, and what it is
  // on.
  drawing::ResolvedNote rn;
  if (t.targeted) {
    if (t.dimId == 0)
      if (const drawing::SheetCurve* c = CurveOfRef(state, t.ref))
        HighlightCurve(dl, state, *c);
    rn = ResolveDrawingNote(state, CandidateNote(state));
    DrawSheetGraphics(dl, state, drawing::NoteGraphics(rn), kAccentInk);
  }
  if (t.dragging) {
    t.pos = cursor - t.grab;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
      t.dragging = false;
    return;
  }
  if (io.WantCaptureMouse || state.sheetView.panning)
    return;

  // A press on the symbol drags it (a frame under a dimension has no place
  // of its own).
  if (t.targeted && t.dimId == 0) {
    glm::dvec2 mn, mx;
    drawing::NoteBox(rn, mn, mx);
    if (cursor.x >= mn.x - tol && cursor.x <= mx.x + tol &&
        cursor.y >= mn.y - tol && cursor.y <= mx.y + tol) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      if (PressedOnSheet(state)) {
        t.dragging = true;
        t.grab = cursor - t.pos;
      }
      return;
    }
  }
  // A frame may stand under a dimension's value.
  if (t.kind == NoteKind::Fcf)
    if (const uint64_t dimId = DimUnderCursor(state, cursor, tol)) {
      const DrawingDim* dim = state.drawing->FindDim(dimId);
      glm::dvec2 mn, mx;
      drawing::DimLabelBox(ResolveDrawingDim(state, *dim), mn, mx);
      dl->AddRect(SheetToScreen(state, {mn.x, mx.y}),
                  SheetToScreen(state, {mx.x, mn.y}), kAccentInk, 2.0f, 0,
                  2.0f);
      if (PressedOnSheet(state))
        TargetDim(state, dimId);
      return;
    }
  uint64_t viewId = 0;
  if (const drawing::SheetCurve* hit = PickCurve(state, cursor, tol, viewId)) {
    HighlightCurve(dl, state, *hit);
    if (PressedOnSheet(state))
      TargetCurve(state, *hit, viewId);
  }
}

// ---- the detail session ----------------------------------------------------

// The part of segment a-b inside the circle (c, r), in place; false when
// none of it is.
bool ClipToCircle(glm::dvec2& a, glm::dvec2& b, glm::dvec2 c, double r) {
  const glm::dvec2 d = b - a, f = a - c;
  const double A = glm::dot(d, d);
  if (A < 1e-18)
    return glm::length(f) <= r;
  const double B = 2.0 * glm::dot(f, d);
  const double C = glm::dot(f, f) - r * r;
  const double disc = B * B - 4.0 * A * C;
  if (disc <= 0.0)
    return false;
  const double s = std::sqrt(disc);
  const double t0 = std::max(0.0, (-B - s) / (2.0 * A));
  const double t1 = std::min(1.0, (-B + s) / (2.0 * A));
  if (t0 >= t1)
    return false;
  const glm::dvec2 a0 = a;
  a = a0 + t0 * d;
  b = a0 + t1 * d;
  return true;
}

// Where a world point lands on `view`, sheet mm (DetailCircles' mapping).
glm::dvec2 WorldOnView(AppState& state, const DrawingView& view,
                       const glm::dvec3& p) {
  glm::dvec3 vd, up;
  ViewAxes(view, vd, up);
  return DrawingFoldedPosition(state, view).pos +
         view.scale * drawing::ProjectPoint(p, vd, up);
}

DrawingView CandidateDetail(AppState& state) {
  const auto& t = state.detailTool;
  DrawingView v;
  v.detail.on = true;
  v.detail.parentViewId = t.parentViewId;
  v.detail.center = t.center;
  v.detail.radius = t.radius;
  v.detail.label = t.label;
  // What the parent decides is the parent's (add_drawing_view's rule).
  if (const DrawingView* parent = state.drawing->FindView(t.parentViewId)) {
    v.sourceDocId = parent->sourceDocId;
    v.orientation = parent->orientation;
    v.rotationDeg = parent->rotationDeg;
    v.configuration = parent->configuration;
    v.showHidden = parent->showHidden;
  }
  v.scale = t.scale;
  v.position = t.position;
  v.name = "detail " + t.label;
  return v;
}

// The views a detail can be of: projected, and not a detail themselves.
const DrawingView* ViewUnderCursor(AppState& state, glm::dvec2 cursor) {
  for (const DrawingView& v : state.drawing->GetViews()) {
    if (v.detail.on)
      continue;
    const auto it = state.drawingRenders.find(v.id);
    if (it == state.drawingRenders.end() || !it->second.hasGeometry)
      continue;
    const drawing::SheetView& sv = it->second;
    if (cursor.x >= sv.bboxMin.x - 2.0 && cursor.x <= sv.bboxMax.x + 2.0 &&
        cursor.y >= sv.bboxMin.y - 2.0 && cursor.y <= sv.bboxMax.y + 2.0)
      return &v;
  }
  return nullptr;
}

// The scale, letter and place, once the circle is known -- each only where
// the user has not set it.
void DeriveDetail(AppState& state) {
  auto& t = state.detailTool;
  const DrawingView* parent = state.drawing->FindView(t.parentViewId);
  if (parent == nullptr || !(t.radius > 0.0))
    return;
  if (!t.scaleTyped)
    t.scale = DefaultDetailScale(state, *parent, t.radius);
  if (t.label.empty())
    t.label = NextViewLetter(state);
  if (!t.moved && t.scale > 0.0)
    t.position = AutoDetailPosition(state, CandidateDetail(state));
}

void HandleDetailTool(ImDrawList* dl, AppState& state) {
  auto& t = state.detailTool;
  ImGuiIO& io = ImGui::GetIO();
  const glm::dvec2 cursor = ScreenToSheet(state, io.MousePos);
  const DrawingView* parent =
      t.parentViewId != 0 ? state.drawing->FindView(t.parentViewId) : nullptr;
  const ImU32 thin = IM_COL32(80, 170, 255, 160);

  // The circle on the parent, and the detail as it would draw: the
  // parent's lines inside the circle, enlarged, at its place.
  const bool hasCircle = parent != nullptr && t.radius > 0.0;
  glm::dvec2 centre(0.0);
  if (hasCircle) {
    centre = WorldOnView(state, *parent, t.center);
    SheetCircle(dl, state, centre, t.radius * parent->scale, kAccentInk, 1.5f);
  }
  const bool hasGhost =
      hasCircle && t.drag != DetailDrag::Radius && t.scale > 0.0;
  const double R = t.radius * t.scale;
  if (hasGhost) {
    SheetCircle(dl, state, t.position, R, thin, 1.2f);
    const auto it = state.drawingRenders.find(parent->id);
    if (it != state.drawingRenders.end()) {
      const double k = t.scale / (parent->scale > 0.0 ? parent->scale : 1.0);
      for (const drawing::SheetCurve& c : it->second.curves) {
        if (!c.visible)
          continue;
        for (size_t i = 1; i < c.points.size(); ++i) {
          glm::dvec2 a = t.position + (c.points[i - 1] - centre) * k;
          glm::dvec2 b = t.position + (c.points[i] - centre) * k;
          if (ClipToCircle(a, b, t.position, R))
            dl->AddLine(SheetToScreen(state, a), SheetToScreen(state, b), thin,
                        1.4f);
        }
      }
    }
  }

  if (t.drag == DetailDrag::Radius && parent != nullptr) {
    const ImVec2 at = SheetToScreen(state, t.pressAt);
    const float dx = io.MousePos.x - at.x, dy = io.MousePos.y - at.y;
    if (std::sqrt(dx * dx + dy * dy) > 3.0f)
      t.radiusDragged = true;
    if (t.radiusDragged)
      t.radius = std::max(glm::length(cursor - t.pressAt), PxToMM(state, 3.0)) /
                 (parent->scale > 0.0 ? parent->scale : 1.0);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      t.drag = DetailDrag::None;
      DeriveDetail(state);
    }
    return;
  }
  if (t.drag == DetailDrag::Place) {
    t.position = cursor - t.grab;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
      t.drag = DetailDrag::None;
    return;
  }
  if (io.WantCaptureMouse || state.sheetView.panning)
    return;

  // A press on the detail moves it...
  if (hasGhost && glm::length(cursor - t.position) <= R) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (PressedOnSheet(state)) {
      t.drag = DetailDrag::Place;
      t.grab = cursor - t.position;
      t.moved = true;
    }
    return;
  }
  // ...and a press on a view is the circle's centre, its drag the radius.
  if (const DrawingView* over = ViewUnderCursor(state, cursor)) {
    const drawing::SheetView& sv = state.drawingRenders.at(over->id);
    dl->AddRect(SheetToScreen(state, {sv.bboxMin.x - 2.0, sv.bboxMax.y + 2.0}),
                SheetToScreen(state, {sv.bboxMax.x + 2.0, sv.bboxMin.y - 2.0}),
                thin, 0.0f, 0, 1.5f);
    if (PressedOnSheet(state)) {
      t.parentViewId = over->id;
      t.pressAt = cursor;
      t.center = SheetPointToWorld(state, *over, cursor);
      // A click with no drag takes an eighth of the view, in model mm.
      const glm::dvec2 ext = sv.bboxMax - sv.bboxMin;
      t.radius = std::min(ext.x, ext.y) / 8.0 /
                 (over->scale > 0.0 ? over->scale : 1.0);
      t.radiusDragged = false;
      t.drag = DetailDrag::Radius;
    }
  }
}

// Accept and Cancel, with Enter and Escape when no field is being typed in
// (the preview window's rule: the key that ends a field does not also end
// the session).
void AcceptCancel(bool blocking, bool& accept, bool& cancel) {
  ImGui::BeginDisabled(blocking);
  ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentColorHovered());
  if (ImGui::Button(ICON_FA_CHECK " Accept###tw_accept"))
    accept = true;
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_XMARK " Cancel###tw_cancel"))
    cancel = true;
  static int idleFrames = 0;
  idleFrames = ImGui::IsAnyItemActive() ? 0 : idleFrames + 1;
  if (idleFrames > 1) {
    if (!blocking && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
      accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      cancel = true;
  }
}

// A line of help, dim and wrapped: the window is as wide as its fields,
// not as its longest sentence.
void Hint(const char* text) {
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
  ImGui::TextDisabled("%s", text);
  ImGui::PopTextWrapPos();
}

void WrappedLine(const ImVec4& colour, const std::string& text) {
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
  ImGui::TextColored(colour, ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
  ImGui::PopTextWrapPos();
}

} // namespace

void EndDrawingTools(AppState& state) {
  state.dimTool = AppState::DimTool{};
  state.drawingNoteTool = AppState::DrawingNoteTool{};
  state.detailTool = AppState::DetailTool{};
}

void ArmDrawingNoteTool(AppState& state, AppState::DrawingNoteTool::Kind kind) {
  EndDrawingTools(state);
  if (!state.drawing)
    return;
  auto& t = state.drawingNoteTool;
  t.kind = kind;
  if (kind == NoteKind::Datum)
    t.letter = NextDatumLetter(state);
  else
    t.fcf.symbol = GeoSymbol::Flatness;
  state.drawingMessage.clear();
}

void BeginFcfOnDimension(AppState& state, uint64_t dimId) {
  ArmDrawingNoteTool(state, NoteKind::Fcf);
  TargetDim(state, dimId);
}

void ArmDetailTool(AppState& state, uint64_t parentViewId) {
  EndDrawingTools(state);
  if (!state.drawing)
    return;
  state.detailTool.active = true;
  state.detailTool.parentViewId = parentViewId;
  state.drawingMessage.clear();
}

std::vector<uint64_t> BalloonViews(AppState& state) {
  std::vector<uint64_t> out;
  if (!state.drawing)
    return out;
  uint64_t source = 0;
  for (const DrawingView& v : state.drawing->GetViews()) {
    if (v.detail.on)
      continue;
    const auto slot = SlotIndexById(state, v.sourceDocId);
    if (!slot || state.parts[*slot].kind != DocKind::Assembly)
      continue;
    if (source == 0)
      source = v.sourceDocId;
    if (v.sourceDocId == source)
      out.push_back(v.id);
  }
  return out;
}

bool LayOutBalloons(AppState& state, const std::vector<uint64_t>& viewIds) {
  EndDrawingTools(state);
  BalloonLayoutReport report;
  std::string error;
  if (!LayoutDrawingBalloons(state, viewIds, report, &error)) {
    state.drawingMessage = "No balloons: " + error;
    state.drawingMessageIsError = true;
    return false;
  }
  std::string msg = "Laid out " + std::to_string(report.placed.size()) +
                    (report.placed.size() == 1 ? " balloon" : " balloons");
  msg += report.crossings == 0
             ? ", no leaders crossing"
             : ", " + std::to_string(report.crossings) + " leaders crossing";
  if (!report.unplaced.empty()) {
    msg += "; no clear edge for";
    for (size_t i = 0; i < report.unplaced.size(); ++i)
      msg += (i == 0 ? " " : ", ") + report.unplaced[i];
  }
  state.drawingMessage = msg;
  state.drawingMessageIsError =
      report.crossings != 0 || !report.unplaced.empty();
  return true;
}

void HandleDrawingAnnotateTools(ImDrawList* dl, AppState& state) {
  if (!state.drawing)
    return;
  if (state.drawingNoteTool.kind != NoteKind::None)
    HandleNoteTool(dl, state);
  else if (state.detailTool.active)
    HandleDetailTool(dl, state);
}

void DrawDrawingNoteToolWindow(AppState& state) {
  auto& t = state.drawingNoteTool;
  if (!state.drawing) {
    t = AppState::DrawingNoteTool{};
    return;
  }
  const bool fcf = t.kind == NoteKind::Fcf;
  BeginToolWindowFrame(state, fcf ? "Feature control frame" : "Datum feature");

  // What it is on.
  if (!t.targeted) {
    Hint(fcf ? "Click the feature's edge on the sheet, or a "
               "dimension's value"
             : "Click the feature's edge on the sheet");
  } else if (t.dimId != 0) {
    const DrawingDim* dim = state.drawing->FindDim(t.dimId);
    const drawing::ResolvedDim rd = dim != nullptr
                                        ? ResolveDrawingDim(state, *dim)
                                        : drawing::ResolvedDim{};
    ImGui::Text("Under %s", rd.resolved ? rd.text.c_str() : "a dimension");
    Hint("Click another edge or value to move it");
  } else {
    ImGui::Text("On %s", t.ref.edge.IsSet() ? "an edge" : "a silhouette");
    Hint(fcf ? "Drag the frame to move it; click another edge "
               "or value to move the leader"
             : "Drag the symbol to move it; click another "
               "edge to move the leader");
  }

  if (fcf) {
    LeftLabel("Symbol");
    if (ImGui::BeginCombo("###fcf_symbol", GeoSymbolToString(t.fcf.symbol))) {
      for (const GeoSymbol s : kSymbols)
        if (ImGui::Selectable(GeoSymbolToString(s), s == t.fcf.symbol)) {
          t.fcf.symbol = s;
          t.symbolChosen = true;
          DeriveTolerance(state);
        }
      ImGui::EndCombo();
    }
    LeftLabel("Tolerance");
    double tolerance = t.fcf.tolerance;
    if (ExprInputDouble(state, "###fcf_tolerance", &tolerance, "%.3f")) {
      t.fcf.tolerance = tolerance;
      t.toleranceTyped = true;
    }
    if (!t.toleranceTyped && t.targeted)
      ImGui::TextDisabled("ISO 2768-K for %.0f mm", t.featureMM);
    ImGui::Checkbox("Ø zone###fcf_diameter", &t.fcf.diameterZone);
    LeftLabel("Modifier");
    if (ImGui::BeginCombo("###fcf_modifier",
                          MaterialModifierToString(t.fcf.modifier))) {
      for (const MaterialModifier m : kModifiers)
        if (ImGui::Selectable(MaterialModifierToString(m), m == t.fcf.modifier))
          t.fcf.modifier = m;
      ImGui::EndCombo();
    }
    // The datums are the sheet's datum features, primary first.
    std::vector<std::string> letters;
    for (const DrawingNote& n : state.drawing->GetNotes())
      if (n.style == NoteStyle::Datum)
        letters.push_back(n.text);
    std::sort(letters.begin(), letters.end());
    if (letters.empty())
      Hint("No datum on this sheet (the Datum tool adds one)");
    static const char* kDatumRows[] = {"Datum", "Secondary", "Tertiary"};
    for (size_t i = 0; i < 3 && !letters.empty() && i <= t.fcf.datums.size();
         ++i) {
      LeftLabel(kDatumRows[i]);
      const std::string id = "###fcf_datum" + std::to_string(i);
      const char* shown =
          i < t.fcf.datums.size() ? t.fcf.datums[i].c_str() : "none";
      if (ImGui::BeginCombo(id.c_str(), shown)) {
        if (ImGui::Selectable("none", i >= t.fcf.datums.size()))
          t.fcf.datums.resize(i);
        for (const std::string& l : letters)
          if (ImGui::Selectable(l.c_str(), i < t.fcf.datums.size() &&
                                               t.fcf.datums[i] == l)) {
            if (i < t.fcf.datums.size())
              t.fcf.datums[i] = l;
            else
              t.fcf.datums.push_back(l);
          }
        ImGui::EndCombo();
      }
    }
  } else {
    LeftLabel("Letter");
    char buf[8];
    std::snprintf(buf, sizeof buf, "%s", t.letter.c_str());
    if (ImGui::InputText("###datum_letter", buf, sizeof buf,
                         ImGuiInputTextFlags_CharsUppercase))
      t.letter = buf;
  }

  // Why it cannot go on the sheet as it stands: the RPC's own answer.
  const std::string why =
      t.targeted ? WhyDrawingNoteRefused(state, CandidateNote(state)) : "";
  if (!t.targeted)
    WrappedLine(kMissing, "Pick the feature to continue");
  else if (!why.empty())
    WrappedLine(kRefusal, why);

  ImGui::Separator();
  bool accept = false, cancel = false;
  AcceptCancel(!t.targeted || !why.empty(), accept, cancel);
  ImGui::End();

  if (accept) {
    DrawingNote n = CandidateNote(state);
    SnapshotDrawing(state);
    state.drawing->AddNote(std::move(n));
    t = AppState::DrawingNoteTool{};
  } else if (cancel) {
    t = AppState::DrawingNoteTool{};
  }
}

void DrawDetailToolWindow(AppState& state) {
  auto& t = state.detailTool;
  if (!state.drawing) {
    t = AppState::DetailTool{};
    return;
  }
  BeginToolWindowFrame(state, "Detail view");
  const DrawingView* parent =
      t.parentViewId != 0 ? state.drawing->FindView(t.parentViewId) : nullptr;
  const bool picked =
      parent != nullptr && t.radius > 0.0 && t.drag != DetailDrag::Radius;
  if (!picked) {
    Hint("Press on a view at the detail's centre and drag out "
         "its radius");
  } else {
    ImGui::Text("Of %s",
                parent->name.empty() ? "a view" : parent->name.c_str());
    Hint("Drag the detail to move it; press on a view again "
         "for another circle");
    LeftLabel("Radius");
    double radius = t.radius;
    if (ExprInputDouble(state, "###detail_radius", &radius, "%.2f") &&
        radius > 0.0) {
      t.radius = radius;
      DeriveDetail(state);
    }
    LeftLabel("Scale");
    double scale = t.scale;
    if (ExprInputDouble(state, "###detail_scale", &scale, "%.3g")) {
      t.scale = scale;
      t.scaleTyped = true;
      DeriveDetail(state);
    }
    LeftLabel("Letter");
    char buf[8];
    std::snprintf(buf, sizeof buf, "%s", t.label.c_str());
    if (ImGui::InputText("###detail_label", buf, sizeof buf,
                         ImGuiInputTextFlags_CharsUppercase))
      t.label = buf;
  }

  std::string why;
  if (picked) {
    const DrawingView v = CandidateDetail(state);
    why = WhyDetailRefused(state, v.detail);
    if (why.empty() && !(v.scale > 0.0))
      why = "scale must be positive";
  }
  if (!picked)
    WrappedLine(kMissing, "Pick the detail's centre to continue");
  else if (!why.empty())
    WrappedLine(kRefusal, why);

  ImGui::Separator();
  bool accept = false, cancel = false;
  AcceptCancel(!picked || !why.empty(), accept, cancel);
  ImGui::End();

  if (accept) {
    const DrawingView v = CandidateDetail(state);
    SnapshotDrawing(state);
    state.drawing->AddView(v);
    t = AppState::DetailTool{};
    UpdateDrawingRenders(state);
  } else if (cancel) {
    t = AppState::DetailTool{};
  }
}

} // namespace ui
