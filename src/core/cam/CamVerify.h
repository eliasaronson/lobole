#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamGcode.h"
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Verifying the G-code we emit (docs/cam/simulation.md).
//
// The loop runs on the POSTED FILE, not on the in-memory toolpath: parse the
// G-code back, simulate the material it removes, and compare the result with
// the model that was designed. That independence is the point — a bug in the
// post, in modal state, or in linking is invisible to anything that inspects
// `std::vector<Move>`, and one of those (a NaN retract) had already shipped.
//
// Surveyed before writing (other open-source CAM): FreeCAD's PathSimulator is a
// height-map (`Array2D<float>` + a tool profile function) and CAMotics is a
// full implicit-field/marching-cubes simulator needing its own build system.
// For 3-axis verification the height map is the right model, and it needs no
// dependency at all. Where this differs from FreeCAD's: it tests cells
// analytically against the swept tool rather than oversampling the path at
// 0.6-pixel steps — for a check that has to be trustworthy, exact beats
// "small enough steps that we probably hit every pixel".
namespace cam {

struct OtherSetups; // CamOtherSetups.h

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

// How far inside its nominal radius a cutter's wall is taken to stand, mm,
// wherever the verifier asks whether a cell is within reach of the tool --
// the planar carve and rapid check here, the rotary walk in CamRotary.cpp
// (docs/cam/simulation.md#the-cutters-wall). Toolpaths lie on the grid's own
// lattice, so a cutter's wall lands EXACTLY on cell centres, and whether such
// a cell was inside was decided by the last bit of rounding of the stock's
// origin: a rapid 3 mm through stock on one side of an ulp, none on the
// other. A tenth of a micron moves the wall off the lattice, and is a
// hundredth of the thresholds any verdict is judged at.
inline constexpr double kCutterWallTolerance = 1e-4;

// Lowers `stock` where a tool sweeping from `from` to `to` passes. THE one
// place material is removed: `Simulate`, the rapid probe in `Inspect` and the
// playback in CamSim.h all funnel through it, so the picture on screen cannot
// remove different metal from the check that signs the job off. A null tool
// is a 2 mm flat mill (the file states a T, not a shape). Returns the volume
// it took, mm^3: what the verifier measures a move's bite from.
double CarveSweep(const glm::dvec3& from, const glm::dvec3& to,
                  const Tool* tool, HeightMap& stock);

// Runs `program` against `stock`, lowering it where the tool passes. Tool
// radii come from the setup's tool table (the file carries only T-words).
void Simulate(const GProgram& program, const Setup& setup, HeightMap& stock);

// The designed part's top surface on the same grid: the highest point of the
// bodies above each cell, or the stock floor where there is no material.
// On every core; a cell is a maximum, so any thread count gives the same map.
[[nodiscard]] HeightMap SampleModel(const std::vector<Body>& bodies,
                                    const HeightMap& like, double floorZ);

// How many bands of rows `Inspect`'s walk splits the map into, on this
// thread, while it lives (docs/cam/simulation.md#on-every-core). 0 derives
// the count from the workers, which is the default. Every count gives the
// same answer to the bit: this exists so a test can say so.
struct ScopedInspectBands {
  explicit ScopedInspectBands(size_t bands);
  ~ScopedInspectBands();
  ScopedInspectBands(const ScopedInspectBands&) = delete;
  ScopedInspectBands& operator=(const ScopedInspectBands&) = delete;

private:
  size_t m_was = 0;
};

// --- the verdict ----------------------------------------------------------

// A milled thread, judged on its own terms (docs/cam/simulation.md#threads).
// A height map cannot see a flank that faces sideways -- the carve clears
// the whole column the head sweeps, crests and all -- so the thread is read
// from the posted file's helix against what the toolpath declared
// (Toolpath::Thread): its pitch and hand, the pitch diameter the last pass's
// tip reached against the class's band, the span of FULL thread it leaves,
// the bore it runs in (cut before it, and wide enough for the neck).
struct ThreadVerdict {
  uint64_t op = 0;
  std::string designation;
  std::string threadClass;
  bool classStated = true;
  bool internal = true;
  // False when the file could not be judged -- no helix of this op in it, a
  // file that turns A -- and `why` says so. Counts against Complete.
  bool judged = false;
  std::string why;
  double pitch = 0.0;         // what the thread is
  double pitchMeasured = 0.0; // what the last pass's helix advances a turn
  bool rightHand = true;      // what the thread is
  bool handMeasured = true;   // the helix's own hand
  double pdMin = 0.0, pdMax = 0.0, pdReached = 0.0; // mm
  int passes = 0;
  double zLo = 0.0, zHi = 0.0;         // the threaded span
  double fullFrom = 0.0, fullTo = 0.0; // where the thread is full depth
  double shortMm = 0.0; // span left short of full, past what a floor forces
  bool floorLimited = false; // a blind floor stops the head short
  double tipLowest = 0.0;    // the lowest the head's tip went in the file
  // Material standing in the bore (round the boss, external) when the op
  // starts, above the lowest the head goes: the tap drill not cut first.
  double boreStanding = 0.0;
  glm::dvec2 boreStandingAt{0.0};
  // How far the neck stays off the bore's wall at the deepest pass; negative
  // is the neck rubbing the crests.
  double neckClearance = 0.0;
  [[nodiscard]] bool PitchOk() const {
    return std::abs(pitchMeasured - pitch) <= 1e-3 * std::max(1.0, pitch);
  }
  [[nodiscard]] bool InBand() const {
    return pdReached >= pdMin - 1e-3 && pdReached <= pdMax + 1e-3;
  }
  // Nothing breaks: the head does not plunge into a bore that is not
  // there, and the neck does not rub the crests.
  [[nodiscard]] bool Safe(double tolerance) const {
    return boreStanding <= tolerance && neckClearance >= -1e-6;
  }
  // The thread is the thread the part asks for.
  [[nodiscard]] bool Good(double tolerance) const {
    return judged && PitchOk() && handMeasured == rightHand && InBand() &&
           shortMm <= tolerance;
  }
};

// A toleranced round wall, judged against its band
// (docs/cam/simulation.md#tolerance-bands): the diameter the posted file cut
// -- the widest the tool's rim reached round a bore over the wall's span,
// the narrowest round a boss -- from every operation that declared it.
struct FitVerdict {
  std::vector<uint64_t> ops; // the operations that aimed at it
  glm::dvec2 centre{0.0};
  // A floor's distance from its reference, not a diameter: `nominal`, the
  // band and `cut` are that distance, and `internal` is a floor below its
  // reference (cutting deeper lengthens it, as a bore grows).
  bool depth = false;
  double refZ = 0.0;
  bool internal = true;
  double nominal = 0.0, min = 0.0, max = 0.0, aim = 0.0; // mm, diameters
  std::string label;
  bool judged = false; // false: no cutting move of those ops on the wall
  std::string why;
  double cut = 0.0; // mm, the diameter the file cuts
  // Past the band on the side nothing can put back: a bore too big, a boss
  // too small. Counts against Safe.
  [[nodiscard]] bool Scrap() const {
    return judged && (internal ? cut > max + 1e-4 : cut < min - 1e-4);
  }
  // Short of the band: metal left on the wall. Counts against Complete.
  [[nodiscard]] bool Short() const {
    return !judged || (internal ? cut < min - 1e-4 : cut > max + 1e-4);
  }
  [[nodiscard]] bool InBand() const { return judged && !Scrap() && !Short(); }
};

// A hole or a thread of the job's parts that the job does not cut
// (docs/cam/simulation.md#what-the-job-does-not-cut). A finding, not a
// failure: a job may leave a feature to the tap, the reamer or another job.
struct UncutFeature {
  enum class Kind { Hole, Thread };
  Kind kind = Kind::Hole;
  uint64_t jobPartId = 0;
  uint64_t featureId = 0;
  std::string feature; // the Hole's name, or the thread's designation
  ElementRef face;     // the bore, or the threaded face
  std::string what;    // "M4 thread", "hole D3.3 deep 8"
  glm::dvec3 at{0.0};  // the face's middle, model space
};

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
  // Rotary only: the 95th percentile of leftover over the compared cells --
  // the TYPICAL scallop. The worst cell of a rotary job sits on an end face
  // no rotary pass can reach, whatever the pattern, so `maxLeftover` alone
  // cannot tell two finishes apart
  // (docs/cam/rotary.md#verifying-a-turning-part).
  double leftoverP95 = 0.0;
  // Rotary only: leftover on the SURFACE a rotary pass is for -- every cell
  // except those on faces turned toward the axis's ends (steeper than 70
  // degrees along the axis, or at the part's end), where no rotary pass can
  // reach and every job's worst cell sits. What tells two patterns apart,
  // and what found the helical ridges
  // (docs/cam/rotary.md#verifying-a-turning-part).
  double leftoverSurfaceMax = 0.0;
  glm::dvec2 leftoverSurfaceAt{0.0};
  double leftoverSurfaceP99 = 0.0;
  // Rotary only: how much leftover (over the area floor) is on end faces,
  // summed by CELL. Counting it by region printed "0 mm2 in 0 places" beside
  // 1.26 mm of it: a region round a dome is mostly surface, so no region was
  // called an end face while its steepest cells were.
  double leftoverEndFaceArea = 0.0;
  // Leftover another setup of the job removes
  // (docs/cam/simulation.md#leftover-another-setup-removes): excess whose
  // every point lies in air another setup leaves. Not in `maxLeftover`, the
  // regions or `Complete()` -- said per setup instead, so a flip job's front
  // does not report the outline its back finishes as a missing operation.
  struct OtherCut {
    uint64_t setup = 0;
    std::string name;
    double area = 0.0; // mm^2
  };
  std::vector<OtherCut> leftoverOtherSetups;
  // The other setups that could not be consulted, and why.
  std::vector<std::string> otherSetupsNotChecked;
  void AddOtherCut(uint64_t setup, const std::string& name, double area) {
    for (OtherCut& c : leftoverOtherSetups)
      if (c.setup == setup) {
        c.area += area;
        return;
      }
    leftoverOtherSetups.push_back({setup, name, area});
  }
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
  // Cuts that bite deeper than their operation ever asked the tool to
  // (docs/cam/simulation.md#bite). Measured on the posted file as material
  // actually removed: the volume a stretch of cutting takes, over the cutter's
  // diameter times the distance, is the depth of a full-width cut that would
  // take the same -- a slot at the stepdown reads as the stepdown, a finishing
  // skim as a fraction of it. Profile, pocket, adaptive and face ops, against
  // their stepdown (or their tab height, which a tab exit plunges). What it
  // exists for: a lead-out that slotted 16 mm of solid MDF at the cutting feed
  // on a job whose stepdown was 2.5, which moved the board and was reported by
  // nothing.
  int overBites = 0;
  double worstBite = 0.0;        // mm, full-width equivalent
  double worstBiteAllowed = 0.0; // mm, what that op's passes take
  glm::dvec2 worstBiteAt{0.0};
  int worstBiteLine = 0;
  uint64_t worstBiteOp = 0;
  // The deepest bite of every op checked, keyed by op id, with what it was
  // allowed: the number a test (or a person) compares against the stepdown.
  std::map<uint64_t, std::pair<double, double>> biteByOp;
  // Moves that reach into the CHUCK or the TAILSTOCK of a setup held in the
  // rotary (docs/cam/rotary.md#chuck-and-tailstock): the tool, a cylinder of
  // its radius from the tip up, nearer the axis over its slab along X than
  // the holding envelope allows. Rapid or cut, checked on every move -- the
  // sampler part's first program drove its first pass through the jaws.
  int holdingCollisions = 0;
  double worstHoldingDepth = 0.0; // mm into the envelope
  double worstHoldingX = 0.0;     // along the axis, setup space
  int worstHoldingLine = 0;
  std::string worstHoldingWhat; // "the chuck" | "the tailstock"
  // The holding sizes the check used that are defaults, not measured.
  std::vector<std::string> holdingDefaults;
  // Moves that drive a SHANK wider than the cutter into material standing
  // above the flutes (docs/cam/simulation.md#shank-collisions): the wall of a
  // cut deeper than the flutes are long. Silent unless the tool states a
  // wider shank (`Tool::shankDiameter`); the depth alone is stated earlier,
  // at the op (cam::ReachCheck).
  int shankCollisions = 0;
  double worstShankDepth = 0.0; // mm the material stands above the flutes
  glm::dvec2 worstShankAt{0.0};
  int worstShankLine = 0;
  // Moves that drive the cutter, or the holder, into a FIXTURE -- a clamp or
  // a vise the job was told about (docs/cam/simulation.md#fixture-collisions).
  // Checked on every move against the fixtures' own height field, which is
  // never carved: a clamp does not come off because the path went through it.
  int fixtureCollisions = 0;
  double worstFixtureDepth = 0.0; // mm of fixture above the tool there
  glm::dvec2 worstFixtureAt{0.0}; // setup space
  double worstFixtureZ = 0.0;     // the fixture's top there: where to mark it
  int worstFixtureLine = 0;
  std::string worstFixtureName;
  bool worstFixtureByHolder = false; // the collar hit it, not the cutter
  int fixturesChecked = 0;
  // Fixtures were given but not checked, because the file turns the part
  // (a rotary setup): a bed-space clamp has no meaning in the part's own
  // turning frame. Said, not skipped.
  bool fixturesSkippedRotary = false;
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
  // This verdict was read from a ROTARY stock model, r(x, theta), not from a
  // height map -- so `gougeAt`, `leftoverAt` and their neighbours are
  // (x mm, theta DEGREES) rather than (x, y). Said in the struct because a
  // pair of doubles cannot say it, and printed by `Explain`, which would
  // otherwise label an angle "Y".
  bool rotary = false;
  int fileErrors = 0;
  std::string firstFileError;
  int fileWarnings = 0;
  std::string firstFileWarning;
  // The operations the file puts under cutter compensation
  // (docs/cam/post.md#cutter-compensation): checked as programmed, which is
  // what they cut with no wear in the controller's tool table.
  std::vector<uint64_t> compensatedOps;
  int outsideTravel = 0;
  int firstOutsideLine = 0;
  double worstOvershoot = 0.0; // mm past the limit, on `outsideAxis`
  char outsideAxis = 'X';
  // The two travel checks, apart: moves past the bed in X/Y (positions, so
  // each has a line), and a program whose Z span exceeds the Z travel (a
  // span, so it has a deepest and a highest line rather than a first).
  int outsideXY = 0;
  double worstXYOvershoot = 0.0;
  char xyAxis = 'X';
  double zSpanOver = 0.0; // mm by which the Z span exceeds the Z travel
  double zSpan = 0.0, zTravel = 0.0;
  int zDeepestLine = 0, zHighestLine = 0;
  // Corners the program relieved on purpose (R4): cells inside a relief's
  // bite are not gouge, and the verdict says how many there were.
  int reliefs = 0;
  // Tabs the program left on purpose: material inside one, at or below its
  // top, is not leftover. Said out loud rather than silently subtracted --
  // "left 4 tabs standing" is a job to sign off, and a job that reports none
  // where four were asked for is a job to look at.
  int tabs = 0;
  // Engraved strokes the program declared
  // (docs/cam/simulation.md#engraved-grooves): a cell inside one's nominal
  // groove is not gouge down to that groove. `grooveShallow` is how far above
  // its groove the cut stopped at the worst cell, and counts against Complete;
  // a groove narrower than `grooveCells` (three cells) is one the grid barely
  // resolves, counted.
  int grooves = 0;
  double grooveShallow = 0.0;
  glm::dvec2 grooveShallowAt{0.0};
  int narrowGrooves = 0;
  double narrowestGroove = 0.0;
  double grooveCells = 0.0;
  // Every milled thread the program declared, judged
  // (docs/cam/simulation.md#threads). The carve inside a thread's radius,
  // down to its head's lowest tip, is not gouge; this is what says whether
  // the thread is right.
  std::vector<ThreadVerdict> threads;
  // Every toleranced wall the program declared, judged against its band
  // (docs/cam/simulation.md#tolerance-bands).
  std::vector<FitVerdict> fits;
  // Pieces of material that through cuts separate from everything that
  // holds them (docs/cam/simulation.md#tabs-and-offcuts). An offcut -- a
  // piece with none of the part in it, the slug of an opening -- is never
  // leftover; one held neither by the stock's edge nor by a tab is cut
  // free, and so is a piece of the part that nothing holds. Said, not
  // failed: a vacuum table or screws the job does not know of may hold it.
  struct CutFree {
    glm::dvec2 at{0.0}; // its middle
    double area = 0.0;  // mm^2
    bool part = false;  // the part, not an offcut
  };
  std::vector<CutFree> cutFree;
  double offcutArea = 0.0; // mm^2 of offcut excused from leftover
  // Holes and threads of the job's parts the job does not cut.
  std::vector<UncutFeature> uncut;
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
  // number to worry about; "four inside corners a round cutter cannot reach"
  // is a job to sign off. Capped, so a fine grid cannot flood a report.
  struct Region {
    glm::dvec2 at{0.0}; // centroid
    double area = 0.0;  // mm^2
    double depth = 0.0; // the worst leftover in it, mm
    // Rotary only: the region sits on faces turned toward the axis's ends,
    // which no rotary pass can reach -- expected, not a missing operation.
    bool endFace = false;
    // Rotary only: how far round the axis it reaches, degrees, and whether it
    // goes all the way -- a RING, whose `at` angle is then 0 and means
    // nothing. A ring is what a corner no pass can reach leaves on a turned
    // part; a patch at one angle is a pass that is missing or lifts there.
    double spanDeg = 0.0;
    bool ring = false;
  };
  std::vector<Region> biggest;

