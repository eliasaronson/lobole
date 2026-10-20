// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// An assembly sequence as something to WATCH
// (docs/assemblies/README.md#playing-and-filming-a-sequence): each step's
// occurrences shown at its turn, travelling from where they start to their
// seat along the path the sequence check resolved, and staying home after.
// Pure: what a step moves and along what comes in already resolved (the
// app's resolver reads them off the check), and a pose is asked of it with
// the occurrence's seat -- its solved placement -- so the same timeline
// follows the document it is shown over.
namespace sequence {

// One step, resolved.
struct TimelineStep {
  // What it moves: one occurrence, or a group moved rigidly together.
  std::vector<uint64_t> occurrences;
  // Placed where it is: the base, or a part with nothing to go on along.
  bool base = false;
  glm::dvec3 point{0.0};           // on the path's line, assembly frame
  glm::dvec3 moves{0.0, 0.0, 1.0}; // unit: the way it moves going home
  double travelMm = 0.0;           // from its seat to where it starts
  // Turned about `moves` (right hand) over the path, from where it starts
  // to its seat: a bayonet's turn, or a thread's turn a pitch.
  double turnDeg = 0.0;

  // Seconds, set by Time(): shown from `appear`, still until `start`,
  // moving until `end`, home after.
  double appear = 0.0, start = 0.0, end = 0.0;
};

// How long things take. Durations, not speeds: the speed comes from the
// assembly's size (Time()).
struct Timing {
  // The assembly's size (its bounds' diagonal) is crossed in this long.
  double crossSeconds = 3.0;
  // A step's parts stand at their start this long before they move.
  double holdSeconds = 0.35;
  // A move takes at least this long, so a short press is still seen.
  double minMoveSeconds = 0.4;
  // A thread turns at most this many turns a second.
  double turnsPerSecond = 3.0;
  // Home at the end, held this long.
  double endSeconds = 1.5;
};

struct Timeline {
  std::vector<TimelineStep> steps;
  double sizeMm = 0.0;
  double speedMmPerS = 0.0;
  double seconds = 0.0; // the whole of it, the end's hold included
  // occurrence -> its step
  std::map<uint64_t, size_t> stepOf;
};

// Lays the steps out in time: one speed for the whole film, `sizeMm` (the
// assembly's bounds' diagonal, > 0) over `timing.crossSeconds`.
Timeline Time(std::vector<TimelineStep> steps, double sizeMm,
              const Timing& timing = {});

// Where `occurrence` stands at `t` seconds, given its seat (its solved
// placement, assembly frame). Null: not shown yet. An occurrence no step
// moves stands at its seat throughout.
std::optional<glm::dmat4> PoseAt(const Timeline& tl, uint64_t occurrence,
                                 const glm::dmat4& seat, double t);

// How far from its seat a step's parts are at `t`, mm along the path (0
// home, travelMm at the start).
double RemainingAt(const TimelineStep& s, double t);

} // namespace sequence
