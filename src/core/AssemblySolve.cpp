#include "core/AssemblySolve.h"

#include "core/FrameMath.h"
#include "core/features/FrameFeature.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/MassProps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

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
  std::vector<uint64_t> ids; // occurrence ids below the root, current path
  std::vector<FlatInstance> out;
  std::string error;
};

bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::mat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth, uint64_t configurationId);

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
  // Not the root itself: the chain records what is INSIDE it.
  if (!ctx.names.empty() && ctx.names.size() > 1)
    ctx.ids.push_back(occ.id);
  // The placement chooses the sub-assembly's configuration (v56): two
  // Gangways, one "short", one "long", from one document.
  const bool ok = FlattenInto(ctx, *sub, world, rootOccurrenceId, visible,
                              depth + 1, occ.configurationId);
  ctx.names.pop_back();
  ctx.stack.pop_back();
  if (!ctx.ids.empty() && ctx.names.size() >= 1)
    ctx.ids.pop_back();
  return ok;
}

bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::mat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth, uint64_t configurationId) {
  if (depth > ctx.maxDepth) {
    ctx.error = "assemblies nested deeper than " +
                std::to_string(ctx.maxDepth) + " levels";
    return false;
  }
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    // Suppression and visibility compose down the tree: hiding a
    // sub-assembly hides everything in it.
    // `configurationId` is the one the PLACEMENT asked for (v56); 0 means
    // this document's active one, so a top-level walk is unchanged.
    const bool visible =
        parentVisible && occ.visible &&
        !asmDoc.OccurrenceSuppressedIn(configurationId, occ.id);
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
    fi.innerIds = ctx.ids;
    if (!ctx.names.empty())
      fi.innerIds.push_back(occ.id); // the leaf's own id inside its parent
    fi.visible = visible;
    ctx.out.push_back(std::move(fi));
  }
  return true;
}
} // namespace

std::expected<std::vector<FlatInstance>, std::string>
Flatten(const AssemblyDocument& top, const PartResolver& partById,
        const AssemblyResolver& asmById, int maxDepth) {
  FlattenCtx ctx{partById, asmById, maxDepth, {}, {}, {}, {}, {}};
  // The top itself uses its own active configuration (nobody placed it).
  if (!FlattenInto(ctx, top, glm::mat4(1.0f), 0, true, 0, 0))
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

// The one frame builder (core/FrameMath.h): origin, in-plane X hint, primary
// Z, columns X Y Z origin, exactly orthonormal.
glm::mat4 FrameMatrix(const glm::vec3& origin, const glm::vec3& xHint,
                      const glm::vec3& zAxis) {
  return frame::Build(origin, xHint, zAxis);
}

// Adjustment from a's connector frame to b's connector frame, composed as
// T(dx,dy,offset) · Rz(rotation) · R_ball · Flip. Each mate type uses a
// subset: fastened/revolute/slider/cylindrical leave dx=dy=0 and R_ball=I (so
// this reduces to the original T(0,0,offset)·Rz·Flip); planar adds the in-plane
// dx,dy; ball uses R_ball. An optional 180° flip about X makes two touching
// planar faces oppose normals. For a fastened mate with no flip and zero DOF
// this is the identity ⇒ the two frames coincide exactly.
glm::mat4 MateAdjust(const Mate& m) {
  // ONE definition of this composition, in AssemblyLoopSolve — the loop solver
  // evaluates the same transform thousands of times per solve, and a second
  // copy of a transform in this codebase has drifted from the first before.
  return MateAdjustFor(m.flipped, m.rotation, m.offset, m.dx, m.dy,
                       m.ballRotation);
}

void Decompose(const glm::mat4& m, glm::vec3& position,
               glm::quat& orientation) {
  position = glm::vec3(m[3]);
  orientation = glm::normalize(glm::quat_cast(glm::mat3(m)));
}

// Short, stable rendering of a millimetre figure for a mate error message.
std::string FormatMm(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", mm);
  return buf;
}

} // namespace

