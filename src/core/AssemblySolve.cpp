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
// The same composition ui::OccurrenceMatrix does; duplicated here (four
// lines) rather than pulling the UI layer into core, which is where every
// other assembly walk already lives.
glm::mat4 PlacementOf(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) *
         glm::mat4_cast(o.orientation);
}

struct FlattenCtx {
  const PartResolver& partById;
  const AssemblyResolver& asmById;
  int maxDepth;
  std::vector<uint64_t> stack; // assembly slot ids on the current path
  std::vector<std::string> names;
  std::vector<FlatInstance> out;
  std::string error;
};

bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::mat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth);

bool DescendOccurrence(FlattenCtx& ctx, const Occurrence& occ,
                       const glm::mat4& world, uint64_t rootOccurrenceId,
                       bool visible, int depth) {
  const AssemblyDocument* sub = ctx.asmById(occ.partId);
  if (sub == nullptr)
    return true; // dangling: the tree still shows what remains
  // A cycle renders forever; say which reference closes it.
  for (const uint64_t onPath : ctx.stack) {
    if (onPath == occ.partId) {
      std::string chain;
      for (const std::string& n : ctx.names)
        chain += n + " -> ";
      ctx.error = "assembly reference cycle: " + chain + occ.name;
      return false;
    }
  }
  ctx.stack.push_back(occ.partId);
  ctx.names.push_back(occ.name);
  const bool ok =
      FlattenInto(ctx, *sub, world, rootOccurrenceId, visible, depth + 1);
  ctx.names.pop_back();
  ctx.stack.pop_back();
  return ok;
}

bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::mat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth) {
  if (depth > ctx.maxDepth) {
    ctx.error = "assemblies nested deeper than " +
                std::to_string(ctx.maxDepth) + " levels";
    return false;
  }
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    // Suppression and visibility compose down the tree: hiding a
    // sub-assembly hides everything in it.
    const bool visible =
        parentVisible && occ.visible && !asmDoc.OccurrenceSuppressed(occ.id);
    const glm::mat4 world = parentWorld * PlacementOf(occ);
    // The TOP-LEVEL occurrence owns everything below it — that is what makes
    // a sub-assembly one contact node, one explode step, one drawing ref.
    const uint64_t root = rootOccurrenceId ? rootOccurrenceId : occ.id;
    const std::string prefix = ctx.names.empty() ? std::string{} : [&] {
      std::string p;
      for (const std::string& n : ctx.names)
        p += n + "/";
      return p;
    }();

    if (occ.refKind == OccurrenceRef::Assembly) {
      if (!DescendOccurrence(ctx, occ, world, root, visible, depth))
        return false;
      continue;
    }
    const Document* part = ctx.partById(occ.partId);
    if (part == nullptr)
      continue; // dangling part reference, as today
    FlatInstance fi;
    fi.partId = occ.partId;
    fi.part = part;
    fi.world = world;
    fi.path = prefix + occ.name;
    fi.rootOccurrenceId = root;
    fi.visible = visible;
    ctx.out.push_back(std::move(fi));
  }
  return true;
}
} // namespace

std::expected<std::vector<FlatInstance>, std::string>
Flatten(const AssemblyDocument& top, const PartResolver& partById,
        const AssemblyResolver& asmById, int maxDepth) {
  FlattenCtx ctx{partById, asmById, maxDepth, {}, {}, {}, {}};
  if (!FlattenInto(ctx, top, glm::mat4(1.0f), 0, true, 0))
    return std::unexpected(ctx.error);
  return ctx.out;
}

namespace {
// One step of a path: match an occurrence in `doc` by id (all digits) or by
// name. Returns nullptr when absent; sets `ambiguous` when a NAME matches
// more than one, which is possible because names are not unique.
const Occurrence* StepInto(const AssemblyDocument& doc, const std::string& step,
                           bool& ambiguous) {
  ambiguous = false;
  const bool numeric = !step.empty() && step.find_first_not_of("0123456789") ==
                                            std::string::npos;
  if (numeric) {
    const uint64_t id = std::stoull(step);
    for (const Occurrence& o : doc.GetOccurrences())
      if (o.id == id)
        return &o;
    return nullptr;
  }
  const Occurrence* hit = nullptr;
  for (const Occurrence& o : doc.GetOccurrences()) {
    if (o.name != step)
      continue;
    if (hit != nullptr) {
      ambiguous = true;
      return nullptr;
    }
    hit = &o;
  }
  return hit;
}
} // namespace

