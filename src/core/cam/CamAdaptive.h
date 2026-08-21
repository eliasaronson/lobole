#pragma once

#include "core/cam/CamJob.h"
#include "core/cam/CamToolpath.h" // Loop

#include <string>
#include <vector>

// Adaptive clearing (docs/cam/plan.md, C8).
//
// Kresimir Tusek's `Adaptive2d`, vendored at deps/libarea/ from FreeCAD
// b2da06bf. Concentric offsetting takes the full width of the cutter on the
// first ring of every pocket; adaptive keeps the ANGLE OF ENGAGEMENT roughly
// constant instead, which is what lets a small spindle take the whole depth
// in one pass. It is the one algorithm in this workstream nobody should
// reinvent.
//
// LICENCE BOUNDARY. libarea is LGPL-2.1-or-later and this application is not
// (today AGPL-3.0, and that is a decision worth keeping open). The LGPL code
// is therefore built as its own shared library — `designer_adaptive`, see
// CMakeLists — and reached only through this header, whose declarations
// carry no LGPL code into the caller. THIS FILE is the whole surface: nothing
// else in Designer includes `Adaptive.hpp`, and nothing should.
//
// Its public interface makes that cheap, which is the reason this is
// vendoring rather than porting: `Execute()` speaks millimetre doubles in and
// (motion type, polyline) pairs out. The Clipper1 it uses internally never
// crosses the boundary.
namespace cam {

// A cleared region's toolpath, in the same Move vocabulary as everything
// else. `error` is non-empty when Adaptive2d gave up, in the words the panel
// prints — it reports seven distinct failures and a boolean would waste them.
struct AdaptiveResult {
  std::vector<Move> moves;
  std::string error;
  bool available = false; // false = this build has no adaptive clearing
};

// Clears `region` (outer loops plus island holes, in setup space) from
// `fromZ` down to `z`, entering on the helix Adaptive2d plans for itself.
[[nodiscard]] AdaptiveResult
AdaptiveClear(const Setup& setup, const Operation& op, const Tool& tool,
              const std::vector<Loop>& region, double z, double fromZ);

// Whether this build has it at all (-DENABLE_CAM_ADAPTIVE=OFF removes the
// dependency entirely). The UI asks so it can fall back rather than refuse.
[[nodiscard]] bool AdaptiveAvailable();

} // namespace cam
