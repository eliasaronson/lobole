#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Verifying the G-code we emit (docs/cam/plan.md, C5).
//
// The loop runs on the POSTED FILE, not on the in-memory toolpath: parse the
// G-code back, simulate the material it removes, and compare the result with
// the model that was designed. That independence is the point — a bug in the
// post, in modal state, or in linking is invisible to anything that inspects
// `std::vector<Move>`, and one of those (a NaN retract) had already shipped.
//
// Surveyed before writing (~/Documents/CAD/CAM): FreeCAD's PathSimulator is a
// height-map (`Array2D<float>` + a tool profile function) and CAMotics is a
// full implicit-field/marching-cubes simulator needing its own build system.
// For 3-axis verification the height map is the right model, and it needs no
// dependency at all. Where this differs from FreeCAD's: it tests cells
// analytically against the swept tool rather than oversampling the path at
// 0.6-pixel steps — for a check that has to be trustworthy, exact beats
// "small enough steps that we probably hit every pixel".
namespace cam {

// --- parsing --------------------------------------------------------------

struct GMove {
  bool rapid = false;
  glm::dvec3 to{0.0};
  int tool = 1;
  int line = 0; // 1-based, for messages that point at the file
  // Modal feed, mm/min. The parser used to read F and THROW IT AWAY, so the
  // one number a machinist looks at was the one number nothing downstream
  // could see — no feed check, and no honest clock for playback. 0 means the
  // file never stated one, which is a fact and not a default.
  double feed = 0.0;
  // Which operation emitted this move, from the post's own `(op N)` comment.
  // Read out of the FILE rather than matched back to the toolpath list: the
  // post interleaves retracts and tool changes, so the two are not parallel,
  // and the file is the thing being simulated. 0 = preamble/postamble.
  uint64_t op = 0;
  // First move under a new tool (an M6, or a T that differs from the last
  // one). What "skip to the next tool" steps between.
  bool toolChange = false;
};

struct GProgram {
  std::vector<GMove> moves;
  std::vector<std::string> errors; // malformed words, NaN coordinates, …
  // Things a CONTROLLER would refuse that this parser survives. Ours counts
  // parenthesis depth and carries on; Mach3 stops on the second "(" with
  // "Nested comment found" and loads nothing. So the verifier once blessed a
  // file no machine would take, which is the exact shape of failure this
  // whole module exists to catch (a tool named "6 mm flat 2F (through cuts)"
  // did it). Warnings, not errors: the parse itself is fine.
  std::vector<std::string> warnings;
};

// Modal G-code: G0/G1 with X/Y/Z/F, T for the tool, M6 for a tool change,
// parenthesised comments. Anything it does not understand is reported rather
// than skipped silently.
[[nodiscard]] GProgram ParseGcode(std::string_view text);

// The posted file, back in SETUP space. A file's coordinates are relative to
// the work origin (the operator's zero); everything that compares a file to
// the model — the verifier, the simulator, the backplot — works in setup
// space. This is the one place the origin is added back, and it is the exact
// inverse of what `Emitter` subtracts (docs/cam/stock-and-origin.md W1).
//
// Take this rather than ParseGcode wherever a Setup is in hand: a program
// parsed raw and compared against a framed model is off by the origin, which
// at origin (0,0,0) — every job before v68 — is invisible.
[[nodiscard]] GProgram ProgramInSetupSpace(std::string_view text,
                                           const Setup& setup);

// --- height maps ----------------------------------------------------------

struct HeightMap {
  double x0 = 0.0, y0 = 0.0; // corner of cell (0,0)
  double res = 0.25;         // mm per cell
  int nx = 0, ny = 0;
  std::vector<float> z;

