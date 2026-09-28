#pragma once

#include "core/cam/CamJob.h" // Setup
#include "core/cam/CamRotaryFeed.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reading a posted G-code file back (docs/cam/simulation.md#one-engine).
//
// Split out of CamVerify.h, which was two subjects in one file: this half
// turns text into moves and knows nothing about stock, and the other half is
// the height map that cuts those moves into a block. The seam matters beyond
// tidiness -- a 4th axis touches the PARSER (an A word is a word like any
// other) and needs a different stock model entirely
// (docs/cam/rotary.md#verifying-a-turning-part), so the two halves change for
// different reasons and at different times.
//
// The parser reads the FILE, not the toolpath that produced it. That
// independence is the point of the whole module: a bug in the post, in modal
// state, or in linking is invisible to anything that inspects
// `std::vector<Move>`, and one of those (a NaN retract) had already shipped.
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
  // The rotary word, modal like everything else. `hasA` false means the file
  // has not stated one yet -- which is a fact about the file and not a zero.
  //
  // Read rather than judged. The parser's job is to say what the file says;
  // whether a moving A can be simulated is a question about the STOCK MODEL,
  // and it is answered where the stock is chosen (`Inspect`), not here.
  bool hasA = false;
  double a = 0.0;
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
// `rotaryWord` is the letter the machine drives its rotary with (A or B,
// CamMount.h); the other one is a warning, never silence -- a B word used to
// vanish into `default:`.
[[nodiscard]] GProgram ParseGcode(std::string_view text, char rotaryWord = 'A');
// Does the A axis MOVE in this program? False for a 3-axis file and for an
// indexed one, which states one constant A before the first cut -- and those
// two are the same question for every check downstream, because in both the
// part stands still while the tool moves.
[[nodiscard]] bool RotatesWhileCutting(const GProgram& p);

// The posted file, back in SETUP space. A file's coordinates are relative to
// the work origin (the operator's zero); everything that compares a file to
// the model — the verifier, the simulator, the backplot — works in setup
// space. This is the one place the origin is added back, and it is the exact
// inverse of what `Emitter` subtracts
// (docs/cam/post.md#work-origin-in-the-file). It inverts the rotary's mount
// too (CamMount.h): the quarter turn, the sign of A, and which letter it is.
//
// Take this rather than ParseGcode wherever a Setup is in hand: a program
// parsed raw and compared against a framed model is off by the origin, which
// at origin (0,0,0) — every job before v68 — is invisible.
[[nodiscard]] GProgram ProgramInSetupSpace(std::string_view text,
                                           const Setup& setup);

// What a feed means when the part turns -- the controller's length, the
// tip's, and the F a turning cut is posted at -- lives in CamRotaryFeed.h,
// because the post needs it in a build without the engine.

} // namespace cam
