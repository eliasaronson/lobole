#pragma once

#include "ui/AppState.h"

#include <string>

// Foreground-draw-list overlays: text and pills painted over the viewport,
// as opposed to the LINE GEOMETRY the renderers draw (that stays in
// AppActions.cpp, which builds vertex buffers and needs no ImGui).
//
// They live here rather than in AppActions.cpp because they were its only
// ImGui dependency — three functions in a 4,700-line file that is otherwise
// pure document logic. See docs/architecture-review-2026-08-24.md; separating
// them is the first step toward that logic being unit-testable.
namespace ui {

enum class PromptPill {
  StatusGood, // green: "Fully defined"
  Status,     // blue: DOF count etc.
  Prompt,     // neutral: what the armed tool expects
  Warning     // amber: actionMsg feedback
};

// One centred pill under the previous one; `y` advances past what it drew, so
// callers stack pills by threading the same y.
// Where a prompt pill of `textSize` at height `y` lands: {min.x, min.y,
// max.x, max.y}. Pure, and separated from the drawing so the one rule that
// matters — it must not run underneath the floating tool window, which
// anchors top-right of the same viewport at the same height — can be pinned
// without a GPU. See tests/test_actions.cpp.
glm::vec4 PromptPillRect(const AppState& state, float y, glm::vec2 textSize);

void DrawPromptPill(AppState& state, float& y, const std::string& text,
                    PromptPill style);

// Names beside the datum planes and axes, in step with their quads/lines.
void DrawDatumLabels(AppState& state);

// Toponaming debug: the hovered (else selected) face's element name, or the
// hovered edge's, at the cursor.
void DrawElementNameOverlay(AppState& state);

} // namespace ui
