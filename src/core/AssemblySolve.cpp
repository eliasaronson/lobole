#include "core/AssemblySolve.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/MassProps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace assembly {

namespace {

// The occurrence's own placement matrix (translation + orientation). Mirrors
// ui::OccurrenceMatrix, kept here so the solver has no UI dependency.
glm::mat4 OccMatrix(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) * glm::mat4_cast(o.orientation);
}

// Build a right-handed orthonormal frame matrix from an origin, an in-plane X
// hint, and the primary Z axis (columns X, Y, Z, origin). X is re-orthogonalized
// against Z so the frame is exactly orthonormal even if the hint drifted.
glm::mat4 FrameMatrix(const glm::vec3& origin, const glm::vec3& xHint,
                      const glm::vec3& zAxis) {
  const glm::vec3 z = glm::normalize(zAxis);
  glm::vec3 x = xHint - glm::dot(xHint, z) * z;
  if (glm::dot(x, x) < 1e-12f) {
    // Degenerate hint: pick any perpendicular deterministically.
    x = std::abs(z.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    x = x - glm::dot(x, z) * z;
  }
  x = glm::normalize(x);
  const glm::vec3 y = glm::cross(z, x);
  glm::mat4 m(1.0f);
  m[0] = glm::vec4(x, 0.0f);
  m[1] = glm::vec4(y, 0.0f);
  m[2] = glm::vec4(z, 0.0f);
  m[3] = glm::vec4(origin, 1.0f);
  return m;
}

// Adjustment from a's connector frame to b's connector frame, composed as
// T(dx,dy,offset) · Rz(rotation) · R_ball · Flip. Each mate type uses a
// subset: fastened/revolute/slider/cylindrical leave dx=dy=0 and R_ball=I (so
// this reduces to the original T(0,0,offset)·Rz·Flip); planar adds the in-plane
// dx,dy; ball uses R_ball. An optional 180° flip about X makes two touching
// planar faces oppose normals. For a fastened mate with no flip and zero DOF
// this is the identity ⇒ the two frames coincide exactly.
glm::mat4 MateAdjust(const Mate& m) {
  glm::mat4 a(1.0f);
  a = glm::translate(a, glm::vec3(m.dx, m.dy, m.offset));
  a = glm::rotate(a, m.rotation, glm::vec3(0.0f, 0.0f, 1.0f));
  a *= glm::mat4_cast(m.ballRotation);
  if (m.flipped)
    a = glm::rotate(a, glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
  return a;
}

void Decompose(const glm::mat4& m, glm::vec3& position, glm::quat& orientation) {
  position = glm::vec3(m[3]);
  orientation = glm::normalize(glm::quat_cast(glm::mat3(m)));
}

// Max absolute element difference — a cheap rigid-transform equality test for
// the loop-consistency check (world-scale, so a millimetre-ish tolerance).
float MaxDiff(const glm::mat4& a, const glm::mat4& b) {
  float d = 0.0f;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      d = std::max(d, std::abs(a[c][r] - b[c][r]));
  return d;
}

constexpr float kConsistencyTol = 1e-3f;

} // namespace

std::optional<glm::mat4> ConnectorLocalFrame(const Connector& c,
                                             const AssemblyDocument& asmDoc,
                                             const PartResolver& partById,
                                             std::string* whyNot) {
  const auto fail = [&](std::string why) {
    if (whyNot) *whyNot = std::move(why);
    return std::nullopt;
  };
  const Occurrence* occ = asmDoc.FindById(c.occurrenceId);
  if (!occ) return fail("its occurrence no longer exists");

  if (c.anchor.origin) return glm::mat4(1.0f); // the part's coordinate origin

  const Document* part = partById(occ->partId);
  if (!part) return fail("the part document is not loaded");

  const ElementRef& ref = c.anchor.face;
  for (const Body& body : part->GetBodies()) {
    if (body.createdBy != ref.featureId) continue;
    const auto faceIdx = body.elementMap.Resolve(ref.name);
    if (!faceIdx) {
      // Owner found, name no longer resolves. The usual cause: a feature
      // added AFTER the mate modified the face, extending its history name
      // ("6/g1;M6" -> "6/g1;M6;M7") — suggest current names that keep the
      // failing name as a prefix.
      std::string similar;
      for (const auto& [idx, name] : body.elementMap.names) {
        (void)idx;
        if (name.size() > ref.name.size() &&
            name.compare(0, ref.name.size(), ref.name) == 0) {
          if (!similar.empty()) similar += ", ";
          similar += name;
        }
      }
      std::string why = "face '" + ref.name + "' not found on its part";
      if (!similar.empty())
        why += "; similar: " + similar +
               " (the face was modified by a later feature — update the "
               "connector to the new name)";
      return fail(std::move(why));
    }

    glm::vec3 origin, primary, xHint;
    bool ok = body.shape.FacePlane(*faceIdx, origin, primary, xHint) ||
              body.shape.FaceAxis(*faceIdx, origin, primary, xHint);
    if (!ok)
      return fail("face '" + ref.name +
                  "' is neither planar nor cylindrical — no connector frame");

    // Inference snap (A2.5 phase 4): override the origin with the snapped edge's
    // characteristic point (a hole center, etc.), keeping the face's Z normal.
    if (c.anchor.snapEdge.IsSet()) {
      if (const auto edgeIdx = body.elementMap.ResolveEdge(c.anchor.snapEdge.name)) {
        glm::vec3 snap;
        if (body.shape.EdgeSnapPoint(*edgeIdx, snap)) origin = snap;
      }
    }
    return FrameMatrix(origin, xHint, /*z=*/primary);
  }
  return fail("no body created by feature " + std::to_string(ref.featureId) +
              " on its part (feature deleted or contributing nothing?)");
}

SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById) {
  SolveReport report;

  // Pre-derive both connector frames for every mate; a mate whose connectors
  // don't resolve (dangling ref / missing part) is stamped errored and sits
  // out the propagation — its occurrences may still be placed by other mates
  // or keep their stored transforms.
  struct MateFrames {
    bool usable = false;
    glm::mat4 fa{1.0f};
    glm::mat4 fb{1.0f};
  };
  std::vector<MateFrames> frames(asmDoc.GetMates().size());

  for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
    Mate& mate = asmDoc.GetMates()[i];
    mate.error.clear();
    std::string whyA, whyB;
    const auto fa = ConnectorLocalFrame(mate.a, asmDoc, partById, &whyA);
    const auto fb = ConnectorLocalFrame(mate.b, asmDoc, partById, &whyB);
    if (!fa || !fb) {
      // Name WHICH side failed (occurrence + why), so a stale toponame is
      // fixable without probing both connectors.
      const auto side = [&](const char* letter, const Connector& c,
                            const std::string& why) {
        const Occurrence* occ = asmDoc.FindById(c.occurrenceId);
        std::string s = std::string("connector ") + letter;
        if (occ) s += " (" + occ->name + ")";
        return s + ": " + why;
      };
      std::string err;
      if (!fa) err = side("A", mate.a, whyA);
      if (!fb) {
        if (!err.empty()) err += "; ";
        err += side("B", mate.b, whyB);
      }
      mate.error = err;
      report.erroredMates++;
      continue;
    }
    frames[i] = {true, *fa, *fb};
  }

  // Placement state: an occurrence's solved world transform, once known.
  std::unordered_map<uint64_t, glm::mat4> world;
  const auto isPlaced = [&](uint64_t occId) { return world.count(occId) != 0; };

  // Anchors: every `fixed` occurrence starts at its stored transform.
  for (const Occurrence& o : asmDoc.GetOccurrences())
    if (o.fixed) world[o.id] = OccMatrix(o);

  // Propagate to a fixpoint, anchoring the lowest-id occurrence of each
  // still-unplaced floating component in between.
  while (true) {
    bool progress = true;
    while (progress) {
      progress = false;
      for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
        const Mate& mate = asmDoc.GetMates()[i];
        if (!frames[i].usable) continue;
        const uint64_t aId = mate.a.occurrenceId;
        const uint64_t bId = mate.b.occurrenceId;
        const bool aP = isPlaced(aId), bP = isPlaced(bId);
        const glm::mat4 adjust = MateAdjust(mate);

        if (aP && !bP) {
          // b's connector frame = a's connector world frame · adjust.
          const glm::mat4 wa = world[aId] * frames[i].fa;
          world[bId] = wa * adjust * glm::inverse(frames[i].fb);
          progress = true;
        } else if (bP && !aP) {
          const glm::mat4 wb = world[bId] * frames[i].fb;
          world[aId] = wb * glm::inverse(adjust) * glm::inverse(frames[i].fa);
          progress = true;
        } else if (aP && bP) {
          // Loop / redundant mate: check consistency, don't re-place.
          if (!mate.error.empty()) continue; // already flagged
          const glm::mat4 wa = world[aId] * frames[i].fa;
          const glm::mat4 wbExpected = wa * adjust;
          const glm::mat4 wbActual = world[bId] * frames[i].fb;
          if (MaxDiff(wbExpected, wbActual) > kConsistencyTol) {
            asmDoc.GetMates()[i].error = "over-constrained (mate loop conflict)";
            report.erroredMates++;
          }
        }
      }
    }

    // Any usable mate with an unplaced occurrence? Anchor the lowest-id such
    // occurrence at its stored transform (a floating component with no fixed
    // anchor stays where the user left it), then propagate again.
    uint64_t floatingAnchor = 0;
    for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
      if (!frames[i].usable) continue;
      for (uint64_t id : {asmDoc.GetMates()[i].a.occurrenceId,
                          asmDoc.GetMates()[i].b.occurrenceId}) {
        if (!isPlaced(id) && (floatingAnchor == 0 || id < floatingAnchor))
          floatingAnchor = id;
      }
    }
    if (floatingAnchor == 0) break;
    if (const Occurrence* o = asmDoc.FindById(floatingAnchor))
      world[floatingAnchor] = OccMatrix(*o);
    else
      break; // shouldn't happen (mate referenced a real occurrence)
  }

  // Write solved transforms back to the non-`fixed` occurrences. Anchors keep
  // their stored transform (their world entry equals it); unmated occurrences
  // have no world entry and are untouched.
  for (Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.fixed) continue;
    const auto it = world.find(o.id);
    if (it == world.end()) continue;
    glm::vec3 pos;
    glm::quat rot;
    Decompose(it->second, pos, rot);
    if (pos != o.position || rot != o.orientation) report.placed++;
    o.position = pos;
    o.orientation = rot;
  }

  return report;
}