  [[nodiscard]] float At(int i, int j) const {
    return z[static_cast<size_t>(j) * static_cast<size_t>(nx) +
             static_cast<size_t>(i)];
  }
  void Set(int i, int j, float v) {
    z[static_cast<size_t>(j) * static_cast<size_t>(nx) +
      static_cast<size_t>(i)] = v;
  }
  [[nodiscard]] double CellX(int i) const { return x0 + (i + 0.5) * res; }
  [[nodiscard]] double CellY(int j) const { return y0 + (j + 0.5) * res; }
};

// The stock block, uncut.
[[nodiscard]] HeightMap MakeStock(const glm::dvec3& min, const glm::dvec3& max,
                                  double res);

// Lowers `stock` where a tool sweeping from `from` to `to` passes. THE one
// place material is removed: `Simulate`, the rapid probe in `Inspect` and the
// playback in CamSim.h all funnel through it, so the picture on screen cannot
// remove different metal from the check that signs the job off. A null tool
// is a 2 mm flat mill (the file states a T, not a shape).
void CarveSweep(const glm::dvec3& from, const glm::dvec3& to, const Tool* tool,
                HeightMap& stock);

// Runs `program` against `stock`, lowering it where the tool passes. Tool
// radii come from the setup's tool table (the file carries only T-words).
void Simulate(const GProgram& program, const Setup& setup, HeightMap& stock);

// The designed part's top surface on the same grid: the highest point of the
// bodies above each cell, or the stock floor where there is no material.
[[nodiscard]] HeightMap SampleModel(const std::vector<Body>& bodies,
                                    const HeightMap& like, double floorZ);

// --- the verdict ----------------------------------------------------------

struct Verdict {
  double maxGouge = 0.0; // mm cut BELOW the designed surface (scrap)
  // How far the deepest cut went below the WORKPIECE — the spoilboard, which
  // is what a through cut is for. Reported, never counted as gouge, and not
  // part of Safe(): below the stock there is no part left to scrap.
  //
  // It has to be separate rather than absent. Every through cut sets bottomZ
  // below the stock floor on purpose (the profile ops on a real job use
  // -0.5), and where the stock's bottom margin is 0 that floor IS the part's
  // bottom face — so comparing cell to cell reported 0.50 mm of "gouge" on
  // correct G-code and told the operator "that is scrap". A verifier that
  // cries wolf on every through cut teaches people to skip the one line that
  // would have mattered.
  double belowStock = 0.0;
  glm::dvec2 belowStockAt{0.0};
  double maxLeftover = 0.0; // mm of stock still above it (unfinished)
  glm::dvec2 gougeAt{0.0};
  glm::dvec2 leftoverAt{0.0};
  int rapidsThroughStock = 0; // rapid moves that pass through material
  // How FAR into the material the worst of them reaches, and where. A count
  // alone cannot be acted on: a ball retracting from a groove of its own
  // shape grazes a scallop's worth, and a rapid across a rib buries the
  // cutter -- the same number of findings, and only one of them stops the
  // machine. Zero when there are none.
  double worstRapidDepth = 0.0; // mm of stock standing above the tool
  glm::dvec2 worstRapidAt{0.0};
  int worstRapidLine = 0;
  // Moves whose HOLDER -- the collet nut above the flutes -- passes through
  // material the cutter never reached: a boss beside a pocket, a clamp, a
  // tall neighbour on the same sheet. A different failure from every other
  // one here, and the only one the shape of the finished part cannot show,
  // because the part can come out perfect and the nut still smash a fixture
  // on the way past.
  //
  // Silent unless the tool states a holder (`HasHolder`). Every job in the
  // corpus predates the two fields, so this counts zero for all of them --
  // which is the honest answer: nobody said how big the nut was.
  int holderCollisions = 0;
  double worstHolderDepth = 0.0; // mm the stock stands above the collar
  glm::dvec2 worstHolderAt{0.0};
  int worstHolderLine = 0;
  // Moves in the POSTED FILE that ask for somewhere the machine cannot go.
  // Checked on the artifact rather than on the generator that made it: the
  // generator's own idea of reach was wrong once already (it nailed the
  // travel box to the stock's corner, so turning a part round moved the
  // machine), and a check that reads the file cannot inherit that mistake.
  // Arcs are re-expanded to chords before this, so a circle is measured by
  // what it SWEEPS and not by its endpoints -- sampling endpoints understated
  // a d148 bore's extent by 18 mm.
  //
  // Not a gouge and not scrap: the controller alarms or hits a stop, so the
  // job simply cannot run. That is still Safe() = false, because "safe" here
  // means the file is fit to send.
  // What the PARSER made of the file, carried into the verdict because it
  // was being thrown away. `Inspect` parsed the program, collected the
  // errors and warnings the parser exists to produce, and returned none of
  // them: a NaN coordinate ("axis word with no number" -- the very bug this
  // module's header says had already shipped), a nested comment Mach3 will
  // not load, a feed of zero, all computed and then dropped on the floor.
  // Tested at the parser and consumed by nobody.
  //
  // An error means the simulation did not see the program the machine will,
  // so the verdict below is an answer to a different question -- which is
  // why it counts against Safe(). A warning is a file that loads here and
  // may not load there, so it is said and not counted.
  int fileErrors = 0;
  std::string firstFileError;
  int fileWarnings = 0;
  std::string firstFileWarning;
  int outsideTravel = 0;
  int firstOutsideLine = 0;
  double worstOvershoot = 0.0; // mm past the limit, on `outsideAxis`
  char outsideAxis = 'X';
  // Corners the program relieved on purpose (R4): cells inside a relief's
  // bite are not gouge, and the verdict says how many there were.
  int reliefs = 0;
  // Tabs the program left on purpose: material inside one, at or below its
  // top, is not leftover. Said out loud rather than silently subtracted --
  // "left 4 tabs standing" is a job to sign off, and a job that reports none
  // where four were asked for is a job to look at.
  int tabs = 0;
  int firstBadLine = 0; // the G-code line of the first such rapid
  size_t cells = 0;
  // How much of the stock the program never passed over. A pocket-only job
  // leaves most of the block untouched and that is CORRECT, so leftover is
  // measured only where the tool actually went — otherwise every partial job
  // reports metres of "leftover" and the verdict becomes noise to scroll
  // past. This number keeps the other signal (a job that machines nothing at
  // all) available without confusing the two.
  double untouchedFraction = 0.0;
  // How much leftover there is, not just how deep the worst of it is. A part
  // cut correctly with a 3 mm cutter leaves 1.5 mm radii in its square inside
  // corners — full-depth leftover, in four tiny places — and that is the
  // right answer, not a defect. A skipped operation leaves ONE region of
  // hundreds of square millimetres. The max alone cannot tell those apart,
  // which is how a job whose verdict should have been read ends up being
  // scrolled past.
  double leftoverArea = 0.0; // mm^2 above the designed surface
  int leftoverRegions = 0;   // separate places it sits in
  // The biggest of those places, largest first. "341 mm2 in 12 places" is a
  // number to worry about; "the slot's slug, two bores and four tabs" is a
  // job to sign off. Capped, so a fine grid cannot flood a report.
  struct Region {
    glm::dvec2 at{0.0}; // centroid
    double area = 0.0;  // mm^2
    double depth = 0.0; // the worst leftover in it, mm
  };
  std::vector<Region> biggest;

