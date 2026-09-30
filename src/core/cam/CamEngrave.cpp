#include "core/cam/CamEngrave.h"

#include "core/cam/CamToolpath.h"
#include "core/cam/CamToolpathInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

// The engraving strategy (docs/cam/operations.md#engrave): the tool's axis ON
// the sketch's curves, its tip at the groove's depth below the surface the
// sketch lies on, the strokes in the order that travels least. The curves come
// resolved (CamSketchCurves.cpp); this turns them into moves.

namespace cam {

using namespace detail;

namespace {

// One target's strokes, framed and checked: a LABEL, cut whole before the
// next -- between its strokes the tool hops at a short retract, between two
// labels it goes up to safe Z.
struct Label {
  EngraveFrame frame;
};

// The nearest any stroke of `l` comes to `p` at a point a cut can start
// from: an open stroke's ends, any point of a closed one.
double DistanceTo(const Label& l, const glm::dvec2& p) {
  double best = std::numeric_limits<double>::max();
  for (size_t s = 0; s < l.frame.strokes.size(); s++) {
    const std::vector<glm::dvec2>& pts = l.frame.strokes[s];
    if (pts.empty())
      continue;
    if (l.frame.closed[s]) {
      for (const glm::dvec2& q : pts)
        best = std::min(best, glm::length(q - p));
    } else {
      best = std::min(
          {best, glm::length(pts.front() - p), glm::length(pts.back() - p)});
    }
  }
  return best;
}

} // namespace

namespace detail {

void GenerateEngrave(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp,
                     const GenerateContext* ctx) {
  if (op.targets.empty()) {
    tp.error = "an engraving follows sketch curves: target {feature_id: "
               "<sketch>, element: \"g<entity id>\"}";
    return;
  }
  const EngraveGroove groove = EngraveGrooveFor(op, tool);
  if (!groove.ok) {
    tp.error = groove.why;
    return;
  }
  // Every target, resolved, framed and found ON the surface -- or said why
  // not. One that cannot be cut is skipped by name and costs the others
  // nothing, as every multi-target operation does.
  std::vector<Label> labels;
  std::vector<std::string> refusals;
  for (size_t i = 0; i < op.targets.size(); i++) {
    const Target& target = op.targets[i];
    const SketchCurves* curves =
        ctx != nullptr ? FindSketchCurves(ctx->sketchCurves, target) : nullptr;
    std::string why;
    Label label;
    if (curves == nullptr) {
      why = "its curves were not resolved for this run -- is the job part's "
            "document open?";
    } else if (!curves->why.empty()) {
      why = curves->why;
    } else {
      label.frame = EngraveFrameOf(setup, *curves);
      if (!label.frame.ok)
        why = label.frame.why;
      else
        // The top is the surface the sketch lies on -- read from the model,
        // never from `topZ`, which is what the derivation reported it as --
        // and only where the model has one: a stroke off the part would be
        // cut in air, and one sunk into it at the wrong depth.
        why = EngraveSurfaceRefusal(bodies, label.frame);
    }
    if (!why.empty()) {
      refusals.push_back(op.targets.size() == 1
                             ? why
                             : "target " + std::to_string(i + 1) + " of " +
                                   std::to_string(op.targets.size()) + " (" +
                                   target.ref.name + "): " + why);
      continue;
    }
    labels.push_back(std::move(label));
  }
  if (labels.empty()) {
    tp.error = refusals.size() == 1 ? refusals.front()
                                    : "no target of this engraving could be "
                                      "followed. " +
                                          refusals.front();
    return;
  }
  tp.skipped = refusals;

  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  // Where one stroke ends ON the next one's start, the tool stays down: a
  // letter drawn as two strokes meeting at a corner is one cut.
  constexpr double kJoin = 1e-3;

  std::vector<bool> done(labels.size(), false);
  glm::dvec2 at = labels.front().frame.strokes.front().front();
  bool down = false; // the tool is in the material at `at`
  for (size_t n = 0; n < labels.size(); n++) {
    // Labels in the order the tool meets them: the authored first, then
    // whichever is nearest where the last one finished.
    size_t next = 0;
    if (n > 0) {
      double best = std::numeric_limits<double>::max();
      for (size_t i = 0; i < labels.size(); i++)
        if (!done[i])
          if (const double d = DistanceTo(labels[i], at); d < best - 1e-12) {
            best = d;
            next = i;
          }
    }
    done[next] = true;
    const EngraveFrame& frame = labels[next].frame;
    const double surface = frame.surfaceZ;
    const double tipZ = surface - groove.depth;
    // A hop between strokes stops a clearance over the surface: a
    // millimetre, or the tool's radius if that is less -- the clearance a
    // surfacing entry keeps over what it knows is there.
    const double hopZ = surface + std::min(1.0, tool.diameter * 0.5);
    // Onto a label from safe Z: a rapid down to the stock top (above it
    // there can be nothing), then the plunge feed.
    const double entryZ =
        std::clamp(std::max(hopZ, stockTop), hopZ, std::max(hopZ, setup.safeZ));
    // A hop that would pass through the part at the hop height -- a boss
    // between two strokes -- goes over at safe Z instead.
    const auto hopClear = [&](const glm::dvec2& a, const glm::dvec2& b) {
      const double len = glm::length(b - a);
      const int steps = std::max(1, static_cast<int>(std::ceil(len / 0.5)));
      std::vector<glm::vec3> probes;
      probes.reserve(static_cast<size_t>(steps) + 1);
      for (int i = 0; i <= steps; i++) {
        const glm::dvec2 p = a + (b - a) * (static_cast<double>(i) / steps);
        probes.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y),
                            static_cast<float>(hopZ));
      }
      // One classifier per body for the whole hop (CADShape::ContainsPoints).
      for (const Body& body : bodies)
        if (body.shape.IsValid())
          for (const bool in : body.shape.ContainsPoints(probes, 1e-6))
            if (in)
              return false;
      return true;
    };
    const auto enter = [&](const glm::dvec2& start) {
      if (down)
        Emit(tp, Move::Kind::Rapid, {at.x, at.y, setup.safeZ}, 0.0);
      Emit(tp, Move::Kind::Rapid, {start.x, start.y, setup.safeZ}, 0.0);
      if (entryZ < setup.safeZ - 1e-9)
        Emit(tp, Move::Kind::Rapid, {start.x, start.y, entryZ}, 0.0);
      // Straight down at the plunge feed: a V-bit's point cuts at its
      // centre, and a 3-degree ramp is longer than most strokes.
      Emit(tp, Move::Kind::Plunge, {start.x, start.y, tipZ}, tool.feedPlunge);
    };

