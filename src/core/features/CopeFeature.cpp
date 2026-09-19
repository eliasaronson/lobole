#include "core/features/CopeFeature.h"

#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace {

// The centreline a feature was swept along, when it is a bent path.
const CurveFeature* BentPathOf(const RegenContext& ctx, const Feature* f) {
  const auto* pipe = dynamic_cast<const PipeFeature*>(f);
  if (pipe == nullptr)
    return nullptr;
  const auto* curve =
      dynamic_cast<const CurveFeature*>(ctx.FindById(pipe->pathId));
  if (curve == nullptr || curve->kind != Curve3DKind::BentPath)
    return nullptr;
  return curve;
}

// Whether this cope lands on the neighbour's BEND, and the sentence that
// says so.
//
// A joint does not belong on a bend: the tube there is a torus, it has been
// thinned by the bending, and there is no straight to fit a cope against —
// so a cut that finds nothing at a bend is a DESIGN answer, not a kernel
// quirk, and blaming tangency sends the reader to open a gap that will not
// help. A bend occupies the path from one tangent point to the other, which
// is `radius * tan(angle/2)` either side of the corner it was drawn from.
// The solid to remove for a MITRE: everything on the far side of the plane
// through `at` whose normal is `keep`. A box big enough to swallow the
// member, placed so its near face IS that plane.
CADShape MitreCutter(const glm::dvec3& at, const glm::dvec3& keep,
                     double size) {
  // Built at the origin occupying -Z, so its top face is the cut plane, then
  // rotated to put -Z along -keep and moved to the joint.
  CADShape box = CADKernel::Instance().CreateBox(size, size, size);
  box.Translate(glm::vec3(static_cast<float>(-size * 0.5),
                          static_cast<float>(-size * 0.5),
                          static_cast<float>(-size)));
  const glm::dvec3 z(0.0, 0.0, 1.0);
  const glm::dvec3 k = glm::normalize(keep);
  const double dot = glm::clamp(glm::dot(z, k), -1.0, 1.0);
  if (dot < 1.0 - 1e-12) {
    glm::dvec3 axis = glm::cross(z, k);
    if (glm::length(axis) < 1e-12)
      axis = glm::dvec3(1.0, 0.0, 0.0); // antiparallel: any perpendicular
    box.RotateAbout(glm::vec3(0.0f), glm::vec3(glm::normalize(axis)),
                    std::acos(dot));
  }
  box.Translate(glm::vec3(at));
  return box;
}

// Where two members MEET: the closest approach of their CENTRELINES.
//
// Not their nearest ends — members are drawn LONG, a diameter past each node,
// because a plane through the very end of a tube only shaves a wedge off it
// instead of mitring it. So the node is where the axes come closest, whether
// or not either member overruns it.
struct Joint {
  bool found = false;
  glm::dvec3 at{0.0};     // the node
  glm::dvec3 mine{0.0};   // unit, along THIS member away from the node
  glm::dvec3 theirs{0.0}; // unit, along the neighbour away from the node
};

// Closest points of two segments, as parameters along each.
void SegmentClosest(const glm::dvec3& p0, const glm::dvec3& p1,
                    const glm::dvec3& q0, const glm::dvec3& q1, double& s,
                    double& t) {
  const glm::dvec3 u = p1 - p0, v = q1 - q0, w = p0 - q0;
  const double a = glm::dot(u, u), b = glm::dot(u, v), c = glm::dot(v, v);
  const double d = glm::dot(u, w), e = glm::dot(v, w);
  const double den = a * c - b * b;
  s = den < 1e-12 ? 0.0 : std::clamp((b * e - c * d) / den, 0.0, 1.0);
  t = c < 1e-12 ? 0.0 : std::clamp((b * s + e) / c, 0.0, 1.0);
  s = a < 1e-12 ? 0.0 : std::clamp((b * t - d) / a, 0.0, 1.0);
}