  // TWO questions, because they have different answers and only one of them
  // is ever certain.
  //
  // Safe: nothing here scraps the part or breaks the tool. A gouge and a rapid
  // through material are unambiguous — no job wants either, ever.
  [[nodiscard]] bool Safe(double tolerance) const {
    return maxGouge <= tolerance && rapidsThroughStock == 0 &&
           outsideTravel == 0 && holderCollisions == 0 &&
           fixtureCollisions == 0 && overBites == 0 && shankCollisions == 0 &&
           holdingCollisions == 0 && fileErrors == 0 &&
           std::all_of(
               threads.begin(), threads.end(),
               [&](const ThreadVerdict& t) { return t.Safe(tolerance); }) &&
           std::none_of(fits.begin(), fits.end(),
                        [](const FitVerdict& f) { return f.Scrap(); });
  }
  // Complete: nothing is left standing that the model says should be gone.
  // This one is SOFTER: a corner a round cutter cannot reach is material
  // left too, and the two used to be one flag that no real job could ever
  // set (docs/cam/simulation.md#verdict). Tabs and offcuts do not count
  // against it: a tab is asked for and the toolpath says where it left it,
  // and a piece through cuts part from the part is not the part
  // (docs/cam/simulation.md#tabs-and-offcuts). Read it with leftoverArea and
  // `biggest`: several small places are the corners a round cutter cannot
  // reach, one large one is an operation nobody added.
  [[nodiscard]] bool Complete(double tolerance) const {
    return maxLeftover <= tolerance && grooveShallow <= tolerance &&
           !CutsNothing() &&
           std::all_of(
               threads.begin(), threads.end(),
               [&](const ThreadVerdict& t) { return t.Good(tolerance); }) &&
           std::none_of(fits.begin(), fits.end(),
                        [](const FitVerdict& f) { return f.Short(); });
  }
  // The program never touches the stock. Leftover is measured only where the
  // tool went, so such a program has none and read as complete -- the chess
  // rook's crenel setups posted seven-line files for every run of a day, each
  // "verified clean". Safe, and not a job: `complete` is false.
  [[nodiscard]] bool CutsNothing() const { return untouchedFraction >= 0.999; }
};

