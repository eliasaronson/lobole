#pragma once

#include "core/cam/CamJob.h"

#include <string>
#include <vector>

// Posting toolpaths to a machine dialect (docs/cam/plan.md).
//
// A post is small — FreeCAD's are 150 (Mach3) to 481 (LinuxCNC) lines — so
// this is a narrow interface with one implementation per machine, not a
// framework. The machine here is Mach3 today and LinuxCNC later, which is
// exactly why the seam exists from the first post rather than the second.
namespace cam {

struct PostOptions {
  std::string program = "designer";
  bool comments = true; // human-readable op headers
  int decimals = 4;     // coordinate precision
  bool useG54 = true;   // emit the work offset in the preamble
  // Mach3 has no tool table: a tool change is M6 and a pause for the human.
  // LinuxCNC can do it properly. Posts decide; this just carries the intent.
  bool pauseOnToolChange = true;
  // C7: fit runs of straight moves back into G2/G3 arcs. Offsetting flattens
  // every curve to a polyline, so a circular pocket posts as hundreds of G1s
  // that the controller then has to look ahead through. Off is the escape
  // hatch for a controller that mishandles arcs — and the way the test proves
  // the fit changes the file without changing the cut.
  bool fitArcs = true;
};

// Every dialect this build can emit, by id ("mach3", "linuxcnc").
[[nodiscard]] std::vector<std::string> PostIds();
[[nodiscard]] bool PostExists(const std::string& id);

// The G-code for `paths`, in the order given. Returns false and fills
// `error` on an unknown post id; a toolpath carrying its own error is
// SKIPPED with a comment rather than silently dropped — a missing operation
// that nobody mentions is how a part comes off the machine unfinished.
bool PostProcess(const std::string& postId, const Setup& setup,
                 const std::vector<Toolpath>& paths, const PostOptions& opts,
                 std::string& out, std::string* error = nullptr);

} // namespace cam