Joint JointBetween(const std::vector<glm::vec3>& ours,
                   const std::vector<glm::vec3>& theirs) {
  Joint j;
  if (ours.size() < 2 || theirs.size() < 2)
    return j;
  double best = 0.0;
  size_t bi = 0, bj = 0;
  double bs = 0.0, bt = 0.0;
  for (size_t i = 0; i + 1 < ours.size(); i++) {
    for (size_t k = 0; k + 1 < theirs.size(); k++) {
      double s = 0.0, t = 0.0;
      SegmentClosest(glm::dvec3(ours[i]), glm::dvec3(ours[i + 1]),
                     glm::dvec3(theirs[k]), glm::dvec3(theirs[k + 1]), s, t);
      const glm::dvec3 a = glm::dvec3(ours[i]) +
                           s * (glm::dvec3(ours[i + 1]) - glm::dvec3(ours[i]));
      const glm::dvec3 b =
          glm::dvec3(theirs[k]) +
          t * (glm::dvec3(theirs[k + 1]) - glm::dvec3(theirs[k]));
      const double d = glm::length(a - b);
      if (!j.found || d < best) {
        j.found = true;
        best = d;
        bi = i;
        bj = k;
        bs = s;
        bt = t;
        j.at = (a + b) * 0.5;
      }
    }
  }
  if (!j.found)
    return j;
  // Which way does each member run AWAY from the node? Along the SEGMENT
  // the node sits on, toward whichever of that segment's ends is further.
  //
  // Not toward the far end of the whole polyline: a bent member's far end
  // can lie in an entirely different direction from the leg the joint is on.
  // A front hoop's legs run UP from the floor and its far end comes back
  // DOWN on the other side, so a cross bar joining one foot was told the
  // hoop ran across the car — the same way the bar does — and the mitre was
  // refused as collinear.
  const auto away = [&](const std::vector<glm::vec3>& line, size_t seg,
                        double at) {
    const glm::dvec3 a(line[seg]), b(line[seg + 1]);
    const glm::dvec3 here = a + at * (b - a);
    const glm::dvec3 toA = a - here;
    const glm::dvec3 toB = b - here;
    const glm::dvec3 pick = glm::length(toA) >= glm::length(toB) ? toA : toB;
    return glm::length(pick) < 1e-9 ? glm::dvec3(0.0) : glm::normalize(pick);
  };
  j.mine = away(ours, bi, bs);
  j.theirs = away(theirs, bj, bt);
  return j;
}

std::string OnABend(const RegenContext& ctx, const CopeFeature& cope,
                    const Feature* neighbour) {
  const CurveFeature* theirs = BentPathOf(ctx, neighbour);
  const CurveFeature* ours = BentPathOf(ctx, ctx.FindById(cope.memberId));
  if (theirs == nullptr || ours == nullptr || ours->points.empty())
    return {};
  const auto* pipe =
      dynamic_cast<const PipeFeature*>(ctx.FindById(cope.memberId));
  const double reach = pipe != nullptr ? pipe->outerDiameter * 0.5 : 0.0;

  for (const glm::vec3& end : {ours->points.front(), ours->points.back()}) {
    for (const CurveFeature::Bend& b : theirs->Bends()) {
      const double tangent =
          b.radius * std::tan(angles::ToRadians(b.angleDeg) * 0.5);
      const double d = glm::length(glm::dvec3(end) - glm::dvec3(b.corner));
      if (d < tangent + reach) {
        return "the joint lands on a BEND of '" + neighbour->GetName() +
               "'. A bend is a torus, thinned by the bending, with no "
               "straight to fit a cope against, so a joint does not belong "
               "there — move it clear of the bend, which reaches " +
               std::to_string(static_cast<int>(std::lround(tangent))) +
               " mm either side of that corner";
      }
    }
  }
  return {};
}

} // namespace

CopeFeature::CopeFeature(const std::string& name, uint64_t memberId,
                         std::vector<uint64_t> against,
                         std::vector<uint64_t> mitre)
    : memberId(memberId), against(std::move(against)), mitre(std::move(mitre)) {
  m_name = name;
}