    const std::vector<StrokeRun> runs =
        OrderStrokes(frame.strokes, frame.closed, at);
    bool first = true;
    for (const StrokeRun& run : runs) {
      const glm::dvec2 start = run.pts.front();
      if (first) {
        enter(start); // a new label: from safe Z
      } else if (glm::length(start - at) <= kJoin) {
        // carries straight on, still down
      } else if (hopClear(at, start)) {
        Emit(tp, Move::Kind::Rapid, {at.x, at.y, hopZ}, 0.0);
        Emit(tp, Move::Kind::Rapid, {start.x, start.y, hopZ}, 0.0);
        Emit(tp, Move::Kind::Plunge, {start.x, start.y, tipZ}, tool.feedPlunge);
      } else {
        enter(start);
      }
      first = false;
      for (size_t i = 1; i < run.pts.size(); i++)
        Emit(tp, Move::Kind::Cut, {run.pts[i].x, run.pts[i].y, tipZ},
             tool.feedCut);
      down = true;
      at = run.pts.back();
      // What the verifier is told was cut on purpose: this stroke, swept by
      // the tool's own shape down to the tip.
      Toolpath::Groove g;
      g.pts = run.pts;
      g.tipZ = tipZ;
      g.topZ = surface;
      g.tool = tool;
      tp.grooves.push_back(std::move(g));
    }
  }
  if (down)
    Emit(tp, Move::Kind::Rapid, {at.x, at.y, setup.safeZ}, 0.0);
}

} // namespace detail

} // namespace cam
