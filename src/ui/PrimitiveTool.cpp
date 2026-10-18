// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PrimitiveTool.h"

#include "core/Document.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "ui/ExprComplete.h"   // ExprCompletingInput (a dimension's fx text)
#include "ui/ExprInput.h"      // ExprDragDouble
#include "ui/Overlays.h"       // ViewportDrawList
#include "ui/PanelLayout.h"    // LeftLabel / SmallButtonReserve
#include "ui/PreviewFeature.h" // BeginPreviewFeature
#include "ui/SceneOverlays.h"  // ScenePlaneHalfExtent
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchInteractionInternal.h" // ScreenRay, RayToPlaneUV, PixelsPerUV
#include "ui/TemplateActions.h"           // BeginTemplatePreview

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui {

namespace {

using primitive::Kind;

const char* KindLabel(Kind kind) { return primitive::KindName(kind); }

// The size a primitive takes on an empty part: half the reach the base
// planes are drawn to, so the first block sits well inside the plane you
// just clicked rather than the size of a literal.
double FallbackSize(AppState& state) {
  return 0.5 * static_cast<double>(ScenePlaneHalfExtent(state));
}

// Half of the primitive's footprint along u and v, and its centre, in the
// plane's frame. A Box's anchor is its centre or (centered = false) its low
// corner; a Cylinder's and a Sphere's is always the centre.
glm::dvec2 HalfSize(const primitive::Spec& s) {
  if (s.kind == Kind::Box)
    return glm::dvec2(s.width.value, s.depth.value) * 0.5;
  return glm::dvec2(s.diameter.value * 0.5);
}
glm::dvec2 CentreOf(const primitive::Spec& s) {
  const glm::dvec2 anchor(s.offsetU.value, s.offsetV.value);
  if (s.kind == Kind::Box && !s.centered)
    return anchor + HalfSize(s);
  return anchor;
}
glm::dvec2 AnchorFor(const primitive::Spec& s, glm::dvec2 centre) {
  if (s.kind == Kind::Box && !s.centered)
    return centre - HalfSize(s);
  return centre;
}

struct Resolved {
  SketchFeature* sketch = nullptr;
  Feature* solid = nullptr;
};
Resolved ResolveSession(AppState& state) {
  Resolved r;
  if (!PrimitiveSessionActive(state))
    return r;
  r.solid = state.document->FindFeatureById(state.primitive.solidId);
  r.sketch = dynamic_cast<SketchFeature*>(
      state.document->FindFeatureById(state.primitive.sketchId));
  if (r.solid == nullptr || r.sketch == nullptr)
    r = {};
  return r;
}

// Rewrites the preview from the session's spec: every expression evaluated
// first, so a field that does not evaluate (or a size that is not positive)
// leaves the preview as it was and says why in the window.
void ApplySpec(AppState& state) {
  const Resolved r = ResolveSession(state);
  if (r.solid == nullptr)
    return;
  primitive::Spec evaluated = state.primitive.spec;
  state.primitive.error =
      primitive::EvaluateDims(evaluated, state.document->Symbols());
  if (!state.primitive.error.empty())
    return;
  state.primitive.spec = evaluated;
  primitive::Refill(*r.sketch, *r.solid, state.primitive.spec);
  state.document->Regenerate();
}

// A dimension row: the drag field for a literal, the expression text for a
// driven one (with "123" back to a literal). Returns true on a change.
bool DimRow(AppState& state, const char* label, primitive::Dim& dim,
            bool positive) {
  bool changed = false;
  ImGui::PushID(label);
  const std::string id = std::string("##") + label;
  if (dim.expr.empty()) {
    LeftLabel(label);
    std::string typed;
    // The step scales with the value, so a 2 m beam and a 2 mm pin both
    // drag at a usable rate.
    const float speed =
        static_cast<float>(std::max(1e-3, 0.005 * std::abs(dim.value)));
    if (ExprDragDouble(state, id.c_str(), &dim.value, speed,
                       positive ? 1e-6 : -1e9, 1e9, "%.3f", &typed)) {
      if (!typed.empty())
        dim.expr = typed; // the text is the point: it stays a link
      changed = true;
    }
  } else {
    static std::string activeField;
    static char buf[256];
    const bool editing = activeField == label;
    char tmp[sizeof(buf)];
    std::snprintf(tmp, sizeof(tmp), "%s", dim.expr.c_str());
    LeftLabel(label, SmallButtonReserve("123"));
    const ExprFieldEvents ev =
        ExprCompletingInput(state, "##expr", editing ? buf : tmp, sizeof(buf));
    if (ev.activated) {
      activeField = label;
      std::snprintf(buf, sizeof(buf), "%s", dim.expr.c_str());
    }
    if (editing && ev.left) {
      activeField.clear();
      char* end = nullptr;
      const double number = std::strtod(buf, &end);
      while (end != nullptr && *end == ' ')
        end++;
      if (end != buf && end != nullptr && *end == '\0') {
        dim.expr.clear(); // a plain number is a literal again
        dim.value = number;
      } else {
        dim.expr = buf;
      }
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("123")) {
      dim.expr.clear(); // keeps the last evaluated value
      changed = true;
    }
    ImGui::TextDisabled("= %.4g", dim.value);
  }
  if (changed)
    dim.set = true;
  ImGui::PopID();
  return changed;
}

std::string OnLabel(AppState& state, const primitive::On& on) {
  switch (on.kind) {
  case primitive::OnKind::WorldPlane:
    return SketchPlane::StandardPlaneName(on.world);
  case primitive::OnKind::Face: {
    // Which feature's face, not its element name: that is a topological
    // address ("2/g1;M78.1;...;M276") no user can read, and as long as it
    // gets it stretched the tool window across the screen.
    const Feature* f = state.document->FindFeatureById(on.face.featureId);
    return "a face of " +
           (f != nullptr ? f->GetName() : std::string("(missing)"));
  }
  case primitive::OnKind::PlaneFeature: {
    const Feature* f = state.document->FindFeatureById(on.featureId);
    return f != nullptr ? f->GetName() : std::string("(missing)");
  }
  }
  return "";
}

// Is the ghost under the ray? Its footprint on the plane it stands on, and
// (a Box or a Cylinder) on the plane of its far cap, so it can be grabbed
// from either side. Not a mesh pick: in Add or Cut mode the ghost is merged
// into the body it changes and has no mesh of its own.
bool GhostUnder(AppState& state, const Ray& ray) {
  const primitive::Spec& s = state.primitive.spec;
  const SketchPlane& plane = state.primitive.placement.plane;
  const glm::dvec2 c = CentreOf(s);
  const glm::dvec2 half = HalfSize(s);
  const auto inside = [&](const SketchPlane& p) {
    const auto uv = RayToPlaneUV(ray, p);
    if (!uv)
      return false;
    const glm::dvec2 d = *uv - c;
    if (s.kind == Kind::Box)
      return std::abs(d.x) <= half.x && std::abs(d.y) <= half.y;
    return glm::length(d) <= half.x;
  };
  if (inside(plane))
    return true;
  if (s.kind == Kind::Sphere)
    return false;
  const double h = s.reversed ? -s.height.value : s.height.value;
  const SketchPlane cap(plane.GetOrigin() + plane.GetNormal() * h,
                        plane.GetNormal(), plane.GetUAxis());
  return inside(cap);
}

} // namespace

AxisSnap SnapPrimitiveAxis(double centre, double half, bool hasExtent,
                           double lo, double hi, double capture) {
  AxisSnap out{centre, ""};
  if (!(capture > 0.0))
    return out;
  struct Candidate {
    double at;
    const char* what;
  };
  std::vector<Candidate> candidates = {{0.0, "centre"}};
  if (hasExtent) {
    candidates.push_back({lo + half, "flush"});
    candidates.push_back({hi - half, "flush"});
    candidates.push_back({lo, "on edge"});
    candidates.push_back({hi, "on edge"});
  }
  double best = capture;
  for (const Candidate& c : candidates) {
    const double d = std::abs(centre - c.at);
    if (d < best) {
      best = d;
      out = {c.at, c.what};
    }
  }
  return out;
}

bool PrimitiveSessionActive(const AppState& state) {
  return state.document != nullptr && state.primitive.solidId != 0 &&
         state.primitive.solidId == state.pendingFeatureId;
}

std::string BeginPrimitiveOn(AppState& state, const primitive::On& on) {
  if (!state.document)
    return "open a part first";
  Document& doc = *state.document;
  const Kind kind = state.primitive.kind;
  primitive::Spec spec;
  spec.kind = kind;
  spec.on = on;
  spec.name = std::string(KindLabel(kind)) + " " +
              std::to_string(doc.FeatureCount() + 1);
  const auto placement = primitive::Resolve(doc, on);
  if (!placement)
    return std::string(KindLabel(kind)) + ": " + placement.error();
  if (std::string why =
          primitive::ApplyDefaults(spec, *placement, FallbackSize(state));
      !why.empty())
    return std::string(KindLabel(kind)) + ": " + why;
  if (std::string why = primitive::EvaluateDims(spec, primitive::Symbols(doc));
      !why.empty())
    return std::string(KindLabel(kind)) + ": " + why;
  // The defaults are now the spec's own numbers: the fields show them, and
  // a later edit of one does not re-derive the others.
  for (primitive::Dim* d : {&spec.width, &spec.depth, &spec.height,
                            &spec.diameter, &spec.offsetU, &spec.offsetV})
    d->set = true;

  if (HasPendingFeature(state))
    CancelPendingFeature(state);
  primitive::Expansion ex = primitive::Expand(spec, *placement);
  state.sketchEdit = SketchEditState{}; // the pick is answered
  // The picked face is the REFERENCE, not a slot value: it must not seed
  // the Extrude's slots the way a select-first pick would.
  state.selection.Clear();
  doc.AddFeature(std::move(ex.sketch));
  const uint64_t sketchId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  primitive::BindSketch(*ex.solid, sketchId);
  const uint64_t solidId = BeginPreviewFeature(state, std::move(ex.solid));
  state.pendingCompanionId = sketchId;

  AppState::PrimitiveSession& session = state.primitive;
  session.kind = kind;
  session.sketchId = sketchId;
  session.solidId = solidId;
  session.spec = spec;
  session.placement = *placement;
  session.error.clear();
  session.dragging = false;
  session.snapU.clear();
  session.snapV.clear();
  return "";
}

std::string BeginPrimitiveOnFace(AppState& state, uint32_t meshId,
                                 uint32_t faceIndex) {
  const std::string kind = KindLabel(state.primitive.kind);
  // The B-Rep's answer, not the tessellation's averaged normal (which would
  // happily stand a box on a tangent plane of a cylinder).
  glm::dvec3 origin, normal, uAxis;
  if (!ExactFacePlane(state, meshId, faceIndex, origin, normal, uAxis))
    return kind + ": that face is curved - pick a flat one.";
  const auto ref = FaceElementRef(state, meshId, faceIndex);
  if (!ref)
    return kind + ": that face has no stable name to stand on - pick a "
                  "plane, or a face of a feature that names its faces.";
  primitive::On on;
  on.kind = primitive::OnKind::Face;
  on.face = *ref;
  return BeginPrimitiveOn(state, on);
}

std::string BeginPlacedOn(AppState& state, const primitive::On& on) {
  if (state.sketchEdit.purpose != SketchPurpose::Template)
    return BeginPrimitiveOn(state, on);
  if (!state.document)
    return "open a part first";
  // A template stands where a primitive does: checked the same way.
  if (const auto r = primitive::Resolve(*state.document, on); !r)
    return "Insert template: " + r.error();
  BeginTemplatePreview(state, primitive::OnToJson(on));
  // A refusal leaves the pick armed, saying why.
  return state.sketchEdit.pickingPlane ? state.sketchEdit.actionMsg
                                       : std::string{};
}

std::string BeginPlacedOnFace(AppState& state, uint32_t meshId,
                              uint32_t faceIndex) {
  if (state.sketchEdit.purpose != SketchPurpose::Template)
    return BeginPrimitiveOnFace(state, meshId, faceIndex);
  glm::dvec3 origin, normal, uAxis;
  if (!ExactFacePlane(state, meshId, faceIndex, origin, normal, uAxis))
    return "Insert template: that face is curved - pick a flat one.";
  const auto ref = FaceElementRef(state, meshId, faceIndex);
  if (!ref)
    return "Insert template: that face has no stable name to stand on - "
           "pick a plane, or a face of a feature that names its faces.";
  primitive::On on;
  on.kind = primitive::OnKind::Face;
  on.face = *ref;
  return BeginPlacedOn(state, on);
}

void StartPrimitiveTool(AppState& state, primitive::Kind kind) {
  if (!state.document)
    return;
  state.primitive = AppState::PrimitiveSession{};
  state.primitive.kind = kind;
  // Select-first: a planar face or a datum plane already picked IS the
  // reference.
  std::string refusal;
  if (const SelectionItem* item =
          state.selection.First(SelectionItem::Kind::Face)) {
    uint32_t meshId = 0, faceIndex = 0;
    if (ResolveFaceItem(state, *item, meshId, faceIndex)) {
      refusal = BeginPrimitiveOnFace(state, meshId, faceIndex);
      if (refusal.empty())
        return;
    }
  } else if (auto* datum =
                 dynamic_cast<DatumPlaneFeature*>(SelectedFeature(state))) {
    primitive::On on;
    on.kind = primitive::OnKind::PlaneFeature;
    on.featureId = datum->GetId();
    refusal = BeginPrimitiveOn(state, on);
    if (refusal.empty())
      return;
  }
  state.sketchEdit = SketchEditState{}; // reset FIRST, then arm
  state.sketchEdit.pickingPlane = true;
  state.sketchEdit.purpose = SketchPurpose::Primitive;
  state.sketchEdit.actionMsg = refusal;
}

void DrawPrimitiveParamWidgets(AppState& state) {
  const Resolved r = ResolveSession(state);
  if (r.solid == nullptr)
    return;
  primitive::Spec& s = state.primitive.spec;

  // The Extrude's own arrow handle drags the height straight into the
  // feature; take it back into the spec before anything rewrites from it.
  if (auto* ext = dynamic_cast<ExtrudeFeature*>(r.solid);
      ext != nullptr && s.height.expr.empty() &&
      std::abs(ext->distance - s.height.value) > 1e-12)
    s.height.value = ext->distance;

  ImGui::TextDisabled("On %s", OnLabel(state, s.on).c_str());
  bool changed = false;
  switch (s.kind) {
  case Kind::Box:
    changed |= DimRow(state, "Width", s.width, true);
    changed |= DimRow(state, "Depth", s.depth, true);
    changed |= DimRow(state, "Height", s.height, true);
    break;
  case Kind::Cylinder:
    changed |= DimRow(state, "Diameter", s.diameter, true);
    changed |= DimRow(state, "Height", s.height, true);
    break;
  case Kind::Sphere:
    changed |= DimRow(state, "Diameter", s.diameter, true);
    break;
  }
  changed |= DimRow(state, "Offset U", s.offsetU, false);
  changed |= DimRow(state, "Offset V", s.offsetV, false);

  if (s.kind == Kind::Box) {
    bool centered = s.centered;
    if (ImGui::Checkbox("Centered###prim_centered", &centered)) {
      // The box stays where it is: only what the offset measures changes
      // (its centre, or its low corner).
      const glm::dvec2 centre = CentreOf(s);
      s.centered = centered;
      const glm::dvec2 anchor = AnchorFor(s, centre);
      if (s.offsetU.expr.empty())
        s.offsetU.value = anchor.x;
      if (s.offsetV.expr.empty())
        s.offsetV.value = anchor.y;
      changed = true;
    }
    ImGui::SetItemTooltip("The offset places the rectangle's centre; off, "
                          "its low corner");
  }

  static const char* const kModes[] = {"New body", "Add", "Cut"};
  int mode = static_cast<int>(s.mode);
  LeftLabel("Mode");
  if (ImGui::Combo("###Mode", &mode, kModes, 3)) {
    s.mode = static_cast<ExtrudeMode>(mode);
    changed = true;
  }
  if (s.kind != Kind::Sphere) {
    if (ImGui::Checkbox("Reversed###prim_reversed", &s.reversed))
      changed = true;
    ImGui::SetItemTooltip("Grow against the reference's normal (into the "
                          "face, for a pocket)");
  }
  ImGui::TextDisabled("Drag it to move it on the %s; it snaps to the "
                      "centre%s (Ctrl: free).",
                      s.on.kind == primitive::OnKind::Face ? "face" : "plane",
                      state.primitive.placement.hasExtent ? " and the edges"
                                                          : "");
  if (changed)
    ApplySpec(state);
  if (!state.primitive.error.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.90f, 0.40f, 0.40f, 1.0f), "%s",
                       state.primitive.error.c_str());
    ImGui::PopTextWrapPos();
  }
}

