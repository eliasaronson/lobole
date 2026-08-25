#include "ui/MeasureOverlay.h"

#include "render/Camera.h"
#include "ui/AppActions.h"        // AppendPrimLine / AppendPrimDot
#include "ui/SketchInteraction.h" // WorldToScreen

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

// One colour for measurement, distinct from the selection blue and from the
// orange face highlight. Measured elements used to reuse the ordinary
// selection visuals, which made a measured face and a merely selected one
// identical — and measured FACES orange while measured EDGES were blue, so
// the two halves of one measurement did not even match each other.
constexpr glm::vec4 kMeasure{1.0f, 0.72f, 0.22f, 1.0f};
constexpr glm::vec4 kWitness{1.0f, 0.72f, 0.22f, 0.45f};

// Roughly-constant on-screen sizes. The prim pipeline expands THICKNESS in
// screen space but positions are world, so anything with a world length —
// an arrowhead, an arc radius — has to be scaled by how far away we are or
// it is a speck on a dock and a wall on a bolt.
float WorldPerPixelish(const AppState& state) {
  return state.camera ? state.camera->GetDistance() * 0.0016f : 0.01f;
}

// An arrowhead at `tip` pointing along `dir`, drawn in the plane facing the
// camera so it reads as an arrow from wherever you are orbiting.
void AppendArrowhead(const AppState& state, const glm::vec3& tip,
                     const glm::vec3& dir, float size,
                     std::vector<PrimVertex>& out) {
  const glm::vec3 view =
      state.camera ? state.camera->GetForward() : glm::vec3(0, 0, -1);
  glm::vec3 side = glm::cross(dir, view);
  if (glm::length(side) < 1e-6f)
    side = glm::cross(dir, glm::vec3(0, 1, 0));
  if (glm::length(side) < 1e-6f)
    return;
  side = glm::normalize(side) * (size * 0.36f);
  const glm::vec3 back = tip - dir * size;
  AppendPrimLine(tip, back + side, 1.6f, kMeasure, out);
  AppendPrimLine(tip, back - side, 1.6f, kMeasure, out);
}

// An arc from `from` to `to` about `at`, both unit directions, sampled fine
// enough that it reads as a curve rather than a chord.
void AppendArc(const glm::vec3& at, const glm::vec3& from, const glm::vec3& to,
               float radius, std::vector<PrimVertex>& out) {
  const float dot = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
  const float sweep = std::acos(dot);
  if (sweep < 1e-4f)
    return;
  // Gram-Schmidt: an in-plane axis perpendicular to `from`, so the arc lies
  // in the plane the two directions span. Degenerate only when they are
  // anti-parallel, which is the parallel case the caller already skips.
  glm::vec3 perp = to - from * dot;
  if (glm::length(perp) < 1e-6f)
    return;
  perp = glm::normalize(perp);
  const int steps = std::clamp(static_cast<int>(sweep * 24.0f), 6, 48);
  glm::vec3 prev = at + from * radius;
  for (int i = 1; i <= steps; i++) {
    const float t = sweep * static_cast<float>(i) / static_cast<float>(steps);
    const glm::vec3 p = at + (from * std::cos(t) + perp * std::sin(t)) * radius;
    AppendPrimLine(prev, p, 1.8f, kMeasure, out);
    prev = p;
  }
  // Short rays out along each direction, so the arc is visibly BETWEEN two
  // things rather than floating near a corner.
  AppendPrimLine(at, at + from * (radius * 1.25f), 1.2f, kWitness, out);
  AppendPrimLine(at, at + to * (radius * 1.25f), 1.2f, kWitness, out);
}

// A pill of text at a world point, matching the CAM depth label's styling so
// the viewport has one voice for "a number about this geometry".
void DrawWorldLabel(AppState& state, const glm::vec3& world,
                    const std::string& text) {
  if (text.empty())
    return;
  const glm::vec2 at = WorldToScreen(state, world);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  // Centred on the point, then nudged up so the line it annotates is not
  // drawn through the text.
  const ImVec2 pos(at.x - size.x * 0.5f, at.y - size.y - 6.0f);
  dl->AddRectFilled(ImVec2(pos.x - 5.0f, pos.y - 3.0f),
                    ImVec2(pos.x + size.x + 5.0f, pos.y + size.y + 3.0f),
                    IM_COL32(30, 24, 16, 225), 4.0f);
  dl->AddText(pos, IM_COL32(255, 200, 110, 255), text.c_str());
}

} // namespace

void AppendMeasureOverlay(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.measure.active)
    return;
  const MeasureState::Dimension& dim = state.measure.dimension;
  const float s = WorldPerPixelish(state);

  if (dim.valid) {
    const glm::vec3 span = dim.b - dim.a;
    const float len = glm::length(span);
    if (len > 1e-6f) {
      const glm::vec3 dir = span / len;
      // Arrowheads eat into the line only when there is room for them;
      // below that the standard drafting fallback is to point inward from
      // outside, which for a 3D overlay reads better as just the line.
      const float head = std::min(s * 9.0f, len * 0.25f);
      AppendPrimLine(dim.a, dim.b, 2.2f, kMeasure, out);
      if (head > s) {
        AppendArrowhead(state, dim.a, -dir, head, out);
        AppendArrowhead(state, dim.b, dir, head, out);
      }
      // Witness lines back to where the minimum actually is, when the
      // dimension was re-anchored away from it (the parallel case).
      if (dim.hasWitness) {
        AppendPrimLine(dim.a, dim.witnessA, 1.2f, kWitness, out);
        AppendPrimLine(dim.b, dim.witnessB, 1.2f, kWitness, out);
      }
      AppendPrimDot(dim.a, 3.0f, kMeasure, out);
      AppendPrimDot(dim.b, 3.0f, kMeasure, out);
    }
  }

  if (dim.hasArc)
    AppendArc(dim.arcAt, dim.arcFrom, dim.arcTo, s * 34.0f, out);
}

void DrawMeasureLabels(AppState& state) {
  if (!state.measure.active)
    return;
  const MeasureState::Dimension& dim = state.measure.dimension;
  if (dim.valid)
    DrawWorldLabel(state, (dim.a + dim.b) * 0.5f, dim.label);
  if (dim.hasArc) {
    // On the arc's midpoint, pushed out past it so the label clears the
    // curve and the two rays.
    const glm::vec3 mid = glm::normalize(dim.arcFrom + dim.arcTo);
    const float r = WorldPerPixelish(state) * 34.0f;
    DrawWorldLabel(state, dim.arcAt + mid * (r * 1.45f), dim.arcLabel);
  }
}

} // namespace ui