AssemblyMass MassProperties(const AssemblyDocument& asmDoc,
                            const PartResolver& partById) {
  AssemblyMass out;
  glm::dvec3 weightedCom(0.0);
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    if (!o.visible || asmDoc.OccurrenceSuppressed(o.id))
      continue; // BOM convention: hidden/suppressed occurrences don't count
    const Document* part = partById(o.partId);
    if (!part) {
      out.skipped.push_back(o.name + " (part not loaded)");
      out.massKnown = false;
      continue;
    }
    std::string err;
    const auto mass = Measure::MassOfBodies(part->GetBodies(), &err);
    if (!mass) {
      out.skipped.push_back(o.name + " (" + err + ")");
      out.massKnown = false;
      continue;
    }
    // World placement of this occurrence (translate + orientation, no scale).
    const glm::dmat4 xf = glm::dmat4(OccMatrix(o));
    const glm::dvec3 worldCom =
        glm::dvec3(xf * glm::dvec4(mass->centerOfMass, 1.0));

    OccurrenceMass om;
    om.occurrenceId = o.id;
    om.name = o.name;
    om.partId = o.partId;
    om.volume = mass->volume;       // volume is rigid-transform invariant
    om.centerOfMass = worldCom;
    om.massKnown = mass->massKnown; // mass is rigid-transform invariant
    om.massG = mass->massG;
    out.occurrences.push_back(om);

    out.volume += mass->volume;
    weightedCom += worldCom * mass->volume;
    if (mass->massKnown)
      out.massG += mass->massG;
    else
      out.massKnown = false;
  }
  out.centerOfMass =
      out.volume > 0.0 ? weightedCom / out.volume : glm::dvec3(0.0);
  if (!out.massKnown) out.massG = 0.0;
  return out;
}

} // namespace assembly