std::optional<glm::mat4> ConnectorLocalFrame(const Connector& c,
                                             const AssemblyDocument& asmDoc,
                                             const PartResolver& partById,
                                             std::string* whyNot,
                                             const AssemblyResolver& asmById) {
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

  // A5/v57: walk into the sub-assembly to the part the anchor means, keeping
  // the transform from the UNIT's frame down to that part. Everything below
  // then works on a part exactly as it always did, and the frame comes back
  // pre-multiplied — so the caller still just composes the occurrence matrix.
  glm::mat4 innerXf(1.0f);
  const Document* partForFace = nullptr;
  if (!c.innerPath.empty()) {
    if (occ->refKind != OccurrenceRef::Assembly)
      return fail("it has a path into a sub-assembly but places a part");
    const AssemblyDocument* doc = asmById ? asmById(occ->partId) : nullptr;
    if (doc == nullptr)
      return fail("its sub-assembly is not loaded");
    const Occurrence* inner = nullptr;
    for (size_t i = 0; i < c.innerPath.size(); i++) {
      if (doc == nullptr)
        return fail("its path goes deeper than the assembly does");
      inner = doc->FindById(c.innerPath[i]);
      if (inner == nullptr)
        return fail("the part it points at inside the sub-assembly is gone");
      innerXf = innerXf * OccMatrix(*inner);
      doc = inner->refKind == OccurrenceRef::Assembly && asmById
                ? asmById(inner->partId)
                : nullptr;
    }
    const Document* innerPart = partById(inner->partId);
    if (innerPart == nullptr)
      return fail("the part inside the sub-assembly is not loaded");
    partForFace = innerPart; // resolve the face on the part the path names
  }

  const Document* part = partForFace ? partForFace : partById(occ->partId);
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

  // v86: a named Frame of the part — the mate as designed. Resolved by id
  // against the part's history; an errored frame refuses the mate rather
  // than handing it a stale matrix.
  if (c.anchor.frameId != 0) {
    const FrameFeature* fr = nullptr;
    std::string frames;
    for (const auto& f : part->GetFeatures()) {
      const auto* cand = dynamic_cast<const FrameFeature*>(f.get());
      if (cand == nullptr)
        continue;
      if (cand->GetId() == c.anchor.frameId)
        fr = cand;
      if (!frames.empty())
        frames += ", ";
      frames += std::to_string(cand->GetId()) + " (" + cand->GetName() + ")";
    }
    if (fr == nullptr) {
      return fail("frame " + std::to_string(c.anchor.frameId) +
                  " is not a Frame feature of its part; frames there: " +
                  (frames.empty() ? std::string("none") : frames));
    }
    if (!fr->GetError().empty())
      return fail("frame '" + fr->GetName() +
                  "' did not resolve: " + fr->GetError());
    return innerXf * fr->Matrix();
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
        // A CORNER (v65) when a second edge is named: the vertex the two
        // share. Falls back to the first edge's own point when they share
        // none or more than one — an ambiguous pair names no single corner,
        // and a plausible guess is worse than the point that IS well defined.
        bool cornered = false;
        if (c.anchor.snapEdge2.IsSet()) {
          if (const auto edgeIdx2 =
                  body.elementMap.ResolveEdge(c.anchor.snapEdge2.name))
            cornered = body.shape.CornerPoint(*edgeIdx, *edgeIdx2, snap);
        }
        if (cornered || body.shape.EdgeSnapPoint(*edgeIdx, snap))
          origin = snap;
      }
    }
    // innerXf is identity unless the anchor reached into a sub-assembly, in
    // which case the frame comes back in the UNIT's space — which is what
    // the solve composes with the occurrence matrix.
    return innerXf * FrameMatrix(origin, xHint, /*z=*/primary);
  }
  return fail("no body created by feature " + std::to_string(ref.featureId) +
              " on its part (feature deleted or contributing nothing?)");
}

int ApplyAttachments(AssemblyDocument& asmDoc) {
  int moved = 0;
  for (Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.attachedTo == 0)
      continue;
    const Occurrence* parent = asmDoc.FindById(o.attachedTo);
    if (parent == nullptr)
      continue; // the parent went; the rider keeps its last pose
    if (parent->attachedTo != 0)
      continue; // one level (see the header)
    const glm::mat4 world = PlacementOf(*parent) *
                            glm::translate(glm::mat4(1.0f), o.attachOffset) *
                            glm::mat4_cast(o.attachRotation);
    glm::vec3 pos;
    glm::quat rot;
    Decompose(world, pos, rot);
    if (pos != o.position || rot != o.orientation) {
      o.position = pos;
      o.orientation = rot;
      moved++;
    }
  }
  return moved;
}

bool MateHasDof(MateType type, MateDof dof) {
  switch (type) {
  case MateType::Revolute:
    return dof == MateDof::Rotation;
  case MateType::Slider:
    return dof == MateDof::Offset;
  case MateType::Cylindrical:
    return true;
  case MateType::Planar:
    return dof == MateDof::Rotation;
  case MateType::Ball:
  case MateType::Fastened:
    break;
  }
  return false;
}

