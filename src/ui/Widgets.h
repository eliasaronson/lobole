#pragma once

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

} // namespace ui