  // TWO questions, because they have different answers and only one of them
  // is ever certain.
  //
  // Safe: nothing here scraps the part or breaks the tool. A gouge and a rapid
  // through material are unambiguous — no job wants either, ever.
  [[nodiscard]] bool Safe(double tolerance) const {
    return maxGouge <= tolerance && rapidsThroughStock == 0 &&
           outsideTravel == 0 && holderCollisions == 0 && fileErrors == 0;
  }
  // Complete: nothing is left standing that the model says should be gone.
  // This one is SOFTER, because a height map cannot watch a slug fall out of
  // a through cut — so a correct job can still report material left over, and
  // the two used to be one flag that no real job could ever set
  // (docs/cam/dogfood-round.md). Tabs no longer count against it: they are
  // asked for, and the toolpath now says where it left them, so every job
  // that used one reported `complete: false` for a reason the flag could not
  // express. Read it with leftoverArea and `biggest`: several small places
  // are the corners a round cutter cannot reach, one large one is an
  // operation nobody added.
  [[nodiscard]] bool Complete(double tolerance) const {
    return maxLeftover <= tolerance;
  }
};

// `touched` (optional, same grid) restricts the comparison to the cells the
// program actually swept — see Verdict::untouchedFraction. Null compares
// every cell.
// Leftover deeper than this counts toward `leftoverArea`/`leftoverRegions`.
// Zero takes the grid's own quantisation (one and a half cells).
// `floorZ` is the bottom of the STOCK. Material removed below it is the
// spoilboard, not the part, so it lands in `belowStock` instead of `maxGouge`
// (see that field). Callers that genuinely have no workpiece bottom can pass
// -infinity, which restores a plain cell-to-cell comparison.
[[nodiscard]] Verdict Compare(const HeightMap& machined,
                              const HeightMap& designed, double floorZ,
                              const std::vector<uint8_t>* touched = nullptr,
                              double leftoverTolerance = 0.0);

// Everything the check produced, for a caller that wants to SHOW it rather
// than just pass or fail (docs/cam/gui.md G4 paints the leftover on the part).
struct Inspection {
  Verdict verdict;
  HeightMap machined;
  HeightMap designed;
  std::vector<uint8_t> touched; // 1 where a cutting move swept this cell
  bool ok = false; // false = there was nothing to check (no stock, no bodies)
};

// `reliefs`: where the toolpaths deliberately cut into the model (corner
// relief, R4) -- cells within a relief's radius are not counted as gouge.
// `tabs`: where they deliberately did NOT cut -- material at or below a tab's
// top, within its radius, is not counted as leftover.
[[nodiscard]] Inspection
Inspect(const std::string& gcode, const Setup& setup,
        const std::vector<Body>& bodies, double res = 0.25,
        const std::vector<Toolpath::Relief>& reliefs = {},
        const std::vector<Toolpath::Tab>& tabs = {});

// Where the tool could not reach: cell centres whose machined height is more
// than `tolerance` above the designed surface, as 3D points sitting on the
// machined surface. Capped at `maxPoints` (evenly sampled) so a fine grid
// cannot flood the overlay.
[[nodiscard]] std::vector<glm::dvec3> LeftoverPoints(const Inspection& insp,
                                                     double tolerance,
                                                     size_t maxPoints = 4000);

// The whole loop, for callers that just want the answer: post text in,
// verdict out.
[[nodiscard]] Verdict
VerifyGcode(const std::string& gcode, const Setup& setup,
            const std::vector<Body>& bodies, double res = 0.25,
            const std::vector<Toolpath::Relief>& reliefs = {},
            const std::vector<Toolpath::Tab>& tabs = {});

// The verdict in the words a person needs, one line each, most severe first
// (docs/cam/gui.md). Empty only when there is nothing to say.
struct VerdictLine {
  enum class Level { Good, Warn, Bad };
  Level level = Level::Good;
  std::string text;
};

// The MACHINE's lines of the verdict (docs/cam/job-document.md, M2): the
// parts' reach beyond the travel, or the sheet's overhang when the parts
// fit -- as WARN lines, never refusals. One helper for the panel and the
// RPC, so the two verdicts cannot disagree about the machine.
// `paths`, when given, adds how much cutting was clipped off at the travel.
[[nodiscard]] std::vector<VerdictLine>
MachineLines(const Setup& setup, const std::vector<Body>& bodies,
             const std::vector<Toolpath>* paths = nullptr);

// `breaksAnEdge` says the job contains an edge-break operation. It changes
// nothing that is measured — it names the most likely cause of the gouge such
// a job reports, which is that the round or chamfer exists in the TOOLPATH and
// not in the model the verifier compares against.
[[nodiscard]] std::vector<VerdictLine> Explain(const Verdict& v,
                                               double tolerance, double minutes,
                                               bool breaksAnEdge = false);

} // namespace cam
