#pragma once

#include <imgui.h>

#include <functional>
#include <string>

// Shared pieces of chrome, so the same thing looks the same wherever it
// happens.
namespace ui {

// One long operation, in progress.
//
// Five places grew their own version of this -- the STEP import modal, the
// render still, the topology solver, the ray-traced preview and the CAM pass
// queue -- and each one rediscovered the same rule the hard way: a bar that
// sits at 0% because the work reports no fraction reads as STUCK, so an
// operation with nothing to report animates instead. The occurrence panel's
// copy even cites "the import modal's rule" in a comment, which is a fair
// description of what a shared widget is for.
//
// They are not all the same SHAPE -- the import is modal because the document
// is half-built until it finishes, and the CAM queue must not be, because the
// whole point of running it off the frame is that the app stays usable. What
// they share is what a person reads: a line saying what is happening, a bar
// that means the same thing every time, and one way to ask it to stop.
struct JobProgress {
  std::string status;     // the line above the bar
  double fraction = -1.0; // < 0 = indeterminate, and it will say "working"
  std::string overlay;    // inside the bar; empty takes ImGui's percentage
  bool cancellable = true;
  bool cancelAsked = false; // already asked: the button greys and says so
  std::string cancelId;     // "###cam_cancel_passes"
  std::string cancelTip;
};

// Draws it. Returns true on the frame the cancel button is pressed.
bool DrawJobProgress(const JobProgress& job);

// A section that opens and closes (docs/ui/panels.md#shared-widgets): a
// chevron pointing right when closed and down when open, the label, an
// optional count after it ("Occurrences 22"), and a hover highlight across
// the whole row. ImGui's CollapsingHeader -- a filled bar with a small
// triangle -- was easy to miss as something that opens, in 12 places. The
// open state lives where CollapsingHeader kept it (the window's storage,
// under the label's id), so a section a person had open stays open, and the
// test engine sees it as openable, so `ItemOpen` works on it as before.
// Returns whether it is open.
bool SectionHeader(const char* label, int count = -1, bool defaultOpen = false);

// One row of a list in the feature tree's style: a glyph, the label as a
// full-width selectable, a dim tail at the right, dimmed when `dimmed`, and --
// when `eye` is not None -- a visibility eye at the right edge that shows on
// hover, or always, struck through, when the row is hidden. The eye is always
// submitted (invisible until hovered), so a test can click it.
struct ListRowSpec {
  const char* glyph = nullptr;
  ImVec4 glyphColor{0, 0, 0, 0}; // alpha 0 = the disabled text colour
  std::string label;             // "text###id"
  bool selected = false;
  bool dimmed = false;
  std::string tail;                // dim text at the right; may be empty
  const char* tailGlyph = nullptr; // a glyph before the tail (e.g. an anchor)
  enum class Eye { None, Shown, Hidden } eye = Eye::None;
  std::string eyeId; // "###occ_eye_7"
  std::string tooltip;
  // Drawn as the row's context menu when it is right-clicked; may be empty.
  std::function<void()> menu;
  std::string menuId; // "###occ_ctx_7"
};
struct ListRowResult {
  bool clicked = false;       // a single click on the row
  bool doubleClicked = false; // a double-click: edit
  bool eyeClicked = false;
  bool hovered = false;
};
ListRowResult ListRow(const ListRowSpec& row);

} // namespace ui
