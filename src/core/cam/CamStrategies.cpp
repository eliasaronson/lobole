#include "core/cam/CamToolpath.h"

#include "core/cam/CamNames.h"
#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamDropCutter.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

// The strategies themselves -- profile, pocket, adaptive, face, drill and
// edge round -- and the one switch that picks between them. The finishing
// passes (planar and rotary) are in CamSurfaceStrategies.cpp.

// One of six files split from CamToolpath.cpp
// (docs/architecture-review-2026-09-06.md, stage 2).

namespace cam {

using namespace detail;

namespace {

// meant twelve of those, and it reads exactly as it sounds — "does one lap,
// lifts and goes back a bit, then down and to the next circle". The link is
// one step-over through the band the next ring is about to cut anyway, so
// there is nothing to lift over. The distance check is what keeps it honest:
// two DISJOINT regions at the same inset are far apart, and linking those at
// depth would drag the cutter across whatever lies between them.
void EmitRing(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double stockTop,
              double step, double entryZ);

// The XY contour(s) a profile follows, and the region a pocket clears, when
// How far above a pocket floor to look for material standing in one of its
// inner wires. Small enough that a boss of any real height is found, large
// enough to clear the tolerance a section carries — and the section at the
// floor ITSELF is ambiguous, because the floor's own face is there.
constexpr double kIslandProbe = 0.05;

// the operation was made by clicking a face: the face's own wires, the same
// at every depth. Empty when the operation has no target, which is the
// caller's signal to fall back to sectioning the model.
// The inner wires of a level face that are genuine OPENINGS: an inner wire
// with nothing standing in it. It is the same question `useOutline` asks
// about a pocket floor (below), asked for the opposite purpose -- there to
// know what may be driven over, here to know what may be cut out -- so it is
// one test and not two. A BOSS has an inner wire too, and cutting round the
// inside of one removes the boss.
std::vector<Loop> FaceOpenings(const std::vector<Body>& bodies,
                               const FaceRegion& region) {
  std::vector<Loop> holes;
  for (const Loop& l : region.loops)
    if (l.hole)
      holes.push_back(l);
  if (holes.empty())
    return holes;
  Clipper2Lib::Paths64 section;
  for (const Loop& l : SectionLoops(bodies, region.z + kIslandProbe))
    section.push_back(ToClipper(l.pts));
  const Clipper2Lib::Paths64 standing =
      Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd);
  std::vector<Loop> out;
  for (const Loop& l : holes) {
    const bool material =
        !Clipper2Lib::Intersect(Clipper2Lib::Paths64{ToClipper(l.pts)},
                                standing, Clipper2Lib::FillRule::NonZero)
             .empty();
    if (!material)
      out.push_back(l);
  }
  return out;
}

Clipper2Lib::Paths64 TargetPaths(const Operation& op,
                                 const std::vector<Body>& bodies, bool holesToo,
                                 double toolRadius, std::string& error) {
  if (op.targets.empty())
    return {};
  // A wall names one contour at every depth — the case an opening that goes
  // all the way through has, because it has no floor to name.
  if (IsWallFace(bodies, op.targets.front().ref,
                 op.targets.front().jobPartId)) {
    const WallContour wall = PickedWallContour(
        bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
    if (!wall.ok) {
      error = wall.why;
      return {};
    }
    return Clipper2Lib::Paths64{ToClipper(wall.loop.pts)};
  }
  const FaceRegion region = PickedFaceRegion(
      bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
  if (!region.ok) {
    error = region.why;
    return {};
  }
  Clipper2Lib::Paths64 outer, islands;
  for (const Loop& l : region.loops)
    (l.hole ? islands : outer).push_back(ToClipper(l.pts));
  if (!holesToo || islands.empty())
    return outer;
  // `useOutline` passes over the OPENINGS in a floor. An inner wire is only
  // an opening if nothing stands in it: a hole has no material at or above
  // the floor, and the cutter bridges it harmlessly — a hole narrower than
  // the cutter is the case the option exists for. A BOSS has the same kind of
  // wire and the opposite meaning, and driving over one is a full-width cut
  // at full depth into standing material. So the option drops openings and
  // keeps islands, whatever it is set to: no flag may point the cutter at
  // material nobody asked it to remove.
  if (op.useOutline) {
    // The material standing just above the floor. EVEN-ODD over every loop of
    // the section, because the answer is a question of NESTING: inside the
    // part is filled, inside the pocket is not, inside a boss standing in
    // that pocket is again. Taking only the outward-wound loops instead reads
    // the part's own outline as "material everywhere", which keeps every hole
    // as an island and makes the option do nothing at all.
    Clipper2Lib::Paths64 section;
    for (const Loop& l : SectionLoops(bodies, region.z + kIslandProbe))
      section.push_back(ToClipper(l.pts));
    const Clipper2Lib::Paths64 standing =
        Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd);
    Clipper2Lib::Paths64 keep;
    for (const Clipper2Lib::Path64& island : islands) {
      const bool material =
          !Clipper2Lib::Intersect(Clipper2Lib::Paths64{island}, standing,
                                  Clipper2Lib::FillRule::NonZero)
               .empty();
      // Something stands in it: an island, always, whatever the option says.
      if (material) {
        keep.push_back(island);
        continue;
      }
      // Nothing stands in it — but SIZE decides whether crossing is a favour
      // or a waste. An opening the cutter cannot enter is not an obstacle it
      // can go round: the detour only leaves a sliver. An opening it CAN
      // enter is a hole the job removes anyway, and clearing across it is
      // time spent cutting air — the whole middle of a flange recess, when
      // the driver cutout takes that middle out regardless.
      const Clipper2Lib::Paths64 room = Clipper2Lib::InflatePaths(
          Clipper2Lib::Paths64{island}, -toolRadius * kScale,
          Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
      if (!room.empty())
        keep.push_back(island); // the tool fits inside: go around it
    }
    islands = std::move(keep);
    if (islands.empty())
      return outer;
  }
  return Clipper2Lib::Difference(outer, islands,
                                 Clipper2Lib::FillRule::NonZero);
}

// Profile: offset the section outward (or inward) by the tool radius and
// follow it, once per depth pass.
// Defined with the other loop surgery below (R4); the profile and the pocket
// both call it.
std::vector<glm::dvec2> AddCornerRelief(const std::vector<glm::dvec2>& pts,
                                        bool materialInside, double length,
                                        CornerRelief style,
                                        std::vector<glm::dvec2>* corners);

void GenerateProfile(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  const double radius = tool.diameter * 0.5;
  const double bottom = op.bottomZ + op.stockToLeave;
  const std::vector<double> passes = DepthPasses(op);
  std::string why;
  // A wall names its contour by POSITION, not by value. The outline it lies
  // on changes with depth wherever a feature opens part-way down, so the
  // mid-height loop `PickedWallContour` returns is right at one height and
  // wrong at the rest — the pick is re-resolved per pass below, against the
  // projection. A floor region and a targetless cut keep their own shapes.
  const bool wallPick =
      !op.targets.empty() &&
      IsWallFace(bodies, op.targets.front().ref, op.targets.front().jobPartId);
  WallContour wall;
  if (wallPick) {
    wall = PickedWallContour(bodies, op.targets.front().ref, 0.02,
                             op.targets.front().jobPartId);
    if (!wall.ok) {
      tp.error = wall.why;
      return;
    }
  }
  // A point ON each contour this operation follows, which is how a pick
  // survives depth: the loop it names can merge with a neighbour lower down,
  // and proximity finds the merged boundary where an index or a shape would
  // not. One seed for a wall; one per OPENING for a face cut inside.
  std::vector<glm::dvec2> seeds;
  if (wallPick)
    seeds.push_back(wall.at);
  // Cutting INSIDE a level face means cutting its openings. It cannot mean
  // anything else -- offsetting the face's own outline inward carves a trench
  // a whisker inside the part, full depth, all the way round, which is the
  // failure the refusal below describes and which this pick used to walk
  // straight into, since `TargetPaths` handed a profile the outer wire alone.
  bool openingsPick = false;
  if (!wallPick && !op.targets.empty() && op.side == Side::Inside) {
    const FaceRegion region = PickedFaceRegion(
        bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
    if (!region.ok) {
      tp.error = region.why;
      return;
    }
    std::vector<Loop> openings = FaceOpenings(bodies, region);
    // The bores the drill takes are not this cutter's business.
    if (op.drilledBores > 0.0) {
      std::vector<Loop> keep;
      for (const Loop& l : openings)
        if (l.boreDiameter <= 0.0 ||
            l.boreDiameter > op.drilledBores + kStockFit)
          keep.push_back(l);
      openings = std::move(keep);
    }
    if (openings.empty()) {
      // Both refusals open with the same words on purpose: "nothing to cut
      // in that face" is the state a caller acts on -- a part with no
      // openings is a part that needs no openings operation -- and the rest
      // of the sentence says which kind of nothing it is.
      tp.error =
          op.drilledBores > 0.0
              ? "nothing to cut in that face: every opening in it is a bore "
                "of " +
                    Round2(op.drilledBores) +
                    " mm or less, which `drilled_bores` leaves to the drill"
              : "nothing to cut in that face: it has no openings. Pick a face "
                "that HAS one (every opening in it is cut), or the wall of "
                "the opening you mean -- an inside cut on a face with none "
                "would follow the part's own outline and trench round it.";
      return;
    }
    for (const Loop& l : openings)
      if (!l.pts.empty())
        seeds.push_back(l.pts.front());
    openingsPick = true;
  }
  const Clipper2Lib::Paths64 picked =
      wallPick || openingsPick ? Clipper2Lib::Paths64{}
                               : TargetPaths(op, bodies, /*holesToo=*/false,
                                             tool.diameter * 0.5, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  // The outline the cutter may follow at each depth: the part's projection
  // from the operation's top down to that pass, NOT its cross-section there
  // (ProjectedOutlines, and docs/known-bugs.md for the gouge that is).
  const std::vector<std::vector<Loop>> projected =
      ProjectedOutlines(bodies, op.topZ, passes);
  // Without a target a profile follows the part's own outline, which is what
  // "cut it out" means and is nonsense for an inside cut: offsetting the
  // perimeter INWARD by a tool radius carves a trench a whisker inside the
  // part, the full depth, all the way round. The first dogfooding round asked
  // for a slot this way and got a 15 mm gouge round the whole outline.
  if (seeds.empty() && picked.empty() && op.side == Side::Inside) {
    tp.error = "an inside profile needs the contour to cut inside of — pick "
               "the wall of the opening. Without one this would follow the "
               "part's own outline and cut a trench round it.";
    return;
  }
  // The contour of the LAST level that had one, kept so a through cut can
  // carry it down past the bottom of the model. `bottomZ` is deliberately
  // below the part — into the spoilboard — and sectioning finds nothing down
  // there, so stopping when the model runs out leaves the part welded to the
  // sheet: the exact failure tabs exist to control.
  Clipper2Lib::Paths64 lastFound;
  // Why nothing came out, if nothing does — see the same field in the pocket.
  std::string tooTight;
  // Where each loop's previous depth pass finished, so the next one carries on
  // from there instead of flying back across the opening at safe height.
  std::vector<double> resume;
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    std::vector<Loop> solid;
    if (picked.empty()) {
      if (!seeds.empty()) {
        // The loop of THIS depth's projection that each pick lies on. One may
        // have merged with a neighbour since the mid-height section named it;
        // a merged boundary is the right one to follow, and proximity finds
        // it either way. Two seeds landing on the same merged loop cut it
        // once.
        for (const glm::dvec2& seed : seeds) {
          const Loop* nearest = nullptr;
          double best = std::numeric_limits<double>::max();
          for (const Loop& l : projected[pass]) {
            const double d = DistanceToLoop(l.pts, seed);
            if (d < best) {
              best = d;
              nearest = &l;
            }
          }
          if (nearest == nullptr)
            continue;
          bool already = false;
          for (const Loop& have : solid)
            already = already || have.pts == nearest->pts;
          if (!already)
            solid.push_back(*nearest);
        }
      } else {
        for (const Loop& l : projected[pass])
          if (!l.hole)
            solid.push_back(l);
      }
      for (Loop& l : solid)
        MakeCounterClockwise(l);
    }
    const double delta = op.side == Side::Inside ? -(radius + op.stockToLeave)
                         : op.side == Side::On   ? 0.0
                                                 : radius + op.stockToLeave;
    Clipper2Lib::Paths64 contour = picked.empty() ? ToClipper(solid) : picked;
    if (contour.empty() && lastFound.empty() && picked.empty() &&
        seeds.empty()) {
      // A pass below the model with no pass above it to inherit from -- a
      // ONE-pass through cut, which a finishing pass is (R6). The outline
      // to carry down is the model's own at its bottom.
      double modelBottom = std::numeric_limits<double>::max();
      for (const Body& b : bodies) {
        glm::vec3 bmin, bmax;
        b.shape.GetBoundingBox(bmin, bmax);
        modelBottom = std::min(modelBottom, static_cast<double>(bmin.z));
      }
      if (modelBottom < op.topZ) {
        const std::vector<std::vector<Loop>> atBottom =
            ProjectedOutlines(bodies, op.topZ, {modelBottom + 1e-3});
        std::vector<Loop> low;
        if (!atBottom.empty())
          for (const Loop& l : atBottom[0])
            if (!l.hole)
              low.push_back(l);
        for (Loop& l : low)
          MakeCounterClockwise(l);
        contour = ToClipper(low);
      }
    }
    if (contour.empty())
      contour = lastFound; // below the model: keep cutting what was there
    else
      lastFound = contour;
    if (contour.empty())
      continue; // above the model, where there has never been anything
    const double lead = op.leadArcs && op.side != Side::On ? radius : 0.0;
    Clipper2Lib::Paths64 paths;
    if (op.side != Side::Inside) {
      // Outside or on: one offset over every contour at once, because two
      // outlines close together must UNION rather than cross each other.
      paths = delta == 0.0 ? contour : Offset(contour, delta);
    } else {
      // Inside: the contours are separate openings and cannot merge, so each
      // is offset on its own -- which is what lets an opening that cannot be
      // cut say so BY NAME instead of taking the whole operation down with
      // it. One operation now follows every opening in a face, and one narrow
      // slot among five is not a reason to cut none of them.
      for (const Clipper2Lib::Path64& src : contour) {
        const Clipper2Lib::Paths64 one =
            Offset(Clipper2Lib::Paths64{src}, delta);
        if (one.empty()) {
          if (tooTight.empty())
            tooTight = TooTightForTool(Clipper2Lib::Paths64{src}, tool,
                                       op.stockToLeave, "opening");
          if (pass == 0)
            tp.skipped.push_back(TooTightForTool(
                Clipper2Lib::Paths64{src}, tool, op.stockToLeave, "opening"));
          continue;
        }
        // A lead arc inside an opening needs a radius more of room than the
        // cut itself: that opening is left, saying so, rather than swept into
        // the far wall.
        if (lead > 0.0 &&
            Offset(Clipper2Lib::Paths64{src}, delta - radius).empty()) {
          if (pass == 0)
            tp.skipped.push_back(
                "an opening here has no room for a lead arc (it needs " +
                Round2(radius) +
                " mm beyond the cut): turn lead arcs off for this operation, "
                "or use a smaller tool");
          continue;
        }
        paths.insert(paths.end(), one.begin(), one.end());
      }
    }
    for (size_t loop = 0; loop < paths.size(); loop++) {
      Clipper2Lib::Path64& path = paths[loop];
      Orient(path, op.climb, op.side == Side::Inside);
      // Tabs on EVERY pass, not only the last one. A tab is a pillar from the
      // bottom of the cut up to `tabHeight`, and a pass that ran through it
      // on the way down left only the part of it below that pass — asking for
      // a 3.8 mm tab and getting 1 mm, which is a part that breaks free under
      // the cutter and is thrown. EmitLoop lifts to max(z, tabZ), so a pass
      // that never reaches the tab top is unaffected.
      //
      // Each loop remembers where its previous pass finished, PER LOOP: a
      // contour can offset into several, and handing loop B the point where
      // loop A stopped would start it somewhere on B that means nothing.
      if (resume.size() <= loop)
        resume.resize(loop + 1, 0.0);
      std::vector<glm::dvec2> pts = FromClipper(path);
      if (op.cornerRelief != CornerRelief::None && op.side != Side::On) {
        std::vector<glm::dvec2> corners;
        pts = AddCornerRelief(pts, op.side == Side::Outside, radius,
                              op.cornerRelief, &corners);
        // Told to the verifier once per corner, not once per pass.
        if (pass == 0)
          for (const glm::dvec2& c : corners)
            tp.reliefs.push_back({c, radius * 2.0});
      }
      // The tabs are recorded from the LAST pass only. Every pass lifts over
      // them, but it is the bottom one that leaves them standing, and a wall
      // pick re-resolves its contour at each depth -- so the bottom pass is
      // the one whose polygon says where the pillars actually are.
      const bool lastPass = pass + 1 == passes.size();
      resume[loop] =
          EmitLoop(tp, setup, tool, pts, z, setup.safeZ, stockTop, op.tabCount,
                   op.tabWidth, bottom + op.tabHeight,
                   pass == 0 ? op.topZ : passes[pass - 1], Entry::Ramp,
                   resume[loop], /*retract=*/true, lead,
                   op.side == Side::Outside, lastPass ? &tp.tabs : nullptr);
    }
  }
  // Every opening skipped is an operation that cut nothing, so it fails as
  // one -- in the words of the first thing it could not do, not in a generic
  // "produced no toolpath". With SOME openings cut, the skipped ones stay a
  // warning and the rest of the job goes ahead.
  if (tp.moves.empty() && tp.error.empty())
    tp.error = !tp.skipped.empty() ? tp.skipped.front() : tooTight;
}

// The pocket's wall ring with its corner relief (R4), the inner rings as
// they are: a bone on an inner ring would reach past the wall ring into the
// wall. The material is OUTSIDE a pocket's ring. Told to the verifier once,
// on the first depth pass.
std::vector<glm::dvec2> WallRelief(const Operation& op, const Tool& tool,
                                   const std::vector<glm::dvec2>& ring,
                                   bool wallRing, Toolpath& tp) {
  if (!wallRing || op.cornerRelief == CornerRelief::None)
    return ring;
  const double radius = tool.diameter * 0.5;
  std::vector<glm::dvec2> corners;
  std::vector<glm::dvec2> out = AddCornerRelief(
      ring, /*materialInside=*/false, radius, op.cornerRelief, &corners);
  if (tp.reliefs.empty())
    for (const glm::dvec2& c : corners)
      tp.reliefs.push_back({c, radius * 2.0});
  return out;
}

// Pocket: clear the voids inside the part's outline. Concentric offsets
// inward until the region closes — islands fall out of the boolean, because
// material inside a pocket is simply not part of the region.
void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  // A clicked floor face IS the region to clear, at every depth: its outer
  // wire is the pocket wall and its inner wires are islands (docs/cam/gui.md).
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/true, tool.diameter * 0.5, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  // Why nothing came out, if nothing does. Kept across the depths and used
  // only when the whole operation ends up empty: a pocket that clears at one
  // depth and closes up at the next is doing the right thing, and a message
  // about that would be noise.
  std::string tooTight;
  for (const double z : DepthPasses(op)) {
    Clipper2Lib::Paths64 region = picked;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      if (loops.empty())
        continue;
      Clipper2Lib::Paths64 outers, holes;
      for (const Loop& l : loops)
        (l.hole ? holes : outers).push_back(ToClipper(l.pts));
      if (holes.empty())
        continue; // nothing enclosed at this level
      // The void = what the outline encloses minus the material.
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    // Collect the concentric rings before emitting any: the LAST non-empty
    // inset brackets the region's inscribed radius (a region shrinks to
    // nothing exactly when the offset passes it), and that is what sizes the
    // helix. First ring sits one radius in from the wall (plus stock).
    std::vector<Clipper2Lib::Paths64> rings;
    double inset = radius + op.stockToLeave;
    double lastInset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      lastInset = inset;
      rings.push_back(std::move(ring));
      inset += step;
      ring = Offset(region, -inset);
    }
    if (rings.empty()) {
      // The region closed up before the first pass — the cutter is wider than
      // the narrowest part of what it was asked to clear.
      if (tooTight.empty())
        tooTight = TooTightForTool(region, tool, op.stockToLeave, "pocket");
      continue;
    }

    // Does the ramp fit on the loop we would cut first? If not, the descent
    // on that loop would be far steeper than asked (C6).
    double longest = 0.0;
    for (const Clipper2Lib::Path64& path : rings.front())
      longest = std::max(longest, Perimeter(FromClipper(path)));
    const double descend = std::max(0.0, entryZ - z);
    const double rampLen =
        descend > 1e-9 ? descend / std::tan(glm::radians(kRampAngleDeg)) : 0.0;
    // The tool CENTRE's circle: the inscribed radius less the cutter and the
    // stock being left. A helix narrower than a tenth of the cutter is a
    // plunge with extra steps, so that is where the fallback stays.
    const double helixRadius = lastInset - radius - op.stockToLeave;
    const bool helix = rampLen > longest && helixRadius > tool.diameter * 0.1 &&
                       descend > 1e-9;

    if (helix) {
      // The innermost ring's centroid stands in for the incentre.
      const std::vector<glm::dvec2> inner = FromClipper(rings.back().front());
      glm::dvec2 centre(0.0);
      for (const glm::dvec2& p : inner)
        centre += p;
      centre /= static_cast<double>(inner.size());
      EmitHelix(tp, tool, centre, helixRadius, std::max(entryZ, z), z,
                setup.safeZ, stockTop, kDeflection);
      // Inside out: the helix cleared the middle, so each ring outward steps
      // into fresh material by one step-over. Outside-in after a helix would
      // drag the tool through uncut material to reach the wall.
      for (auto it = rings.rbegin(); it != rings.rend(); ++it)
        for (Clipper2Lib::Path64& path : *it) {
          Orient(path, op.climb, /*insideMaterial=*/true);
          EmitRing(tp, setup, tool,
                   WallRelief(op, tool, FromClipper(path),
                              &*it == &rings.front(), tp),
                   z, stockTop, step, /*entryZ=*/z);
        }
      Emit(tp, Move::Kind::Rapid,
           {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
    } else {
      for (Clipper2Lib::Paths64& paths : rings)
        for (Clipper2Lib::Path64& path : paths) {
          Orient(path, op.climb, /*insideMaterial=*/true);
          EmitRing(tp, setup, tool,
                   WallRelief(op, tool, FromClipper(path),
                              &paths == &rings.front(), tp),
                   z, stockTop, step, entryZ);
        }
      Emit(tp, Move::Kind::Rapid,
           {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
    }
    entryZ = z;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = tooTight;
}

void EmitRing(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double stockTop,
              double step, double entryZ) {
  if (pts.size() < 2)
    return;
  const bool atDepth =
      !tp.moves.empty() && std::abs(tp.moves.back().to.z - z) < 1e-9;
  if (atDepth) {
    const glm::dvec2 here(tp.moves.back().to.x, tp.moves.back().to.y);
    const std::vector<glm::dvec2> rotated = StartNearest(pts, here);
    // A step-over and a half: enough for the neighbouring ring, not enough to
    // reach a separate region.
    if (glm::length(rotated.front() - here) <= step * 1.5) {
      (void)EmitLoop(tp, setup, tool, rotated, z, setup.safeZ, stockTop, 0, 0.0,
                     z, z, Entry::AtDepth, 0.0, /*retract=*/false);
      return;
    }
  }
  (void)EmitLoop(tp, setup, tool, pts, z, setup.safeZ, stockTop, 0, 0.0, z,
                 entryZ, Entry::Ramp, 0.0, /*retract=*/false);
}

void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp);

// Adaptive: the same region a pocket clears, handed to libarea's Adaptive2d
// (C8). Everything about WHICH region and at what depths is shared with the
// pocket; only the clearing strategy differs, which is the point of putting
// the split here rather than in a second operation type's worth of geometry.
void GenerateAdaptive(const Setup& setup, const Operation& op, const Tool& tool,
                      const std::vector<Body>& bodies, Toolpath& tp,
                      GenerateContext* ctx) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  if (!AdaptiveAvailable()) {
    // Not a refusal: the job still has to cut. The concentric pocket is the
    // strategy this build has, and saying so beats producing nothing.
    GeneratePocket(setup, op, tool, bodies, tp);
    tp.strategy = "concentric (this build has no adaptive clearing)";
    return;
  }
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/true, tool.diameter * 0.5, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  // Why nothing came out, if nothing does — see the same field in the pocket.
  std::string tooTight;
  // One pass was handed to the caller's worker instead of run here. Every
  // later pass that is not cached goes the same way — the whole operation is
  // queued in one go, in depth order — and nothing below the queueing is
  // allowed to run on a partial result.
  bool deferred = false;
  for (const double z : DepthPasses(op)) {
    Clipper2Lib::Paths64 region = picked;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      Clipper2Lib::Paths64 holes;
      for (const Loop& l : loops)
        if (l.hole)
          holes.push_back(ToClipper(l.pts));
      if (holes.empty())
        continue;
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    if (region.empty())
      continue;
    std::vector<Loop> asLoops;
    for (const Clipper2Lib::Path64& path : region) {
      Loop l;
      l.pts = FromClipper(path);
      l.hole = false;
      asLoops.push_back(std::move(l));
    }
    // The pass: from the cache, from a worker later, or right here.
    const bool cached = ctx != nullptr && ctx->cache != nullptr;
    const uint64_t key =
        cached ? AdaptiveKey(setup, op, tool, asLoops, z, entryZ) : 0;
    const AdaptiveResult* hit = cached ? ctx->cache->Find(key) : nullptr;
    if (hit == nullptr && cached && ctx->defer) {
      ctx->pending.push_back(
          MakeAdaptiveRequest(setup, op, tool, asLoops, z, entryZ));
      deferred = true;
      entryZ = z;
      continue;
    }
    if (deferred) {
      entryZ = z;
      continue;
    }
    AdaptiveResult computed;
    if (hit == nullptr) {
      computed = AdaptiveClear(setup, op, tool, asLoops, z, entryZ);
      if (cached)
        ctx->cache->Put(key, computed);
    }
    const AdaptiveResult& cleared = hit != nullptr ? *hit : computed;
    if (cleared.error.empty() && cleared.moves.empty() &&
        op.side == Side::Outside) {
      // Adaptive is the one clearing strategy that reads `side`, and outside
      // a region whose own boundary is the stock there is nothing to clear —
      // it returns silence rather than a complaint. The concentric pocket
      // ignores `side` entirely, so the same operation cut one way and
      // vanished the other, under the generic "produced no toolpath".
      tp.error = "this operation clears OUTSIDE its boundary, and there is "
                 "nothing out there to cut — a pocket clears inside "
                 "(side: inside)";
      return;
    }
    if (!cleared.error.empty() && cleared.moves.empty()) {
      // Adaptive declined this region — a vertex layout the vendored library
      // throws on, a shape it cannot lead into, a start point it cannot find.
      // The pocket still has to be cleared, and the concentric strategy is
      // right here and cuts the same region: falling back is what the
      // no-library branch above already does, and giving up instead left the
      // user clicking "Clear a pocket" at a face that never changed
      // (docs/cam/dogfood-round.md). Which strategy cut it is REPORTED, not
      // hidden — a job that quietly changed strategy is a job whose feeds
      // were chosen for the other one.
      tp.moves.clear();
      GeneratePocket(setup, op, tool, bodies, tp);
      tp.strategy =
          "concentric — adaptive declined this shape (" + cleared.error + ")";
      return;
    }
    if (cleared.error.empty() && cleared.moves.empty() && tooTight.empty()) {
      // Adaptive returns SILENCE for a region its cutter cannot enter — no
      // moves and no complaint — so the width has to be measured here or the
      // operation lands on the generic "check its depths". This is the path a
      // real pocket takes: a derived pocket deep enough comes back as
      // `adaptive`, so it is the one a person actually hits.
      tooTight = TooTightForTool(region, tool, op.stockToLeave, "pocket");
    }
    if (!cleared.error.empty() && tp.error.empty())
      tp.error = cleared.error; // partial: say so, keep what it did cut
    tp.moves.insert(tp.moves.end(), cleared.moves.begin(), cleared.moves.end());
    entryZ = z;
  }
  if (deferred) {
    // Nothing this operation shows may be a partial cut: the caller decides
    // what to draw in the meantime (the previous toolpath, by preference).
    tp.moves.clear();
    tp.error.clear();
    tp.strategy.clear();
    tp.reliefs.clear();
    tp.tabs.clear();
    tp.pending = true;
    return;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = tooTight;
}

// Face: clear the stock top down to topZ over the whole stock rectangle.
void GenerateFace(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies, Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi)) {
    tp.error = "no bodies, so there is no stock to face";
    return;
  }
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  std::vector<glm::dvec2> rect = {
      {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
  const Clipper2Lib::Paths64 region{ToClipper(rect)};
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    // Start with the cutter centred ON the stock edge so the outside is
    // cleaned up, then spiral inward.
    double inset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      for (Clipper2Lib::Path64& path : ring) {
        Orient(path, op.climb, /*insideMaterial=*/true);
        EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, stockTop,
                 0, 0.0, z, entryZ);
      }
      inset += step;
      if (inset > glm::length(hi - lo))
        break; // guard: a degenerate stock must not spin forever
      ring = Offset(region, -inset);
    }
    entryZ = z;
    (void)radius;
  }
}

// The radius of a cylindrical face, for the one question a drill has to ask:
// is this tool the size of this hole?
bool BoreRadius(const Body& body, int idx, double& radius) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    if (i != idx)
      continue;
    BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
    if (surf.GetType() != GeomAbs_Cylinder)
      return false;
    radius = surf.Cylinder().Radius();
    return radius > 0.0;
  }
  return false;
}

// Corner relief (R4). `pts` is a TOOL-CENTRE loop -- the wall pass, one
// radius off the material -- and `materialInside` says which side of it the
// part is on (an outside profile: inside the loop; a pocket or an inside
// profile: outside it). A sharp corner of that loop is a corner of the
// material the round cutter cannot reach: on an outside profile it shows as
// a REFLEX vertex (the part's inner corner), on a pocket as a CONVEX one
// (the pocket's corner), and in both the material's corner lies away from the
// corner's own small wedge -- along `u - v`, the incoming direction minus the
// outgoing. Out that way by `length` and back is the dogbone; along the
// longer of the two walls and back is the T-bone. Corners gentler than 30
// degrees are left alone: the arcs an offset puts on convex corners are many
// small kinks, and a relief on each would be a serrated wall.
std::vector<glm::dvec2> AddCornerRelief(const std::vector<glm::dvec2>& pts,
                                        bool materialInside, double length,
                                        CornerRelief style,
                                        std::vector<glm::dvec2>* corners) {
  const size_t n = pts.size();
  if (n < 3 || style == CornerRelief::None || length <= 1e-9)
    return pts;
  double area2 = 0.0;
  for (size_t i = 0; i < n; i++) {
    const glm::dvec2& a = pts[i];
    const glm::dvec2& b = pts[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
  }
  const bool ccw = area2 > 0.0;
  std::vector<glm::dvec2> out;
  out.reserve(n + 12);
  constexpr double kMinTurnDeg = 30.0;
  for (size_t i = 0; i < n; i++) {
    const glm::dvec2& prev = pts[(i + n - 1) % n];
    const glm::dvec2& p = pts[i];
    const glm::dvec2& next = pts[(i + 1) % n];
    out.push_back(p);
    const glm::dvec2 din = p - prev, dout = next - p;
    const double lin = glm::length(din), lout = glm::length(dout);
    if (lin < 1e-9 || lout < 1e-9)
      continue;
    const glm::dvec2 u = din / lin, v = dout / lout;
    const double cross = u.x * v.y - u.y * v.x;
    const double turn =
        glm::degrees(std::atan2(std::abs(cross), glm::dot(u, v)));
    if (turn < kMinTurnDeg)
      continue;
    const bool leftTurn = cross > 0.0;
    const bool convex = leftTurn == ccw; // w.r.t. the loop's interior
    const bool reachable = materialInside ? convex : !convex;
    if (reachable)
      continue; // the cutter gets into this corner already
    glm::dvec2 dir;
    if (style == CornerRelief::Dogbone) {
      dir = u - v;
      if (glm::length(dir) < 1e-9)
        continue; // a 180-degree turn has no bisector to speak of
      dir = glm::normalize(dir);
    } else {
      dir = lin >= lout ? u : -v;
    }
    out.push_back(p + dir * length);
    out.push_back(p);
    if (corners != nullptr)
      corners->push_back(p);
  }
  return out;
}

// Drill: one plunge per target hole, at the hole's axis.
void GenerateDrill(const Setup& setup, const Operation& op, const Tool& tool,
                   const std::vector<Body>& bodies, Toolpath& tp) {
  if (op.targets.empty()) {
    tp.error = "drill needs target holes; pick the cylindrical faces to drill";
    return;
  }
  int found = 0;
  for (const Target& t : op.targets) {
    const ElementRef& ref = t.ref;
    for (const Body& body : bodies) {
      if (body.createdBy != ref.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(ref.name);
      if (!idx)
        continue;
      glm::vec3 origin(0.0f), axis(0.0f), xAxis(0.0f);
      if (!body.shape.FaceAxis(*idx, origin, axis, xAxis))
        continue; // not a cylinder: silently skipping would drill nothing
      // A plunge goes straight down, so a bore lying on its side is not a
      // hole this operation can make — it would be drilled somewhere the
      // part has no hole at all.
      if (std::abs(axis.z) < 0.99f) {
        tp.error = "that hole does not point at the spindle — index the setup "
                   "to bring it up, or drill it in another setup";
        return;
      }
      // The size the cutter makes IS the size of the hole. A 3 mm tool
      // plunged into a 6 mm bore leaves a 6 mm hole half cut and nothing said
      // so: the first dogfooding round posted exactly that, and it showed up
      // only as 42 mm2 of "leftover" among the tabs and the slug.
      double radius = 0.0;
      if (BoreRadius(body, *idx, radius)) {
        const double dia = radius * 2.0;
        if (tool.diameter > dia + 1e-6) {
          tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                     Round2(tool.diameter) + " mm and that hole is " +
                     Round2(dia) + " mm — it would not fit in";
          return;
        }
        if (tool.diameter < dia - 1e-3) {
          // A hole WIDER than the tool is bored (R3): a helix down at the
          // ramp angle on the circle the tool centre has to walk, then a
          // flat turn at the bottom -- and for a hole wider than two tools,
          // rings from the centre outwards by the stepover, the wall last.
          // The user pointed at a hole; the tool decides how it is made, the
          // same rule Break an edge follows. This used to be a refusal
          // ("a plunge makes a hole the size of the cutter"), which is true
          // and was the wrong answer.
          found++;
          const glm::dvec2 centre(origin.x, origin.y);
          const double wall = radius - tool.diameter * 0.5 - op.stockToLeave;
          if (wall < 1e-6) {
            tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                       Round2(tool.diameter) + " mm and that hole is " +
                       Round2(dia) + " mm: nothing is left to bore once " +
                       Round2(op.stockToLeave) + " mm is left on the wall";
            return;
          }
          glm::dvec3 stockLo(0.0), stockHi(0.0);
          const double stockTop =
              StockBoundsFramed(setup, bodies, stockLo, stockHi) ? stockHi.z
                                                                 : setup.safeZ;
          const double step = std::max(
              tool.diameter * std::clamp(op.stepover, 0.05, 0.95), 0.1);
          std::vector<double> rings;
          // A centre pass only when the cutter cannot reach the middle from
          // the wall ring: a 7 mm hole with a 6 mm tool is one ring.
          for (double r = wall; r > 1e-9; r -= step)
            rings.push_back(r);
          if (rings.back() > step * 0.5 && wall > tool.diameter * 0.5)
            rings.push_back(std::min(rings.back(), step * 0.5));
          std::sort(rings.begin(), rings.end()); // inner first, the wall last
          for (const double r : rings)
            EmitHelix(tp, tool, centre, r, op.topZ, op.bottomZ, setup.safeZ,
                      stockTop, kDeflection);
          Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
          break;
        }
      }
      found++;
      const glm::dvec3 centre(origin.x, origin.y, 0.0);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      // Down to the top of the cut at rapid, and only then at feed. Feeding
      // the whole way from Safe Z spends seconds per hole cutting air — the
      // same reason a profile rapids to the stock top before it ramps.
      if (op.topZ < setup.safeZ - 1e-9)
        Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, op.topZ}, 0.0);
      if (op.peck > 1e-9 && op.topZ - op.bottomZ > op.peck + 1e-9) {
        // Pecks (R3): down one peck, all the way back out to the hole's top
        // so the chips clear (the G83 reading, not G73's short lift), rapid
        // back to just above the floor of the last peck, and on.
        double z = op.topZ;
        while (z > op.bottomZ + 1e-9) {
          const double next = std::max(op.bottomZ, z - op.peck);
          Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, next},
               tool.feedPlunge);
          z = next;
          if (z > op.bottomZ + 1e-9) {
            Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, op.topZ}, 0.0);
            Emit(tp, Move::Kind::Rapid,
                 {centre.x, centre.y, z + std::min(op.peck * 0.1, 0.5)}, 0.0);
          }
        }
      } else {
        Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, op.bottomZ},
             tool.feedPlunge);
      }
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      break;
    }
  }
  if (found == 0)
    tp.error = "none of the drill targets resolved to a cylindrical face";
}