std::expected<PathHit, std::string> ResolvePath(const AssemblyDocument& top,
                                                const std::string& ref,
                                                const PartResolver& partById,
                                                const AssemblyResolver& asmById,
                                                int maxDepth) {
  std::vector<std::string> steps;
  size_t start = 0;
  while (start <= ref.size()) {
    const size_t slash = ref.find('/', start);
    steps.push_back(ref.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start));
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  if (steps.empty() || steps.front().empty())
    return std::unexpected("empty occurrence path");
  if (static_cast<int>(steps.size()) > maxDepth + 1)
    return std::unexpected("occurrence path is deeper than " +
                           std::to_string(maxDepth) + " levels");

  const AssemblyDocument* doc = &top;
  PathHit hit;
  std::string canonical;
  for (size_t i = 0; i < steps.size(); i++) {
    if (doc == nullptr)
      return std::unexpected("'" + steps[i - 1] +
                             "' is a part, so nothing is inside it");
    bool ambiguous = false;
    const Occurrence* occ = StepInto(*doc, steps[i], ambiguous);
    if (ambiguous)
      return std::unexpected("'" + steps[i] +
                             "' names more than one occurrence here — use its "
                             "id instead (list_occurrences shows them)");
    if (occ == nullptr)
      return std::unexpected("no occurrence '" + steps[i] + "'" +
                             (i ? " inside '" + steps[i - 1] + "'" : ""));
    hit.owner = doc;
    hit.occ = occ;
    hit.world = hit.world * PlacementOf(*occ);
    if (i == 0)
      hit.rootId = occ->id;
    canonical += (canonical.empty() ? "" : "/") + std::to_string(occ->id);
    doc = occ->refKind == OccurrenceRef::Assembly ? asmById(occ->partId)
                                                  : nullptr;
  }
  hit.path = canonical;
  (void)partById; // resolution is structural; bodies are the caller's job
  return hit;
}

bool WouldCycle(const AssemblyDocument& host, uint64_t hostSlotId,
                uint64_t refId, OccurrenceRef kind,
                const AssemblyResolver& asmById, std::string* why) {
  if (kind != OccurrenceRef::Assembly)
    return false;
  if (refId == hostSlotId) {
    if (why)
      *why = "an assembly cannot contain itself";
    return true;
  }
  // Does the candidate already reach the host, anywhere below it?
  std::vector<uint64_t> stack{refId};
  std::vector<uint64_t> seen;
  while (!stack.empty()) {
    const uint64_t id = stack.back();
    stack.pop_back();
    if (std::find(seen.begin(), seen.end(), id) != seen.end())
      continue;
    seen.push_back(id);
    const AssemblyDocument* doc = asmById(id);
    if (doc == nullptr)
      continue;
    for (const Occurrence& occ : doc->GetOccurrences()) {
      if (occ.refKind != OccurrenceRef::Assembly)
        continue;
      if (occ.partId == hostSlotId) {
        if (why)
          *why = "that assembly already contains this one, so placing it here "
                 "would close a cycle";
        return true;
      }
      stack.push_back(occ.partId);
    }
  }
  return false;
}

std::vector<std::pair<uint64_t, uint64_t>>
SubtreeVersions(const AssemblyDocument& top, const PartResolver& partById,
                const AssemblyResolver& asmById, int maxDepth) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  std::vector<std::pair<const AssemblyDocument*, int>> stack{{&top, 0}};
  std::vector<uint64_t> visited;
  while (!stack.empty()) {
    const auto [doc, depth] = stack.back();
    stack.pop_back();
    if (depth > maxDepth)
      continue;
    for (const Occurrence& occ : doc->GetOccurrences()) {
      if (occ.refKind == OccurrenceRef::Assembly) {
        const AssemblyDocument* sub = asmById(occ.partId);
        if (sub == nullptr)
          continue;
        // The sub's OWN version: an occurrence moved inside it changes
        // nothing about its parts, and must still restale the top.
        out.emplace_back(occ.partId, sub->Version());
        if (std::find(visited.begin(), visited.end(), occ.partId) ==
            visited.end()) {
          visited.push_back(occ.partId);
          stack.push_back({sub, depth + 1});
        }
      } else if (const Document* part = partById(occ.partId)) {
        out.emplace_back(occ.partId, part->Version());
      }
    }
  }
  return out;
}

namespace {

// The occurrence's own placement matrix (translation + orientation). Mirrors
// ui::OccurrenceMatrix, kept here so the solver has no UI dependency.
glm::mat4 OccMatrix(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) *
         glm::mat4_cast(o.orientation);
}

// Build a right-handed orthonormal frame matrix from an origin, an in-plane X
// hint, and the primary Z axis (columns X, Y, Z, origin). X is
// re-orthogonalized against Z so the frame is exactly orthonormal even if the
// hint drifted.
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