// Everything the check produced, for a caller that wants to SHOW it rather
// than just pass or fail (the leftover painted on the part,
// docs/cam/gui.md#what-the-viewport-draws).
struct Inspection {
  Verdict verdict;
  HeightMap machined;
  HeightMap designed;
  std::vector<uint8_t> touched; // 1 where a cutting move swept this cell
  bool ok = false; // false = there was nothing to check (no stock, no bodies)
};

// A fixture as the verifier sees it: its bodies already in SETUP (bed) space
// -- never through BodiesInSetupFrame, whose frame turns the part and not
// the vise -- and the name a finding reports it by.
struct FixtureSolid {
  uint64_t id = 0;
  std::string name;
  std::vector<Body> bodies;
};

// `reliefs`: where the toolpaths deliberately cut into the model (corner
// relief, R4) -- cells within a relief's radius are not counted as gouge.
// `tabs`: where they deliberately did NOT cut -- material at or below a tab's
// top, within its radius, is not counted as leftover.
[[nodiscard]] Inspection
Inspect(const std::string& gcode, const Setup& setup,
        const std::vector<Body>& bodies, double res = 0.25,
        const std::vector<Toolpath::Relief>& reliefs = {},
        const std::vector<Toolpath::Tab>& tabs = {},
        const std::vector<FixtureSolid>& fixtures = {},
        const OtherSetups* others = nullptr,
        const std::vector<Toolpath::Groove>& grooves = {},
        const std::vector<Toolpath::Thread>& threads = {},
        const std::vector<Toolpath::Fit>& fits = {});

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
            const std::vector<Toolpath::Tab>& tabs = {},
            const std::vector<FixtureSolid>& fixtures = {},
            const OtherSetups* others = nullptr,
            const std::vector<Toolpath::Groove>& grooves = {},
            const std::vector<Toolpath::Thread>& threads = {},
            const std::vector<Toolpath::Fit>& fits = {});