bool HandlePrimitiveDragInput(AppState& state, bool overViewport) {
  AppState::PrimitiveSession& session = state.primitive;
  if (!PrimitiveSessionActive(state)) {
    session.dragging = false;
    return false;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const bool clicked = overViewport && !io.KeyAlt &&
                       ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (!session.dragging && !clicked)
    return false;
  if (!state.faceSelector || !state.camera)
    return false;
  const Ray ray = ScreenRay(state, glm::vec2(io.MousePos.x, io.MousePos.y));
  const SketchPlane& plane = session.placement.plane;
  const auto uv = RayToPlaneUV(ray, plane);
  primitive::Spec& s = session.spec;

  if (!session.dragging) {
    if (!uv || !GhostUnder(state, ray))
      return false; // a miss falls through to picking
    session.dragging = true;
    session.grabUV = *uv;
    session.startOffset = glm::dvec2(s.offsetU.value, s.offsetV.value);
    session.snapU.clear();
    session.snapV.clear();
    return true;
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
      !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    session.dragging = false;
    return true;
  }
  if (!uv)
    return true; // the plane is edge-on: hold where it is

  // Where the centre would be, then each axis snapped on its own: one can
  // sit flush with an edge while the other is still free.
  primitive::Spec moved = s;
  moved.offsetU.value = session.startOffset.x + (uv->x - session.grabUV.x);
  moved.offsetV.value = session.startOffset.y + (uv->y - session.grabUV.y);
  const glm::dvec2 centre = CentreOf(moved);
  const glm::dvec2 half = HalfSize(moved);
  // A capture in screen pixels, converted at the cursor: the same feel at
  // any zoom and on any size of part.
  const float ppu = PixelsPerUV(state, plane, *uv);
  const double capture =
      io.KeyCtrl || !(ppu > 0.0f) ? 0.0 : 10.0 / static_cast<double>(ppu);
  const primitive::Placement& p = session.placement;
  const AxisSnap su =
      SnapPrimitiveAxis(centre.x, half.x, p.hasExtent, p.lo.x, p.hi.x, capture);
  const AxisSnap sv =
      SnapPrimitiveAxis(centre.y, half.y, p.hasExtent, p.lo.y, p.hi.y, capture);
  session.snapU = su.what;
  session.snapV = sv.what;
  const glm::dvec2 anchor = AnchorFor(moved, glm::dvec2(su.value, sv.value));
  if (anchor.x == s.offsetU.value && anchor.y == s.offsetV.value &&
      s.offsetU.expr.empty() && s.offsetV.expr.empty())
    return true;
  // A dragged offset is a number, not a formula: a drag over an expression
  // replaces it, as the force-arrow drag does.
  s.offsetU = {anchor.x, "", true};
  s.offsetV = {anchor.y, "", true};
  ApplySpec(state);
  return true;
}

void AbortPrimitiveDrag(AppState& state) {
  AppState::PrimitiveSession& session = state.primitive;
  if (!session.dragging)
    return;
  session.dragging = false;
  session.snapU.clear();
  session.snapV.clear();
  session.spec.offsetU.value = session.startOffset.x;
  session.spec.offsetV.value = session.startOffset.y;
  ApplySpec(state);
}

void DrawPrimitiveSnapCue(AppState& state) {
  const AppState::PrimitiveSession& session = state.primitive;
  if (!PrimitiveSessionActive(state) || !session.dragging)
    return;
  const SketchPlane& plane = session.placement.plane;
  ImDrawList* dl = ViewportDrawList();
  const ImU32 accent = IM_COL32(255, 220, 120, 255);
  // The reference's centre, always: the target the drag is measured from.
  const glm::vec2 o = SketchUVToScreen(state, plane, glm::dvec2(0.0));
  dl->AddLine(ImVec2(o.x - 6, o.y), ImVec2(o.x + 6, o.y), accent, 1.5f);
  dl->AddLine(ImVec2(o.x, o.y - 6), ImVec2(o.x, o.y + 6), accent, 1.5f);
  if (session.snapU.empty() && session.snapV.empty())
    return;
  const glm::vec2 p = SketchUVToScreen(state, plane, CentreOf(session.spec));
  dl->AddCircle(ImVec2(p.x, p.y), 7.0f, accent, 0, 2.0f); // "grabbed" ring
  std::string label;
  if (session.snapU == session.snapV)
    label = session.snapU;
  else
    label = (session.snapU.empty() ? std::string("free") : session.snapU) +
            " / " +
            (session.snapV.empty() ? std::string("free") : session.snapV);
  const ImVec2 sz = ImGui::CalcTextSize(label.c_str());
  const float gx = p.x + 12.0f, gy = p.y - 24.0f;
  dl->AddRectFilled(ImVec2(gx - 3, gy - 1),
                    ImVec2(gx + sz.x + 3, gy + sz.y + 1),
                    IM_COL32(30, 30, 30, 200), 3.0f);
  dl->AddText(ImVec2(gx, gy), accent, label.c_str());
}

} // namespace ui