void Decompose(const glm::mat4& m, glm::vec3& position,
               glm::quat& orientation) {
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
    if (whyNot)
      *whyNot = std::move(why);
    return std::nullopt;
  };
  const Occurrence* occ = asmDoc.FindById(c.occurrenceId);
  if (!occ)
    return fail("its occurrence no longer exists");

  if (c.anchor.origin)
    return glm::mat4(1.0f); // the part's coordinate origin

  const Document* part = partById(occ->partId);
  if (!part) {
    // A5: distinguish "gone" from "it is a sub-assembly". A face anchor
    // names a face of a PART; a placed assembly has no faces of its own, and
    // saying "not loaded" sends someone looking for a missing file.
    if (occ->refKind == OccurrenceRef::Assembly)
      return fail("it references a sub-assembly, which has no faces of its "
                  "own — mate to its origin instead, or open that assembly "
                  "and mate the part inside it");
    return fail("the part document is not loaded");
  }

  const ElementRef& ref = c.anchor.face;
  for (const Body& body : part->GetBodies()) {
    if (body.createdBy != ref.featureId)
      continue;
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
          if (!similar.empty())
            similar += ", ";
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
                  "' has no connector frame (planar, cylindrical, conical, "
                  "and spherical faces do)");

    // Inference snap (A2.5 phase 4): override the origin with the snapped
    // edge's characteristic point (a hole center, etc.), keeping the face's Z
    // normal.
    if (c.anchor.snapEdge.IsSet()) {
      if (const auto edgeIdx =
              body.elementMap.ResolveEdge(c.anchor.snapEdge.name)) {
        glm::vec3 snap;
        if (body.shape.EdgeSnapPoint(*edgeIdx, snap))
          origin = snap;
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
        if (occ)
          s += " (" + occ->name + ")";
        return s + ": " + why;
      };
      std::string err;
      if (!fa)
        err = side("A", mate.a, whyA);
      if (!fb) {
        if (!err.empty())
          err += "; ";
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
    if (o.fixed)
      world[o.id] = OccMatrix(o);

  // Propagate to a fixpoint, anchoring the lowest-id occurrence of each
  // still-unplaced floating component in between.
  while (true) {
    bool progress = true;
    while (progress) {
      progress = false;
      for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
        const Mate& mate = asmDoc.GetMates()[i];
        if (!frames[i].usable)
          continue;
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
          if (!mate.error.empty())
            continue; // already flagged
          const glm::mat4 wa = world[aId] * frames[i].fa;
          const glm::mat4 wbExpected = wa * adjust;
          const glm::mat4 wbActual = world[bId] * frames[i].fb;
          if (MaxDiff(wbExpected, wbActual) > kConsistencyTol) {
            asmDoc.GetMates()[i].error =
                "over-constrained (mate loop conflict)";
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
      if (!frames[i].usable)
        continue;
      for (uint64_t id : {asmDoc.GetMates()[i].a.occurrenceId,
                          asmDoc.GetMates()[i].b.occurrenceId}) {
        if (!isPlaced(id) && (floatingAnchor == 0 || id < floatingAnchor))
          floatingAnchor = id;
      }
    }
    if (floatingAnchor == 0)
      break;
    if (const Occurrence* o = asmDoc.FindById(floatingAnchor))
      world[floatingAnchor] = OccMatrix(*o);
    else
      break; // shouldn't happen (mate referenced a real occurrence)
  }

  // Write solved transforms back to the non-`fixed` occurrences. Anchors keep
  // their stored transform (their world entry equals it); unmated occurrences
  // have no world entry and are untouched.
  for (Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.fixed)
      continue;
    const auto it = world.find(o.id);
    if (it == world.end())
      continue;
    glm::vec3 pos;
    glm::quat rot;
    Decompose(it->second, pos, rot);
    if (pos != o.position || rot != o.orientation)
      report.placed++;
    o.position = pos;
    o.orientation = rot;
  }

  return report;
}

AssemblyMass MassProperties(const AssemblyDocument& asmDoc,
                            const PartResolver& partById,
                            const AssemblyResolver& asmById) {
  AssemblyMass out;
  glm::dvec3 weightedCom(0.0);
  // A5: aggregate over the LEAF parts of the tree. A resolver that finds no
  // assemblies flattens to exactly the top-level occurrences, which is the
  // pre-A5 behaviour.
  const AssemblyResolver none = [](uint64_t) -> const AssemblyDocument* {
    return nullptr;
  };
  auto flat = Flatten(asmDoc, partById, asmById ? asmById : none);
  if (!flat) {
    out.skipped.push_back(flat.error());
    out.massKnown = false;
    return out;
  }
  for (const FlatInstance& inst : *flat) {
    if (!inst.visible)
      continue; // BOM convention: hidden/suppressed occurrences don't count
    std::string err;
    const auto mass = Measure::MassOfBodies(inst.part->GetBodies(), &err);
    if (!mass) {
      out.skipped.push_back(inst.path + " (" + err + ")");
      out.massKnown = false;
      continue;
    }
    const glm::dmat4 xf = glm::dmat4(inst.world);
    const glm::dvec3 worldCom =
        glm::dvec3(xf * glm::dvec4(mass->centerOfMass, 1.0));

    OccurrenceMass om;
    // The TOP-LEVEL occurrence: a sub-assembly reports as one line, which is
    // the same choice contact and drawings make.
    om.occurrenceId = inst.rootOccurrenceId;
    om.name = inst.path;
    om.partId = inst.partId;
    om.volume = mass->volume; // volume is rigid-transform invariant
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
  if (!out.massKnown)
    out.massG = 0.0;
  return out;
}

} // namespace assembly
