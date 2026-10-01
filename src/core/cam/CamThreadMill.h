#pragma once

#include "core/Feature.h" // Body, ThreadMark
#include "core/cam/CamGcode.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamVerify.h" // ThreadVerdict, HeightMap

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Thread milling (docs/cam/operations.md#thread-mill).
//
// FreeCAD's shape (Mod/CAM Path/Op/ThreadMilling.py and
// Path/Base/Generator/threadmilling.py): the helix radius from the thread's
// sharp-V tip less the tool's flat, radial passes of equal chip area,
// half-turn G2/G3 arcs each advancing Z by half a pitch, a partial last arc,
// semicircle lead-in and lead-out from an "elevator" inside the bore. The
// functions below reproduce its numbers, which the tests pin. What differs is
// what drives them: the Thread on the face (size, pitch, hand, side, and the
// class, whose pitch-diameter band's MIDDLE the last pass aims at), a
// through thread overtravelling a pitch past its last full turn, a blind one
// stopped by the head on its floor, and the refusals FreeCAD only logs.
namespace cam {

// --- FreeCAD's numbers ------------------------------------------------------

// The tool CENTRE's radius for the first and the last pass
// (`threadRadii`). The last puts the tool's rim at the thread's sharp-V tip
// -- major/2 + H/8 inside, minor/2 - H/4 outside, H = 1.6 (major - minor)/2
// -- less the flat's own reach, flat / (2 tan(angle / 2)) = 0.866 flat at 60
// degrees. Internal: from (minor - tool)/2 out; external: from
// (major + tool)/2 in.
struct HelixRadii {
  double first = 0.0, last = 0.0;
};
[[nodiscard]] HelixRadii ThreadRadii(bool internal, double majorDia,
                                     double minorDia, double toolDia,
                                     double tipFlat, double angleDeg = 60.0);

// The radius of each of `count` passes from `first` to `last`, each taking
// the same AREA of the thread's triangle: Hi = H sqrt((i + 1) / count)
// (`threadPasses`).
[[nodiscard]] std::vector<double> ThreadPassRadii(int count, double first,
                                                  double last);

// How many passes take `depth` (mm, radially) with this cutter, when nobody
// said: the fewest whose FIRST -- the deepest, at equal area -- takes at
// most half the cutter's profile depth.
[[nodiscard]] int DerivedThreadPasses(double depth, const Tool& tool);

// One piece of a pass before it is flattened into moves: a line, or an arc
// round `centre` (setup XY) ending at `to`, Z linear along it.
struct HelixSegment {
  bool arc = false;
  bool clockwise = false;
  glm::dvec3 to{0.0};
  glm::dvec2 centre{0.0};
};

// One pass (`generate`): from the elevator -- `elevator` mm off `centre`
// along +Y -- the lead-in (a semicircle out to `radius`, in the helix's
// direction inside a hole and against it round a boss; a straight move
// without `leadInOut`), then half turns of `clockwise` arcs from zStart,
// each advancing Z by half a pitch toward zFinal, a partial last arc to
// zFinal, and the lead-out back to the elevator's radius on the radial line
// the helix ended on. The tool is taken to be AT the elevator at zStart.
[[nodiscard]] std::vector<HelixSegment>
HelixPass(const glm::dvec2& centre, bool clockwise, double zStart,
          double zFinal, double pitch, double radius, bool leadInOut,
          double elevator, bool internal);

// FreeCAD's direction table: climb inside a hole is G3 (counter-clockwise),
// conventional G2, and round a boss the other way; a right-hand thread's
// helix rises when it turns counter-clockwise. So which way the arcs turn,
// and whether a pass runs bottom to top.
struct HelixDirection {
  bool clockwise = false;
  bool upward = true;
};
[[nodiscard]] HelixDirection ThreadDirection(bool internal, bool rightHand,
                                             bool climb);

// A thread mill head's axial width: its flat plus both flanks' rise over the
// profile depth -- how far the V's middle stands above the tool's tip.
[[nodiscard]] double ThreadMillHeadWidth(const Tool& tool);

// --- what a target is -------------------------------------------------------

// Why this setup cannot post a helix, or empty: a machine set to post no
// arcs (fit_arcs off), or a post whose controller cannot interpolate one
// (PostWritesHelicalArcs).
[[nodiscard]] std::string HelicalArcRefusal(const Setup& setup);

// Why this tool cannot mill `pitch`, or empty: not a thread mill, a V that
// is not 60 degrees, a flat wider than the thread's root (P / 8) -- too fine
// a thread for it -- or a neck no narrower than its head.
[[nodiscard]] std::string ThreadToolRefusal(const Tool& tool, double pitch);

// Everything a thread-mill target resolves to, or why it cannot be cut. In
// setup space, from bodies already framed. The same answer for derivation,
// generation and the declaration the verifier judges against.
struct ThreadPlan {
  bool ok = false;
  std::string why;
  std::vector<std::string> notes; // true, worth saying, not refusals
  ThreadMark mark;
  Toolpath::Thread thread; // what the verifier is told
  double depth = 0.0;      // radial, bore to tip, mm
  double startRadius = 0.0, finalRadius = 0.0, elevator = 0.0; // tool centre
  std::vector<double> passes;      // tool-centre radius of each pass
  double headWidth = 0.0;          // the V's axial width
  double tipLo = 0.0, tipHi = 0.0; // the tool TIP's lowest and highest Z
  HelixDirection direction;
};
// `op` supplies the direction (climb) and the passes; `stockTop` is where a
// rapid may stop above (the setup's stock top).
[[nodiscard]] ThreadPlan PlanThread(const Setup& setup, const Operation& op,
                                    const Tool& tool,
                                    const std::vector<Body>& framed,
                                    const Target& target);

// --- judging it --------------------------------------------------------------

// The thread `decl` declares, read from the posted program (in setup space):
// the op's helix runs, the last pass's pitch, hand and radius, and the span
// of full thread. `boreStanding`/`At` are measured separately (they need the
// stock as it was before the op) and passed through.
[[nodiscard]] ThreadVerdict JudgeThread(const GProgram& program,
                                        const Toolpath::Thread& decl);

// The stock standing where the head must travel -- the bore inside
// `boreDia` for an internal thread, the ring round the boss for an external
// one -- above the lowest the op's tool goes, when the op STARTS: `stock` is
// the map before the program's first move, carved here by every move before
// the op's first. The tap drill cut in an earlier op (or setup) leaves none.
// Nothing stands below `stockBottom`: under the stock is the spoilboard.
void MeasureBoreBefore(const GProgram& program, const Setup& setup,
                       const HeightMap& stock, double stockBottom,
                       const Toolpath::Thread& decl, ThreadVerdict& v);

// Per cell of `grid`, the lowest the verifier excuses a cut to, or the
// largest float where no thread's head reaches: inside the thread's widest
// allowed tip radius (outside it, round a boss), down to the head's lowest
// tip in the file.
[[nodiscard]] std::vector<float>
ThreadExcuse(const HeightMap& grid, const std::vector<Toolpath::Thread>& decls,
             const std::vector<ThreadVerdict>& judged);

} // namespace cam