bool CopeFeature::Execute(RegenContext& ctx) {
  auto member =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == memberId; });
  if (member == ctx.bodies.end()) {
    SetError("the member (feature " + std::to_string(memberId) +
             ") has no body — did it fail, or has something already consumed "
             "it?");
    return false;
  }
  if (against.empty() && mitre.empty()) {
    SetError("a cope needs something to fit TO");
    return false;
  }

  CADShape shape = member->shape;
  ElementMap map = member->elementMap;

  // Two lists, two cuts. `against` is a neighbour whose SIDE this member
  // lands on and wants a fishmouth; `mitre` is one it meets END TO END and
  // wants the bisecting plane. Which is which is topology the caller holds,
  // not geometry this can read: members overrun their nodes (a plane through
  // the very end of a tube shaves a wedge instead of mitring it), and once
  // they do, a corner and a tee look alike.
  struct Cut {
    uint64_t id;
    bool asMitre;
  };
  std::vector<Cut> cuts;
  for (const uint64_t id : against)
    cuts.push_back({id, false});
  for (const uint64_t id : mitre)
    cuts.push_back({id, true});

  for (const Cut& cut : cuts) {
    const uint64_t id = cut.id;
    if (id == memberId) {
      SetError("a member cannot be fitted to itself");
      return false;
    }
    const Feature* f = ctx.FindById(id);
    if (f == nullptr) {
      SetError("feature " + std::to_string(id) +
               " is not earlier in this part's history");
      return false;
    }
    CADShape cutter;
    ElementMap cutterMap;
    const auto* pipe = dynamic_cast<const PipeFeature*>(f);

    if (cut.asMitre) {
      const CurveFeature* ourPath = BentPathOf(ctx, ctx.FindById(memberId));
      const CurveFeature* theirPath = BentPathOf(ctx, f);
      if (ourPath == nullptr || theirPath == nullptr) {
        SetError("mitre to '" + f->GetName() +
                 "': a mitre is between two tubes on bent paths, and one of "
                 "these is not");
        return false;
      }
      const Joint j = JointBetween(ourPath->points, theirPath->points);
      if (!j.found) {
        SetError("mitre to '" + f->GetName() + "': they share no node");
        return false;
      }
      // Two members running IN LINE have no corner to bisect, and drawn
      // long they overlap, so where the joint is cannot be read off the
      // geometry at all — every point of the overlap is equally close.
      // Refuse rather than cut somewhere arbitrary.
      if (std::abs(glm::dot(j.mine, j.theirs)) > 0.999) {
        SetError("mitre to '" + f->GetName() +
                 "': these two run in LINE, so there is no corner to bisect "
                 "and nothing says where along the overlap the joint is. A "
                 "butt between collinear tubes is a square cut somebody has "
                 "to place");
        return false;
      }
      // The bisecting plane: two tubes at 90 degrees are each cut at 45, and
      // the sharper the corner the further the heel reaches past the node.
      glm::dvec3 keep = glm::normalize(j.mine - j.theirs);
      glm::vec3 lo{0.0f}, hi{0.0f};
      shape.GetBoundingBox(lo, hi);
      const double size = glm::length(glm::dvec3(hi - lo)) * 2.0 + 200.0;
      cutter = MitreCutter(j.at + keep * gap, keep, size);
      if (!cutter.IsValid()) {
        SetError("mitre to '" + f->GetName() + "': could not build the cut");
        return false;
      }
    } else if (pipe != nullptr) {
      // A tube neighbour is rebuilt from its PATH as a solid, so the cut is
      // the outside of the tube and not its wall.
      std::string why;
      cutter =
          PipeOuterSolid(ctx, pipe->pathId, pipe->outerDiameter, gap, &why);
      if (!cutter.IsValid()) {
        SetError("cope to '" + f->GetName() + "': " + why);
        return false;
      }
    } else {
      if (gap > 1e-9) {
        SetError("a gap is a tube's, measured on its radius — '" +
                 f->GetName() + "' is a " + f->TypeName() +
                 ", so there is no radius to open");
        return false;
      }
      const auto neighbour =
          std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                       [&](const Body& b) { return b.createdBy == id; });
      if (neighbour == ctx.bodies.end()) {
        SetError("'" + f->GetName() + "' has no body to cope to");
        return false;
      }
      cutter = neighbour->shape;
      cutterMap = neighbour->elementMap;
    }

    ElementMap out;
    std::string why;
    const double was = shape.Volume();
    CADShape next = topo::BooleanNamed(BooleanOp::Subtract, shape, map, cutter,
                                       cutterMap, GetId(), out, &why);
    const char* what = cut.asMitre ? "mitre" : "cope";
    if (!next.IsValid() || next.Volume() < 1e-9) {
      SetError(std::string(what) + " to '" + f->GetName() +
               "' removed the whole member (do they actually meet?)" +
               (why.empty() ? "" : ": " + why));
      return false;
    }
    // A cut that removed NOTHING is a joint that does not exist. Reporting it
    // as done puts an unfitted tube in the order and a gap in the frame, and
    // both are found at the welding table.
    if (next.Volume() > was - 1e-9) {
      glm::vec3 lo{0.0f}, hi{0.0f}, clo{0.0f}, chi{0.0f};
      shape.GetBoundingBox(lo, hi);
      cutter.GetBoundingBox(clo, chi);
      const bool apart = hi.x < clo.x || chi.x < lo.x || hi.y < clo.y ||
                         chi.y < lo.y || hi.z < clo.z || chi.z < lo.z;
      if (apart) {
        SetError(std::string(what) + " to '" + f->GetName() +
                 "' removed nothing — the two do not reach each other, so "
                 "there is no joint here to cut");
      } else if (cut.asMitre) {
        // A member is cut for every joint it has, in order, and an earlier
        // cut at the same END can already have taken everything past this
        // plane. Then there is nothing left to do and nothing wrong: the
        // member already stops inside it. Only a mitre gets this — a COPE
        // that removes nothing has not reached its neighbour's surface, and
        // that is a gap in the frame.
        continue;
      } else if (const std::string bend = OnABend(ctx, *this, f);
                 !cut.asMitre && !bend.empty()) {
        SetError("cope to '" + f->GetName() + "' removed nothing: " + bend);
      } else {
        SetError(
            std::string(what) + " to '" + f->GetName() +
            "' removed nothing, although they overlap: the cut is TANGENT, "
            "which a boolean answers by handing back what it was given. Two "
            "tubes of one diameter whose axes meet exactly do this. Give it "
            "a `gap` — 0.05 mm is enough to make the surfaces cross, and a "
            "real joint is cut a little open for the weld root anyway");
      }
      return false;
    }
    shape = std::move(next);
    map = std::move(out);
  }

  // The member is REPLACED, not consumed into something new: it is the same
  // part of the frame, one cut shorter, and the cut list has to go on seeing
  // one member where there is one member.
  member->shape = std::move(shape);
  member->elementMap = std::move(map);
  return true;
}