void GenerateEdgeRound(const Setup& setup, const Operation& op,
                       const Tool& tool, const std::vector<Body>& bodies,
                       Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  if (op.targets.empty()) {
    tp.error = "an edge break follows a wall — pick the vertical face whose "
               "top edge to round or chamfer";
    return;
  }
  const WallContour wall = PickedWallContour(
      bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
  if (!wall.ok) {
    tp.error = wall.why;
    return;
  }
  const EdgeBreak brk =
      EdgeBreakFor(tool, op.edgeSize, std::max(0.0, op.topZ - op.bottomZ));
  if (!brk.ok) {
    tp.error = brk.why;
    return;
  }
  const double r = tool.diameter * 0.5;
  const double top = op.topZ;
  // An opening is cut from inside itself, so "into the material" is the
  // direction that GROWS its contour; round the outside of a part and it is
  // the one that shrinks it.
  const double sign = wall.opening ? 1.0 : -1.0;
  const Clipper2Lib::Paths64 base{ToClipper(wall.loop.pts)};

  const auto lap = [&](double delta, double z) {
    Clipper2Lib::Paths64 paths =
        std::abs(delta) < 1e-9 ? base : Offset(base, delta);
    for (Clipper2Lib::Path64& path : paths) {
      Orient(path, op.climb, wall.opening);
      EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, stockTop,
               /*tabCount=*/0, /*tabWidth=*/0.0, /*tabZ=*/0.0,
               /*entryZ=*/top);
    }
  };

  if (tool.type == ToolType::VBit) {
    // One pass: the axis on the contour, down by the bevel's own depth.
    lap(0.0, top - brk.depth);
    return;
  }
  // Arc passes from a graze on the flat top round to the wall. The step is
  // the cusp control the stepover already means everywhere else.
  const double R = brk.size;
  const double arc = R * glm::half_pi<double>();
  const double step = std::max(1e-3, op.stepover * tool.diameter);
  const int passes = std::max(1, static_cast<int>(std::ceil(arc / step)));
  for (int i = 0; i <= passes; i++) {
    const double t =
        glm::half_pi<double>() *
        (1.0 - static_cast<double>(i) / static_cast<double>(passes));
    const double inward = R - (R + r) * std::cos(t);
    // Every Z here is the tool's LOWEST POINT, the control point the post and
    // the verifier both use — so the ball's centre height is r above it.
    lap(sign * inward, top - R + (R + r) * std::sin(t) - r);
  }
}