double MateDofValue(const Mate& m, MateDof dof) {
  return dof == MateDof::Rotation ? m.rotation : m.offset;
}

void SetMateDof(Mate& m, MateDof dof, double value) {
  if (dof == MateDof::Rotation)
    m.rotation = static_cast<float>(value);
  else
    m.offset = static_cast<float>(value);
}

int PropagateRelations(AssemblyDocument& asmDoc, uint64_t drivenMateId) {
  int wrote = 0;
  std::vector<Relation>& rels = asmDoc.GetRelations();
  // Validate first: a relation with a problem is reported and skipped, and
  // never half-applied.
  std::vector<bool> usable(rels.size(), false);
  for (size_t i = 0; i < rels.size(); i++) {
    Relation& r = rels[i];
    r.error.clear();
    const Mate* a = asmDoc.FindMateById(r.mateA);
    const Mate* b = asmDoc.FindMateById(r.mateB);
    if (a == nullptr || b == nullptr) {
      r.error = "a mate this relation couples is gone";
      continue;
    }
    if (!MateHasDof(a->type, r.dofA) || !MateHasDof(b->type, r.dofB)) {
      r.error = "a mate this relation couples no longer has that degree of "
                "freedom";
      continue;
    }
    if (r.mateA == r.mateB && r.dofA == r.dofB) {
      r.error = "a relation cannot couple a degree of freedom to itself";
      continue;
    }
    usable[i] = true;
  }
  // Walk the relation graph from the driven mate (or from every A when
  // unprompted). A mate reached once is settled; a later relation that
  // would move it again is left to the loop solver's rows, which is where
  // an inconsistent set belongs.
  std::vector<uint64_t> frontier;
  std::vector<uint64_t> settled;
  const auto isSettled = [&](uint64_t id) {
    return std::find(settled.begin(), settled.end(), id) != settled.end();
  };
  if (drivenMateId != 0) {
    frontier.push_back(drivenMateId);
    settled.push_back(drivenMateId);
  } else {
    for (size_t i = 0; i < rels.size(); i++)
      if (usable[i] && !isSettled(rels[i].mateA)) {
        frontier.push_back(rels[i].mateA);
        settled.push_back(rels[i].mateA);
      }
  }
  while (!frontier.empty()) {
    const uint64_t from = frontier.back();
    frontier.pop_back();
    for (size_t i = 0; i < rels.size(); i++) {
      if (!usable[i])
        continue;
      Relation& r = rels[i];
      Mate* a = asmDoc.FindMateById(r.mateA);
      Mate* b = asmDoc.FindMateById(r.mateB);
      if (r.mateA == from && !isSettled(r.mateB)) {
        SetMateDof(*b, r.dofB, r.ratio * MateDofValue(*a, r.dofA) + r.phase);
        settled.push_back(r.mateB);
        frontier.push_back(r.mateB);
        wrote++;
      } else if (r.mateB == from && !isSettled(r.mateA) && r.ratio != 0.0) {
        SetMateDof(*a, r.dofA, (MateDofValue(*b, r.dofB) - r.phase) / r.ratio);
        settled.push_back(r.mateA);
        frontier.push_back(r.mateA);
        wrote++;
      } else if (r.mateA == from && r.mateB == from) {
        // A screw: one mate, two DOFs — the driven DOF is whichever the
        // caller changed, which this cannot know; A's DOF drives.
        SetMateDof(*b, r.dofB, r.ratio * MateDofValue(*a, r.dofA) + r.phase);
        wrote++;
      }
    }
  }
  return wrote;
}

SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById,
                  const AssemblyResolver& asmById) {
  SolveReport report;
  // Relations hold before anything is placed: in a mate tree this IS their
  // enforcement; in a loop they are rows below as well.
  (void)PropagateRelations(asmDoc, 0);

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
    const auto fa =
        ConnectorLocalFrame(mate.a, asmDoc, partById, &whyA, asmById);
    const auto fb =
        ConnectorLocalFrame(mate.b, asmDoc, partById, &whyB, asmById);
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

  // A6: the same propagation, now recording the SHAPE of the walk — which
  // mates placed something (the spanning forest), which found both ends
  // already placed (the cut set, one per loop), and where each component got
  // pinned. A loop-free assembly produces an EMPTY cut set and nothing below
  // this block runs at all, so its placement stays closed-form and identical.
  LoopProblem loop;
  loop.mates.resize(asmDoc.GetMates().size());
  for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
    const Mate& m = asmDoc.GetMates()[i];
    LoopMate& lm = loop.mates[i];
    lm.id = m.id;
    lm.type = m.type;
    lm.aOcc = m.a.occurrenceId;
    lm.bOcc = m.b.occurrenceId;
    lm.fa = frames[i].fa;
    lm.fb = frames[i].fb;
    lm.flipped = m.flipped;
    lm.rotation = m.rotation;
    lm.offset = m.offset;
    lm.dx = m.dx;
    lm.dy = m.dy;
    lm.ballRotation = m.ballRotation;
  }
  {
    std::unordered_map<uint64_t, size_t> indexOfMate;
    for (size_t i = 0; i < asmDoc.GetMates().size(); i++)
      indexOfMate[asmDoc.GetMates()[i].id] = i;
    for (const Relation& r : asmDoc.GetRelations()) {
      if (!r.error.empty())
        continue;
      const auto ia = indexOfMate.find(r.mateA);
      const auto ib = indexOfMate.find(r.mateB);
      if (ia == indexOfMate.end() || ib == indexOfMate.end())
        continue;
      loop.relations.push_back(
          {ia->second, ib->second, r.dofA, r.dofB, r.ratio, r.phase});
    }
  }
  std::vector<bool> classified(asmDoc.GetMates().size(), false);
  const auto pin = [&](uint64_t occId, const glm::mat4& at) {
    world[occId] = at;
    loop.anchor.emplace_back(occId, at);
  };

  // Anchors: every `fixed` occurrence starts at its stored transform.
  for (const Occurrence& o : asmDoc.GetOccurrences())
    if (o.fixed)
      pin(o.id, OccMatrix(o));

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
          loop.tree.push_back({i, aId, bId, false});
          classified[i] = true;
          progress = true;
        } else if (bP && !aP) {
          const glm::mat4 wb = world[bId] * frames[i].fb;
          world[aId] = wb * glm::inverse(adjust) * glm::inverse(frames[i].fa);
          loop.tree.push_back({i, bId, aId, true});
          classified[i] = true;
          progress = true;
        } else if (aP && bP && !classified[i]) {
          // Both ends already placed: this mate CLOSES A LOOP. A6 collects it
          // instead of judging it here — comparing the two frames could only
          // ever say "over-constrained", which cannot tell three consistent
          // bolts through a flange from a mechanism that genuinely conflicts.
          classified[i] = true;
          loop.cut.push_back(i);
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
      pin(floatingAnchor, OccMatrix(*o));
    else
      break; // shouldn't happen (mate referenced a real occurrence)
  }

  // ---- A6: close the loops --------------------------------------------------
  report.loopMates = static_cast<int>(loop.cut.size());
  if (!loop.cut.empty()) {
    const LoopSolution closed = SolveLoops(loop);
    if (!closed.unavailable.empty()) {
      // No Eigen in this build. Say so on every mate that closes a loop —
      // an unsolved loop placed anyway would look right and be wrong.
      for (size_t idx : loop.cut) {
        asmDoc.GetMates()[idx].error = closed.unavailable;
        report.erroredMates++;
      }
      report.loopsClosed = false;
    } else {
      report.dof = closed.dof;
      report.residualMm = closed.residualMm;
      report.loopsClosed = closed.converged;
      report.redundantMates = closed.redundant;
      // MERGE, never replace: the loop pass returns placements only for the
      // components that actually had a loop, so an occurrence in a mate tree
      // elsewhere in the document must keep the one the walk just gave it.
      for (const auto& [id, m] : closed.world)
        world[id] = m;

      // A mate inside a loop has OUTPUT DOF now: turn the crank and the
      // rocker follows. Written back so the file reopens where it converged
      // — and only for mates the loop pass actually saw, so a loop-free
      // document is still never dirtied by a solve.
      for (size_t i = 0;
           i < closed.solved.size() && i < asmDoc.GetMates().size(); i++) {
        Mate& m = asmDoc.GetMates()[i];
        const LoopMate& s = closed.solved[i];
        m.rotation = s.rotation;
        m.offset = s.offset;
        m.dx = s.dx;
        m.dy = s.dy;
        m.ballRotation = s.ballRotation;
      }

      // Only a loop that could NOT close is an error, and it says by how
      // much. The old check reported 0.02 mm and 3 mm with the same words.
      for (uint64_t id : closed.conflicting) {
        if (Mate* m = asmDoc.FindMateById(id)) {
          m->error = "loop does not close (off by " +
                     FormatMm(closed.residualMm) + " mm)";
          report.erroredMates++;
        }
      }
    }
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

  // Riders last: their parents have just been placed, and an attachment is a
  // composition off the result rather than an input to it.
  report.placed += ApplyAttachments(asmDoc);
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