// The threads of the job's parts that the job does not cut
// (docs/cam/simulation.md#what-the-job-does-not-cut), added to `v.uncut`:
// a thread is cut when an enabled thread mill of the job (`setups`, every
// setup) targets its face and generated a thread (`milled`: the ops whose
// toolpath declared one, by id). The holes are `Inspect`'s, which has the
// machined map they are judged on.
void AddUncutThreads(const std::vector<Setup>& setups,
                     const std::vector<Body>& rawBodies,
                     const std::vector<uint64_t>& milled, Verdict& v);

// Every toleranced wall `fits` declares, judged from `program` (in setup
// space): the walls declared by several operations (a bore and its spring
// pass) are one wall, cut by all of them
// (docs/cam/simulation.md#tolerance-bands).
[[nodiscard]] std::vector<FitVerdict>
JudgeFits(const GProgram& program, const Setup& setup,
          const std::vector<Toolpath::Fit>& fits);

// The holes of the job's parts whose bores no setup leaves in air, into
// `v.uncut`: this setup's `machined` map where the bore stands up in it,
// and `others` (may be null) for the rest
// (docs/cam/simulation.md#what-the-job-does-not-cut).
void FindUncutHoles(const Setup& setup, const std::vector<Body>& rawBodies,
                    const HeightMap& machined, const OtherSetups* others,
                    Verdict& v);

// The verdict in the words a person needs, one line each, most severe first
// (docs/cam/simulation.md#verdict). Empty only when there is nothing to say.
struct VerdictLine {
  enum class Level { Good, Warn, Bad };
  Level level = Level::Good;
  std::string text;
};

// The chuck and the tailstock against a program in setup space, into `v`
// (Verdict::holdingCollisions). Nothing for a setup not held in the rotary.
void CheckHolding(const GProgram& program, const Setup& setup,
                  const std::vector<Body>& bodies, Verdict& v);

// The MACHINE's lines of the verdict
// (docs/cam/setup.md#the-machines-work-area): the parts' reach beyond the
// travel, or the sheet's overhang when the parts fit -- as WARN lines, never
// refusals. One helper for the panel and the RPC, so the two verdicts cannot
// disagree about the machine. `paths`, when given, adds how much cutting was
// clipped off at the travel -- on its own line, naming the operations, when
// the parts fit and the clip is the window's doing.
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