// Which generators read `op.targets.front()` and stop there. Drill walks the
// list itself (it is what "drill these holes" means) and Face takes no target
// at all; the other four resolve ONE contour, so several targets on one of
// them is several runs -- see GenerateFramed.

void GenerateOne(const Setup& setup, const Operation& op, const Tool& tool,
                 const std::vector<Body>& bodies, Toolpath& tp,
                 GenerateContext* ctx) {
  switch (op.type) {
  case OpType::Profile:
    GenerateProfile(setup, op, tool, bodies, tp);
    break;
  case OpType::Pocket:
    GeneratePocket(setup, op, tool, bodies, tp);
    break;
  case OpType::Face:
    GenerateFace(setup, op, tool, bodies, tp);
    break;
  case OpType::Drill:
    GenerateDrill(setup, op, tool, bodies, tp);
    break;
  case OpType::Adaptive:
    GenerateAdaptive(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::EdgeRound:
    GenerateEdgeRound(setup, op, tool, bodies, tp);
    break;
  case OpType::SurfaceParallel:
  case OpType::SurfaceWaterline:
  case OpType::SurfaceRotary:
    SurfaceThroughCache(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::Count:
    // Not a type. Named so the switch stays exhaustive and a REAL new type
    // still breaks the build rather than landing here.
    tp.error = "not an operation type";
    break;
  }
}

} // namespace

Toolpath GenerateFramed(const Setup& setup, const Operation& op,
                        const std::vector<Body>& bodies, GenerateContext* ctx) {
  Toolpath tp;
  tp.operationId = op.id;
  tp.toolNumber = op.toolNumber;

  const Tool* tool = setup.FindTool(op.toolNumber);
  if (tool == nullptr) {
    tp.error = "no tool " + std::to_string(op.toolNumber) + " in this setup";
    return tp;
  }
  if (tool->diameter <= 0.0) {
    tp.error = "tool diameter must be positive";
    return tp;
  }
  if (op.bottomZ >= op.topZ) {
    tp.error = "the bottom of the cut must be below its top";
    return tp;
  }

  // ONE RUN PER TARGET, for the generators that resolve a single contour.
  // Before this they read targets.front() and dropped the rest in silence:
  // five bolt holes picked into one operation bored the first and left four
  // standing, and the file posted at full length with nothing to say about
  // it. What comes out is what N one-target operations would have produced,
  // in the order the targets were picked; a target that cannot be followed
  // goes in `skipped` rather than failing the four that can.
  if (op.targets.size() > 1 && FollowsOneTargetAtATime(op.type)) {
    Operation one = op;
    for (size_t i = 0; i < op.targets.size(); i++) {
      one.targets = {op.targets[i]};
      Toolpath sub;
      GenerateOne(setup, one, *tool, bodies, sub, ctx);
      if (sub.error.empty() && sub.moves.empty() && !sub.pending)
        sub.error = "it produced no toolpath — check the operation's "
                    "depths against the model";
      if (!sub.error.empty()) {
        tp.skipped.push_back("target " + std::to_string(i + 1) + " of " +
                             std::to_string(op.targets.size()) + " (" +
                             op.targets[i].ref.name + "): " + sub.error);
        continue;
      }
      tp.pending = tp.pending || sub.pending;
      if (tp.strategy.empty())
        tp.strategy = sub.strategy;
      tp.moves.insert(tp.moves.end(), sub.moves.begin(), sub.moves.end());
      tp.reliefs.insert(tp.reliefs.end(), sub.reliefs.begin(),
                        sub.reliefs.end());
      tp.tabs.insert(tp.tabs.end(), sub.tabs.begin(), sub.tabs.end());
    }
    // Every target refused: there is no operation left, so it fails as one.
    if (tp.moves.empty() && !tp.pending && !tp.skipped.empty())
      tp.error = "no target of this operation could be followed. " +
                 tp.skipped.front();
  } else {
    GenerateOne(setup, op, *tool, bodies, tp, ctx);
  }
  if (tp.moves.empty() && tp.error.empty() && !tp.pending)
    tp.error = "this operation produced no toolpath — check its depths "
               "against the model";
  // Cut what fits: clipped HERE, at the one place every toolpath is made,
  // and not in GenerateAll -- the panel regenerates a verb's operation on
  // its own through this function, and a clip that lived only in the full
  // pass left the verb's path running off the bed while the RPC's file
  // stopped at it (found live, the morning after it shipped).
  glm::dvec3 bedLo(0.0), bedHi(0.0);
  if (tp.error.empty() && MachineBoundsFramed(setup, bodies, bedLo, bedHi)) {
    ClipToMachine(tp, bedLo, bedHi, setup.safeZ,
                  tool != nullptr ? tool->feedPlunge : 100.0);
    // Clipped away ENTIRELY: this operation is off the bed, which is not the
    // same as empty. The "produced no toolpath" check above runs BEFORE the
    // clip, so a station beyond the travel came back with no moves, no error
    // and `failed: 0` -- a file that posts at full length with a whole
    // station missing from it (found on a 1122 mm baffle, 900 mm of Y).
    if (tp.clippedMm > 0.0 && tp.CutLength() <= 0.0) {
      tp.moves.clear();
      tp.error =
          "every cut of this operation lies beyond " +
          (setup.machine.name.empty() ? std::string("the machine")
                                      : setup.machine.name) +
          "'s travel, so none of it is in the file: move the part on the "
          "stock, or cut it in a second setup with the sheet slid along";
    }
  }
  // The index this setup is cut at rides on every move, so the post emits it
  // and the machine is never asked to guess which way the part is facing.
  // How the part is CLAMPED never gets here: it is already baked into the
  // coordinates, and a 3-axis shop must not be handed an axis it lacks.
  if (EmitsRotary(setup))
    for (Move& m : tp.moves) {
      m.hasA = true;
      m.a = setup.rotationA;
    }
  return tp;
}

Toolpath Generate(const Setup& setup, const Operation& op,
                  const std::vector<Body>& bodies, GenerateContext* ctx) {
  return GenerateFramed(setup, op, BodiesInSetupFrame(setup, bodies), ctx);
}

} // namespace cam
