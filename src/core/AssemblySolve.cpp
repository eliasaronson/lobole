// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/AssemblySolve.h"
#include "core/CamProfileMath.h"
#include "core/ContactFollower.h"
#include "core/features/CamProfileFeature.h"

#include "core/Angles.h"
#include "core/ElementOrigin.h"
#include "core/ThreadStandards.h"
#include "core/features/ThreadFeature.h"

#include "core/DatumFrame.h"
#include "core/FrameMath.h"
#include "core/features/FrameFeature.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/sketch/ExternalPrint.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace assembly {

namespace {
// The same composition ui::OccurrenceMatrix does; duplicated here (four
// lines) rather than pulling the UI layer into core, which is where every
// other assembly walk already lives.
glm::dmat4 PlacementOf(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

struct FlattenCtx {
  const PartResolver& partById;
  const AssemblyResolver& asmById;
  int maxDepth;
  std::vector<uint64_t> stack; // assembly slot ids on the current path
  std::vector<std::string> names;
  std::vector<uint64_t> ids; // occurrence ids below the root, current path
  std::vector<FlatInstance::Level> levels; // sub-assembly placements, path
  std::vector<FlatInstance> out;
  std::string error;
};

// `owner` is the placement whose sub-assembly `asmDoc` is when that
// placement is FLEXIBLE (v129): its own poses place what is inside.
bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::dmat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth, uint64_t configurationId,
                 const Occurrence* owner);

bool DescendOccurrence(FlattenCtx& ctx, const Occurrence& occ,
                       const glm::dmat4& local, const glm::dmat4& world,
                       uint64_t rootOccurrenceId, bool visible, int depth) {
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
  ctx.levels.push_back(
      {occ.id, occ.partId, occ.configurationId, local, occ.name});
  // Not the root itself: the chain records what is INSIDE it.
  if (!ctx.names.empty() && ctx.names.size() > 1)
    ctx.ids.push_back(occ.id);
  // The placement chooses the sub-assembly's configuration (v56): two
  // Gangways, one "short", one "long", from one document.
  const bool ok =
      FlattenInto(ctx, *sub, world, rootOccurrenceId, visible, depth + 1,
                  occ.configurationId, occ.flexible ? &occ : nullptr);
  ctx.levels.pop_back();
  ctx.names.pop_back();
  ctx.stack.pop_back();
  if (!ctx.ids.empty() && ctx.names.size() >= 1)
    ctx.ids.pop_back();
  return ok;
}

bool FlattenInto(FlattenCtx& ctx, const AssemblyDocument& asmDoc,
                 const glm::dmat4& parentWorld, uint64_t rootOccurrenceId,
                 bool parentVisible, int depth, uint64_t configurationId,
                 const Occurrence* owner) {
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
    const glm::dmat4 local =
        owner != nullptr ? InnerPlacement(*owner, occ) : PlacementOf(occ);
    const glm::dmat4 world = parentWorld * local;
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
      if (!DescendOccurrence(ctx, occ, local, world, root, visible, depth))
        return false;
      continue;
    }
    const Document* part = ctx.partById(occ.partId, occ.configurationId);
    if (part == nullptr)
      continue; // dangling part reference, as today
    FlatInstance fi;
    fi.partId = occ.partId;
    fi.configurationId = occ.configurationId;
    fi.material = occ.material;
    fi.part = part;
    fi.world = world;
    fi.path = prefix + occ.name;
    fi.rootOccurrenceId = root;
    fi.innerIds = ctx.ids;
    if (!ctx.names.empty())
      fi.innerIds.push_back(occ.id); // the leaf's own id inside its parent
    fi.visible = visible;
    fi.levels = ctx.levels;
    fi.occurrenceId = occ.id;
    fi.local = local;
    fi.name = occ.name;
    ctx.out.push_back(std::move(fi));
  }
  return true;
}
} // namespace

std::expected<std::vector<FlatInstance>, std::string>
Flatten(const AssemblyDocument& top, const PartResolver& partById,
        const AssemblyResolver& asmById, int maxDepth) {
  FlattenCtx ctx{partById, asmById, maxDepth, {}, {}, {}, {}, {}, {}};
  // The top itself uses its own active configuration (nobody placed it).
  if (!FlattenInto(ctx, top, glm::dmat4(1.0), 0, true, 0, 0, nullptr))
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
  const Occurrence* owner = nullptr; // the placement `doc` is inside
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
    hit.world = hit.world * (owner != nullptr && owner->flexible
                                 ? InnerPlacement(*owner, *occ)
                                 : PlacementOf(*occ));
    owner = occ;
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
      } else if (const Document* part = partById(occ.partId, 0)) {
        // One stamp per PART, whatever configuration a placement asks for
        // (a caller keys them by part): its Version, and its configurations'
        // revision, since editing a configuration a placement uses moves no
        // Version. Choosing another is an edit of the assembly itself.
        out.emplace_back(occ.partId,
                         part->Version() ^ (part->ConfigurationsRevision() *
                                            0x9E3779B97F4A7C15ull));
      }
    }
  }
  return out;
}

std::vector<std::pair<uint64_t, uint64_t>>
SubtreePartConfigurations(const AssemblyDocument& top,
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
        const AssemblyDocument* sub = asmById ? asmById(occ.partId) : nullptr;
        if (sub != nullptr && std::find(visited.begin(), visited.end(),
                                        occ.partId) == visited.end()) {
          visited.push_back(occ.partId);
          stack.push_back({sub, depth + 1});
        }
        continue;
      }
      const std::pair<uint64_t, uint64_t> key{occ.partId, occ.configurationId};
      if (std::find(out.begin(), out.end(), key) == out.end())
        out.push_back(key);
    }
  }
  return out;
}

namespace {

// The occurrence's own placement matrix (translation + orientation). Mirrors
// ui::OccurrenceMatrix, kept here so the solver has no UI dependency.
glm::dmat4 OccMatrix(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

// The one frame builder (core/FrameMath.h): origin, in-plane X hint, primary
// Z, columns X Y Z origin, exactly orthonormal.
glm::dmat4 FrameMatrix(const glm::dvec3& origin, const glm::dvec3& xHint,
                       const glm::dvec3& zAxis) {
  return frame::Build(origin, xHint, zAxis);
}

// Adjustment from a's connector frame to b's connector frame, composed as
// T(dx,dy,offset) · Rz(rotation) · R_ball · Flip. Each mate type uses a
// subset: fastened/revolute/slider/cylindrical leave dx=dy=0 and R_ball=I (so
// this reduces to the original T(0,0,offset)·Rz·Flip); planar adds the in-plane
// dx,dy; ball uses R_ball. An optional 180° flip about X makes two touching
// planar faces oppose normals. For a fastened mate with no flip and zero DOF
// this is the identity ⇒ the two frames coincide exactly.
glm::dmat4 MateAdjust(const Mate& m) {
  // ONE definition of this composition, in AssemblyLoopSolve — the loop solver
  // evaluates the same transform thousands of times per solve, and a second
  // copy of a transform in this codebase has drifted from the first before.
  return MateAdjustFor(m.flipped, m.rotation, m.offset, m.dx, m.dy,
                       m.ballRotation);
}

void Decompose(const glm::dmat4& m, glm::dvec3& position,
               glm::dquat& orientation) {
  position = glm::dvec3(m[3]);
  orientation = glm::normalize(glm::quat_cast(glm::dmat3(m)));
}

// Short, stable rendering of a millimetre figure for a mate error message.
std::string FormatMm(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", mm);
  return buf;
}

// "'crank pin' connector A (crank)": a mate by its name (or id), and which of
// its connectors, by the occurrence that connector sits on.
std::string SuspectLabel(const LoopTiltTrial& t,
                         const AssemblyDocument& asmDoc) {
  const Mate* m = asmDoc.FindMateById(t.mate);
  std::string s = "mate ";
  s += m != nullptr && !m->name.empty() ? "'" + m->name + "'"
                                        : "#" + std::to_string(t.mate);
  if (m == nullptr || t.connector == 0)
    return s;
  const Connector& c = t.connector == 'b' ? m->b : m->a;
  s += t.connector == 'b' ? " connector B" : " connector A";
  if (const Occurrence* o = asmDoc.FindById(c.occurrenceId))
    s += " (" + o->name + ")";
  return s;
}

std::string FormatDeg(double radians) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", angles::ToDegrees(radians));
  return std::string(buf) + "\xC2\xB0"; // degree sign, UTF-8
}

// What a failed loop's diagnosis adds to its cut mates' error, or nothing
// when it found no single tilted connector. Several suspects are named
// together and none is picked: the numbers do not say which.
std::string LoopHint(const LoopDiagnosis& d, const AssemblyDocument& asmDoc) {
  if (d.suspects.empty())
    return "";
  if (d.suspects.size() == 1) {
    const LoopTiltTrial& t = d.suspects.front();
    return t.connector == 0
               ? "; " + SuspectLabel(t, asmDoc) +
                     " closes it if its axis tilts " + FormatDeg(t.tiltRad) +
                     " at either connector - check both"
               : "; " + SuspectLabel(t, asmDoc) + " closes it if its axis " +
                     "tilts " + FormatDeg(t.tiltRad) +
                     " - check that connector";
  }
  std::string s = "; any one of ";
  for (size_t i = 0; i < d.suspects.size(); i++) {
    if (i > 0)
      s += i + 1 == d.suspects.size() ? " or " : ", ";
    s += SuspectLabel(d.suspects[i], asmDoc) + " (" +
         FormatDeg(d.suspects[i].tiltRad) + ")";
  }
  return s + " closes it with that tilt; the loop cannot say which - check "
             "their connectors";
}

using topo::Descent;
using topo::DescentOf;

// ---- prints of imported elements (v201) ---------------------------------
//
// A print is an OCC surface or curve analysis, and a drag solves many times
// a second: both what an element under a name is now, and where a print
// went, are kept per (part instance, part version), which only a
// regeneration moves.
struct PrintKey {
  uint64_t doc, version;
  size_t body;
  bool face;
  int index;
  auto operator<=>(const PrintKey&) const = default;
};
struct FoundKey {
  uint64_t doc, version, feature;
  bool face;
  std::string shape;
  double size;
  double x, y, z;
  auto operator<=>(const FoundKey&) const = default;
};
struct FoundAt {
  size_t body;
  int index;
  std::string name;
};
constexpr size_t kPrintCacheMax = 1u << 14;
std::mutex g_printMutex;
// The element at `index` as it now is: whether it is an imported file's
// own (only those are printed), its print, and its body's diagonal, which
// the print is compared to.
struct PrintedNow {
  bool own = false;
  std::optional<sketch::ElementPrint> print;
  double diagonal = 1.0;
};

std::map<PrintKey, PrintedNow> g_printNow;
std::map<FoundKey, std::vector<FoundAt>> g_printFound;

size_t BodyIndex(const Document& part, const Body* body) {
  return static_cast<size_t>(body - part.GetBodies().data());
}

PrintedNow PrintNow(const Document& part, const Body& body, bool face,
                    int index, const std::string& name) {
  const PrintKey key{part.InstanceId(), part.Version(), BodyIndex(part, &body),
                     face, index};
  {
    std::scoped_lock lock(g_printMutex);
    if (const auto it = g_printNow.find(key); it != g_printNow.end()) {
      PrintedNow p = it->second;
      if (p.print)
        p.print->element = name;
      return p;
    }
  }
  PrintedNow p;
  p.own = sketch::IsFileOwn(
      [&](uint64_t id) -> const Feature* { return part.FindFeatureById(id); },
      name);
  if (p.own) {
    p.print = sketch::PrintAt(body, face, index, name);
    p.diagonal = sketch::PrintDiagonal(body);
  }
  std::scoped_lock lock(g_printMutex);
  if (g_printNow.size() > kPrintCacheMax)
    g_printNow.clear();
  g_printNow[key] = p;
  return p;
}

std::vector<FoundAt> FindPrintedNow(const Document& part, uint64_t feature,
                                    bool face,
                                    const sketch::ElementPrint& was) {
  const FoundKey key{
      part.InstanceId(), part.Version(), feature,        face,
      was.shape,         was.size,       was.centroid.x, was.centroid.y,
      was.centroid.z};
  {
    std::scoped_lock lock(g_printMutex);
    if (const auto it = g_printFound.find(key); it != g_printFound.end())
      return it->second;
  }
  std::vector<FoundAt> out;
  for (const sketch::Found& f : sketch::FindPrinted(
           part.GetBodies(),
           [&](uint64_t id) -> const Feature* {
             return part.FindFeatureById(id);
           },
           feature, face, was))
    out.push_back({BodyIndex(part, f.body), f.index, f.name});
  std::scoped_lock lock(g_printMutex);
  if (g_printFound.size() > kPrintCacheMax)
    g_printFound.clear();
  g_printFound[key] = out;
  return out;
}

// Checks the element at `index` (named `name`) on `*body` against its
// stored print, following it when the file moved it: `*body` and `index`
// then say where it went. `now` receives its print as it stands (empty when
// it is not an imported file's own element), `followed` the name it moved
// to. False with `why` when nothing, or more than one thing, is it.
bool CheckPrinted(const Document& part, const Body*& body, bool face,
                  int& index, const std::string& name,
                  const std::optional<sketch::ElementPrint>& stored,
                  std::optional<sketch::ElementPrint>* now,
                  std::string* followed, std::string& why) {
  const bool checked = stored && stored->element == name;
  if (!checked && now == nullptr)
    return true;
  const PrintedNow at = PrintNow(part, *body, face, index, name);
  if (!at.own)
    return true;
  std::optional<sketch::ElementPrint> current = at.print;
  if (checked &&
      !(current && sketch::SamePrint(*stored, *current, at.diagonal))) {
    std::vector<FoundAt> found =
        FindPrintedNow(part, body->createdBy, face, *stored);
    // A snap edge stays on the body its face is on.
    if (!face)
      std::erase_if(found, [&](const FoundAt& f) {
        return f.body != BodyIndex(part, body);
      });
    const char* what = face ? "face" : "snap edge";
    if (found.size() != 1) {
      why = std::string("its ") + what + " '" + name +
            "' is not the one it was picked on, " +
            sketch::Describe(*stored, face) + ": the imported file " +
            (found.empty() ? std::string("has no such ") + what + " now"
                           : "now has " + std::to_string(found.size()) +
                                 " alike, and which one is meant is not "
                                 "known") +
            " -- re-pick the connector";
      return false;
    }
    body = &part.GetBodies()[found[0].body];
    index = found[0].index;
    if (followed)
      *followed = found[0].name;
    current = *stored;
    current->element = found[0].name;
  }
  if (now)
    *now = current;
  return true;
}

void TakePrints(Connector& c, const ConnectorPrints& p) {
  if (!c.anchor.takePrints)
    return;
  const auto take = [](const ElementRef& ref,
                       std::optional<sketch::ElementPrint>& stored,
                       const std::optional<sketch::ElementPrint>& now,
                       const std::string& followed) {
    if (!followed.empty() || !now || !ref.IsSet())
      return; // a followed print is the stored one; nothing to take
    if (!stored || stored->element != ref.name)
      stored = now;
  };
  take(c.anchor.face, c.anchor.facePrint, p.face, p.faceFollowed);
  take(c.anchor.snapEdge, c.anchor.snapPrint, p.snap, p.snapFollowed);
  take(c.anchor.snapEdge2, c.anchor.snap2Print, p.snap2, p.snap2Followed);
}

} // namespace

std::optional<glm::dmat4>
ConnectorLocalFrame(const Connector& c, const AssemblyDocument& asmDoc,
                    const PartResolver& partById, std::string* whyNot,
                    const AssemblyResolver& asmById, bool followModified,
                    ConnectorPrints* prints) {
  const auto fail = [&](std::string why) {
    if (whyNot)
      *whyNot = std::move(why);
    return std::nullopt;
  };
  const Occurrence* occ = asmDoc.FindById(c.occurrenceId);
  if (!occ)
    return fail("its occurrence no longer exists");

  // A5/v57: walk into the sub-assembly to the part the anchor means, keeping
  // the transform from the UNIT's frame down to that part. Everything below
  // then works on a part exactly as it always did, and the frame comes back
  // pre-multiplied — so the caller still just composes the occurrence matrix.
  glm::dmat4 innerXf(1.0);
  const Document* partForFace = nullptr;
  if (!c.innerPath.empty()) {
    if (occ->refKind != OccurrenceRef::Assembly)
      return fail("it has a path into a sub-assembly but places a part");
    const AssemblyDocument* doc = asmById ? asmById(occ->partId) : nullptr;
    if (doc == nullptr)
      return fail("its sub-assembly is not loaded");
    const Occurrence* inner = nullptr;
    const Occurrence* owner = occ; // whose document `doc` is
    for (size_t i = 0; i < c.innerPath.size(); i++) {
      if (doc == nullptr)
        return fail("its path goes deeper than the assembly does");
      inner = doc->FindById(c.innerPath[i]);
      if (inner == nullptr)
        return fail("the part it points at inside the sub-assembly is gone");
      // A flexible placement (v129) places what is inside it at its own
      // poses, and a connector reads the unit as it stands.
      innerXf = innerXf * (owner->flexible ? InnerPlacement(*owner, *inner)
                                           : OccMatrix(*inner));
      owner = inner;
      doc = inner->refKind == OccurrenceRef::Assembly && asmById
                ? asmById(inner->partId)
                : nullptr;
    }
    // An ORIGIN anchor with a path is the origin of what the path names —
    // a part inside the unit, or a sub-assembly inside it — not the unit's
    // own origin, which it used to answer before walking the path at all.
    if (c.anchor.origin)
      return innerXf;
    const Document* innerPart = partById(inner->partId, inner->configurationId);
    if (innerPart == nullptr)
      return fail("the part inside the sub-assembly is not loaded");
    partForFace = innerPart; // resolve the face on the part the path names
  }
  if (c.anchor.origin)
    return glm::dmat4(1.0); // the part's coordinate origin

  const Document* part =
      partForFace ? partForFace : partById(occ->partId, occ->configurationId);
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
    // Or a DatumPlane / DatumAxis: a scan's fitted datums
    // (docs/modeling/README.md#datums-on-a-scan).
    std::string anchors;
    const Feature* fr =
        datumframe::Find(*part, nlohmann::json(c.anchor.frameId), anchors);
    if (fr == nullptr) {
      return fail("frame " + std::to_string(c.anchor.frameId) +
                  " is not a Frame, DatumPlane or DatumAxis of its part; "
                  "there: " +
                  (anchors.empty() ? std::string("none") : anchors));
    }
    if (!fr->GetError().empty())
      return fail(std::string(datumframe::KindLabel(*fr)) + " '" +
                  fr->GetName() + "' did not resolve: " + fr->GetError());
    return innerXf * datumframe::Matrix(*fr);
  }

  const ElementRef& ref = c.anchor.face;
  for (const Body& body : part->GetBodies()) {
    if (body.createdBy != ref.featureId)
      continue;
    auto faceIdx = body.elementMap.Resolve(ref.name);
    if (!faceIdx && followModified) {
      // A seat follows its face through later modifications: the one face
      // whose name is this one plus whole, unsplit `;M<tag>` steps. Any
      // other descendant beside it (a split piece) and there is no one face.
      int descendants = 0;
      std::optional<int> modified;
      for (const auto& [idx, name] : body.elementMap.names) {
        const Descent d = DescentOf(ref.name, name);
        if (d == Descent::None)
          continue;
        ++descendants;
        if (d == Descent::Modified)
          modified = idx;
      }
      if (descendants == 1 && modified)
        faceIdx = modified;
    }
    if (!faceIdx) {
      // Owner found, name no longer resolves. The usual cause: a feature
      // added AFTER the mate modified the face, extending its history name
      // ("6/g1;M6" -> "6/g1;M6;M7") — suggest current names that keep the
      // failing name as a prefix. A SPLIT is said as one: "39/p1/bore;M39"
      // becoming "...;M39.0" and "...;M39.1" is a face cut in two (here a
      // hole breaking out through the part's end), which no re-pick of "the
      // new name" repairs.
      std::string similar;
      int pieces = 0;
      uint64_t splitBy = 0;
      for (const auto& [idx, name] : body.elementMap.names) {
        (void)idx;
        if (name.size() > ref.name.size() &&
            name.compare(0, ref.name.size(), ref.name) == 0) {
          if (!similar.empty())
            similar += ", ";
          similar += name;
          uint64_t by = 0;
          if (DescentOf(ref.name, name, &by) == Descent::Split) {
            ++pieces;
            if (splitBy == 0)
              splitBy = by;
          }
        }
      }
      std::string why = "face '" + ref.name + "' not found on its part";
      if (!similar.empty() && pieces > 0)
        why += "; similar: " + similar + " (feature " +
               std::to_string(splitBy) + " split the face into " +
               std::to_string(pieces) +
               " pieces, so the name means no one face — find out what cut "
               "it, or re-pick)";
      else if (!similar.empty())
        why += "; similar: " + similar +
               " (the face was modified by a later feature — update the "
               "connector to the new name)";
      return fail(std::move(why));
    }

    // A mesh body has no face to put a connector on
    // (docs/interchange/README.md#mesh-bodies).
    if (std::string why = MeshBodyRefusal(body, "a mate's connector");
        !why.empty())
      return fail(std::move(why));
    // An imported face is the one picked, or followed to where the file
    // moved it (v201).
    const Body* on = &body;
    int faceAt = *faceIdx;
    if (std::string why;
        !CheckPrinted(*part, on, true, faceAt, ref.name, c.anchor.facePrint,
                      prints ? &prints->face : nullptr,
                      prints ? &prints->faceFollowed : nullptr, why))
      return fail(std::move(why));
    faceIdx = faceAt;

    glm::dvec3 origin, primary, xHint;
    bool ok = on->shape.FacePlane(*faceIdx, origin, primary, xHint) ||
              on->shape.FaceAxis(*faceIdx, origin, primary, xHint);
    if (!ok)
      return fail("face '" + ref.name +
                  "' has no connector frame (planar, cylindrical, conical, "
                  "and spherical faces do)");

    // Inference snap (A2.5 phase 4): override the origin with the snapped
    // edge's characteristic point (a hole center, etc.), keeping the face's Z
    // normal.
    if (c.anchor.snapEdge.IsSet()) {
      if (auto edgeIdx = on->elementMap.ResolveEdge(c.anchor.snapEdge.name)) {
        // An imported edge as its face is: checked, and followed.
        const Body* edgeOn = on;
        int edgeAt = *edgeIdx;
        if (std::string why;
            !CheckPrinted(*part, edgeOn, false, edgeAt, c.anchor.snapEdge.name,
                          c.anchor.snapPrint, prints ? &prints->snap : nullptr,
                          prints ? &prints->snapFollowed : nullptr, why))
          return fail(std::move(why));
        edgeIdx = edgeAt;
        glm::dvec3 snap;
        // A CORNER (v65) when a second edge is named: the vertex the two
        // share. Falls back to the first edge's own point when they share
        // none or more than one — an ambiguous pair names no single corner,
        // and a plausible guess is worse than the point that IS well defined.
        bool cornered = false;
        if (c.anchor.snapEdge2.IsSet()) {
          if (auto edgeIdx2 =
                  on->elementMap.ResolveEdge(c.anchor.snapEdge2.name)) {
            const Body* edge2On = on;
            int edge2At = *edgeIdx2;
            if (std::string why; !CheckPrinted(
                    *part, edge2On, false, edge2At, c.anchor.snapEdge2.name,
                    c.anchor.snap2Print, prints ? &prints->snap2 : nullptr,
                    prints ? &prints->snap2Followed : nullptr, why))
              return fail(std::move(why));
            cornered = on->shape.CornerPoint(*edgeIdx, edge2At, snap);
          }
        }
        if (cornered || on->shape.EdgeSnapPoint(*edgeIdx, snap))
          origin = snap;
      } else {
        // Gone, the connector is NOT the face's own frame: that is a
        // different point (the centroid), and a bolted joint snapped to its
        // hole would quietly slide the parts by the distance between them.
        return fail("its snap edge '" + c.anchor.snapEdge.name +
                    "' is not on its part any more (consumed by a later "
                    "feature, a fillet or chamfer on it?) — re-pick the "
                    "connector");
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

namespace {
// One face of a seat, resolved as a mate connector on it would be. A memo
// (per ApplyAttachments call) answers a face many riders share once: every
// screw of a pattern bears on the same plate face, and resolving a face
// computes its centroid.
using FaceMemo =
    std::map<std::string, std::pair<std::optional<glm::dmat4>, std::string>>;

std::optional<glm::dmat4>
SeatFaceFrame(const ElementRef& face, const std::vector<uint64_t>& innerPath,
              const Occurrence& parent, const AssemblyDocument& asmDoc,
              const PartResolver& partById, const AssemblyResolver& asmById,
              std::string& why, FaceMemo* memo) {
  std::string key;
  if (memo != nullptr) {
    key = std::to_string(parent.id) + "/" + std::to_string(face.featureId) +
          "/" + face.name;
    for (const uint64_t step : innerPath)
      key += "|" + std::to_string(step);
    if (const auto it = memo->find(key); it != memo->end()) {
      why = it->second.second;
      return it->second.first;
    }
  }
  Connector c;
  c.occurrenceId = parent.id;
  c.innerPath = innerPath;
  c.anchor.face = face;
  const auto frame = ConnectorLocalFrame(c, asmDoc, partById, &why, asmById,
                                         /*followModified=*/true);
  if (memo != nullptr)
    (*memo)[key] = {frame, why};
  return frame;
}

std::optional<glm::dmat4> SeatFrameMemo(const AttachSeat& seat,
                                        const Occurrence& parent,
                                        const AssemblyDocument& asmDoc,
                                        const PartResolver& partById,
                                        const AssemblyResolver& asmById,
                                        std::string* whyNot, FaceMemo* memo) {
  const auto fail = [&](std::string why) -> std::optional<glm::dmat4> {
    if (whyNot)
      *whyNot = std::move(why);
    return std::nullopt;
  };
  if (!seat.IsSet())
    return fail("it names no seat");
  if (!partById)
    return fail("nothing resolves the part it is seated in");
  std::string why;
  const auto onFace = SeatFaceFrame(seat.face, seat.innerPath, parent, asmDoc,
                                    partById, asmById, why, memo);
  if (!onFace)
    return fail("its face: " + why);
  const auto onAxis = SeatFaceFrame(seat.axis, seat.innerPath, parent, asmDoc,
                                    partById, asmById, why, memo);
  if (!onAxis)
    return fail("its axis: " + why);
  const glm::dvec3 n = glm::normalize(glm::dvec3((*onFace)[2]));
  const glm::dvec3 inPlane((*onFace)[3]);
  const glm::dvec3 d = glm::normalize(glm::dvec3((*onAxis)[2]));
  const glm::dvec3 onLine((*onAxis)[3]);
  // Square, as a hole is to the face it opens in and a groove's floor to
  // its flank. An axis that has tilted off its face (the names now meaning
  // different faces) has no one point to seat anything at, and picking one
  // is how hardware ends up in a wall.
  const double c0 = glm::dot(d, n);
  if (std::abs(c0) < 1.0 - 1e-6)
    return fail("its axis is no longer square to its face (" +
                FormatDeg(std::acos(std::min(1.0, std::abs(c0)))) + " off)");
  const glm::dvec3 origin = onLine + d * (glm::dot(inPlane - onLine, n) / c0);
  // The twist about the axis from the PARENT's axes, never a face's own
  // parametrization, which a regeneration is free to change.
  const glm::dvec3 xHint = std::abs(n.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0)
                                               : glm::dvec3(0.0, 1.0, 0.0);
  return FrameMatrix(origin, xHint, n);
}
} // namespace

std::optional<glm::dmat4>
SeatFrame(const AttachSeat& seat, const Occurrence& parent,
          const AssemblyDocument& asmDoc, const PartResolver& partById,
          const AssemblyResolver& asmById, std::string* whyNot) {
  return SeatFrameMemo(seat, parent, asmDoc, partById, asmById, whyNot,
                       nullptr);
}

bool SeatRider(const AssemblyDocument& asmDoc, Occurrence& rider,
               const AttachSeat& seat, const PartResolver& partById,
               const AssemblyResolver& asmById, std::string* whyNot) {
  const Occurrence* parent =
      rider.attachedTo != 0 ? asmDoc.FindById(rider.attachedTo) : nullptr;
  if (parent == nullptr) {
    if (whyNot)
      *whyNot = "it rides on nothing";
    return false;
  }
  const auto frame =
      SeatFrame(seat, *parent, asmDoc, partById, asmById, whyNot);
  if (!frame)
    return false;
  glm::dvec3 pos;
  glm::dquat rot;
  Decompose(glm::inverse(PlacementOf(*parent) * *frame) * PlacementOf(rider),
            pos, rot);
  rider.attachSeat = seat;
  rider.seatCacheDoc = 0; // a new seat: resolve it afresh
  rider.attachOffset = pos;
  rider.attachRotation = rot;
  rider.attachWarning.clear();
  rider.attachError.clear();
  return true;
}

namespace {
// The parts under `parent` a hole can be in, each with its placement in the
// parent's space and the path a Connector names it by.
struct HoleLeaf {
  const Document* part = nullptr;
  glm::dmat4 unit{1.0};
  std::vector<uint64_t> innerPath;
};

std::vector<HoleLeaf> LeavesOf(const AssemblyDocument& asmDoc,
                               const Occurrence& parent,
                               const PartResolver& partById,
                               const AssemblyResolver& asmById) {
  std::vector<HoleLeaf> out;
  if (!partById)
    return out;
  if (parent.refKind == OccurrenceRef::Part) {
    if (const Document* part = partById(parent.partId, parent.configurationId))
      out.push_back({part, glm::dmat4(1.0), {}});
    return out;
  }
  const auto flat = Flatten(asmDoc, partById, asmById);
  if (!flat)
    return out;
  const glm::dmat4 toUnit = glm::inverse(PlacementOf(parent));
  for (const FlatInstance& inst : *flat)
    if (inst.rootOccurrenceId == parent.id && inst.part != nullptr)
      out.push_back({inst.part, toUnit * inst.world, inst.innerIds});
  return out;
}
} // namespace

std::optional<AttachSeat>
FindHoleSeat(const AssemblyDocument& asmDoc, const Occurrence& parent,
             const glm::dmat4& riderWorld, const PartResolver& partById,
             const AssemblyResolver& asmById, std::string* whyNot) {
  const auto fail = [&](std::string why) -> std::optional<AttachSeat> {
    if (whyNot)
      *whyNot = std::move(why);
    return std::nullopt;
  };
  const std::vector<HoleLeaf> leaves =
      LeavesOf(asmDoc, parent, partById, asmById);
  if (leaves.empty())
    return fail("its part is not loaded");
  // The piece's axis in the parent's space: a fastener's origin is its
  // bearing face and its Z the axis it sits on.
  const glm::dmat4 rider = glm::inverse(PlacementOf(parent)) * riderWorld;
  const glm::dvec3 at(rider[3]);
  const glm::dvec3 axis = glm::normalize(glm::dvec3(rider[2]));
  // Coincidence, not a design size: what OCC's exact geometry and a pose
  // written in floats agree to.
  constexpr double kOn = 1e-3;     // mm
  constexpr double kSquare = 1e-6; // 1 - |cos|
  const auto offAxis = [&](const glm::dvec3& p) {
    const glm::dvec3 v = p - at;
    return glm::length(v - glm::dot(v, axis) * axis);
  };

  struct Face {
    const HoleLeaf* leaf;
    const Body* body;
    int index;
    double s;
    glm::dvec3 point, normal;
  };
  std::optional<Face> best;
  std::vector<CADShape::FaceSnap> snaps;
  for (const HoleLeaf& leaf : leaves) {
    for (const Body& body : leaf.part->GetBodies()) {
      if (body.reference)
        continue;
      for (const auto& [index, name] : body.elementMap.names) {
        glm::dvec3 c, n, u;
        if (!body.shape.FacePlane(index, c, n, u))
          continue;
        const glm::dvec3 nu =
            glm::normalize(glm::dvec3(leaf.unit * glm::dvec4(n, 0.0)));
        if (std::abs(glm::dot(nu, axis)) < 1.0 - kSquare)
          continue;
        const glm::dvec3 cu(leaf.unit * glm::dvec4(c, 1.0));
        // The piece stands on the face's OUTSIDE, on it or beyond it by
        // what is stacked between (a washer under a head).
        const double s = glm::dot(at - cu, nu);
        if (s < -kOn || (best && s >= best->s))
          continue;
        // ...and the face is this hole's: a circle of its edge on the axis.
        snaps.clear();
        body.shape.FaceSnapPoints(index, snaps);
        const bool rimmed =
            std::any_of(snaps.begin(), snaps.end(), [&](const auto& sp) {
              return sp.circleCenter &&
                     offAxis(glm::dvec3(leaf.unit *
                                        glm::dvec4(sp.point, 1.0))) < kOn;
            });
        if (rimmed)
          best = Face{&leaf, &body, index, s, cu, nu};
      }
    }
  }
  if (!best)
    return fail("no face of " + parent.name +
                " with a hole on the piece's axis faces it");

  // The bore: coaxial, and opening in that face's plane. A cylinder running
  // INTO the material first (the bore under a counterbore floor), then any
  // cylinder, then a cone (a countersink, whose rim is on the face).
  const HoleLeaf& leaf = *best->leaf;
  const Body& body = *best->body;
  int bore = -1;
  int rank = 4;
  for (const auto& [index, name] : body.elementMap.names) {
    glm::dvec3 o, a, x;
    if (body.shape.FacePlane(index, o, a, x) ||
        !body.shape.FaceAxis(index, o, a, x))
      continue;
    const auto info = Measure::DescribeFaceAt(body, index);
    if (!info || (info->kind != "cylinder" && info->kind != "cone"))
      continue;
    const glm::dvec3 au =
        glm::normalize(glm::dvec3(leaf.unit * glm::dvec4(a, 0.0)));
    if (std::abs(glm::dot(au, axis)) < 1.0 - kSquare ||
        offAxis(glm::dvec3(leaf.unit * glm::dvec4(o, 1.0))) > kOn)
      continue;
    snaps.clear();
    body.shape.FaceSnapPoints(index, snaps);
    bool inPlane = false, behind = false;
    for (const auto& sp : snaps) {
      if (!sp.circleCenter)
        continue;
      const double h = glm::dot(
          glm::dvec3(leaf.unit * glm::dvec4(sp.point, 1.0)) - best->point,
          best->normal);
      inPlane |= std::abs(h) < kOn;
      behind |= h < -kOn;
    }
    if (!inPlane)
      continue;
    const int r = info->kind == "cylinder" ? (behind ? 0 : 1) : 2;
    if (r < rank) {
      rank = r;
      bore = index;
    }
  }
  if (bore < 0)
    return fail("the face it bears on in " + parent.name +
                " has no bore on the piece's axis");
  AttachSeat seat;
  seat.face = ElementRef{body.createdBy, body.elementMap.names.at(best->index)};
  seat.axis = ElementRef{body.createdBy, body.elementMap.names.at(bore)};
  seat.innerPath = leaf.innerPath;
  return seat;
}

bool BindRiderToHole(const AssemblyDocument& asmDoc, Occurrence& rider,
                     const PartResolver& partById,
                     const AssemblyResolver& asmById) {
  rider.attachSearched = true;
  const Occurrence* parent =
      rider.attachedTo != 0 ? asmDoc.FindById(rider.attachedTo) : nullptr;
  if (parent == nullptr)
    return false;
  std::string why;
  const auto seat = FindHoleSeat(asmDoc, *parent, PlacementOf(rider), partById,
                                 asmById, &why);
  if (!seat || !SeatRider(asmDoc, rider, *seat, partById, asmById, &why)) {
    rider.attachWarning = "rides " + parent->name +
                          " at a fixed offset: " + why +
                          ", so it will not follow a hole if the part changes";
    return false;
  }
  return true;
}

int BindRidersToHoles(AssemblyDocument& asmDoc, const PartResolver& partById,
                      const AssemblyResolver& asmById) {
  int bound = 0;
  for (Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.attachedTo == 0 || o.attachSeat.IsSet() || o.attachSearched)
      continue;
    const Occurrence* parent = asmDoc.FindById(o.attachedTo);
    if (parent == nullptr || parent->attachedTo != 0)
      continue; // ApplyAttachments leaves these alone too
    // Where the old offset puts it NOW: the parent may have moved since the
    // rider's pose was written.
    Decompose(PlacementOf(*parent) *
                  glm::translate(glm::dmat4(1.0), o.attachOffset) *
                  glm::mat4_cast(o.attachRotation),
              o.position, o.orientation);
    if (BindRiderToHole(asmDoc, o, partById, asmById))
      bound++;
  }
  return bound;
}

int ApplyAttachments(AssemblyDocument& asmDoc, const PartResolver& partById,
                     const AssemblyResolver& asmById, int* errored) {
  int moved = 0;
  int failed = 0;
  FaceMemo memo;
  for (Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.attachedTo == 0)
      continue;
    const Occurrence* parent = asmDoc.FindById(o.attachedTo);
    if (parent == nullptr)
      continue; // the parent went; the rider keeps its last pose
    if (parent->attachedTo != 0)
      continue; // one level (see the header)
    glm::dmat4 seat(1.0);
    if (o.attachSeat.IsSet()) {
      // The part the seat is in, when the parent places one directly: what
      // the cached frame was resolved against. A path into a sub-assembly
      // is resolved every time (a flexible unit moves what is inside it).
      const Document* leaf =
          partById && o.attachSeat.innerPath.empty() &&
                  parent->refKind == OccurrenceRef::Part
              ? partById(parent->partId, parent->configurationId)
              : nullptr;
      if (leaf != nullptr && o.seatCacheDoc == leaf->InstanceId() &&
          o.seatCacheVersion == leaf->Version()) {
        seat = o.seatCacheFrame;
      } else {
        std::string why;
        const auto frame = SeatFrameMemo(o.attachSeat, *parent, asmDoc,
                                         partById, asmById, &why, &memo);
        o.seatCacheDoc = 0;
        if (!frame) {
          // Left where it was, and SAID: a screw standing where its hole
          // used to be is the plausible-and-wrong this must not be quiet
          // about.
          o.attachError = "its seat in " + parent->name + " is gone: " + why;
          failed++;
          continue;
        }
        o.attachError.clear();
        seat = *frame;
        if (leaf != nullptr) {
          o.seatCacheDoc = leaf->InstanceId();
          o.seatCacheVersion = leaf->Version();
          o.seatCacheFrame = seat;
        }
      }
    }
    const glm::dmat4 world = PlacementOf(*parent) * seat *
                             glm::translate(glm::dmat4(1.0), o.attachOffset) *
                             glm::mat4_cast(o.attachRotation);
    glm::dvec3 pos;
    glm::dquat rot;
    Decompose(world, pos, rot);
    if (pos != o.position || rot != o.orientation) {
      o.position = pos;
      o.orientation = rot;
      moved++;
    }
  }
  if (errored)
    *errored = failed;
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
    m.rotation = value;
  else
    m.offset = value;
}

namespace {
// Where a toothed freewheel DRIVES from: the notch at or behind its lead,
// in the direction it drives. Between notches it is riding over a tooth, and
// the input has to travel the rest of the pitch before it drives again —
// a ratchet's dead band. Without teeth the lead is the whole of it.
double FreewheelNotch(const Relation& r) {
  if (r.teeth <= 0)
    return r.lead;
  const double pitch = 2.0 * M_PI / r.teeth;
  const double k = r.lead / pitch;
  return (r.drives > 0 ? std::floor(k + 1e-9) : std::ceil(k - 1e-9)) * pitch;
}
} // namespace

std::shared_ptr<const cammech::PreparedMotion>
CamProfileMotion(const Relation& r, const PartResolver& partById,
                 std::string* why) {
  const Document* doc = partById ? partById(r.profilePart, 0) : nullptr;
  if (doc == nullptr) {
    *why = "the cam profile's part (" + std::to_string(r.profilePart) +
           ") is not loaded";
    return nullptr;
  }
  const CamProfileFeature* cp = nullptr;
  for (size_t i = 0; i < doc->GetFeatures().size(); ++i) {
    if (doc->EffectiveRollback() >= 0 &&
        i >= static_cast<size_t>(doc->EffectiveRollback()))
      break;
    const auto& f = doc->GetFeatures()[i];
    if (f->GetId() == r.profileFeature && !doc->EffectiveSuppressed(i))
      cp = dynamic_cast<const CamProfileFeature*>(f.get());
  }
  if (cp == nullptr) {
    *why = "feature " + std::to_string(r.profileFeature) +
           " of that part is not a CamProfile (or is suppressed)";
    return nullptr;
  }
  if (!cp->report.valid) {
    *why = "the cam profile '" + cp->GetName() + "' did not build" +
           (cp->GetError().empty() ? std::string() : ": " + cp->GetError());
    return nullptr;
  }
  const bool turns =
      cp->report.follower.motion == cammech::FollowerMotion::Turn;
  if (turns != (r.dofB == MateDof::Rotation)) {
    *why = std::string("the cam profile was cut for a ") +
           (turns ? "turning" : "sliding") + " follower, and b " +
           (r.dofB == MateDof::Rotation ? "turns" : "slides");
    return nullptr;
  }
  return std::make_shared<const cammech::PreparedMotion>(cp->report.motion,
                                                         cp->report.follower);
}

double RelationValue(const Relation& r, double a) {
  // A contact relation reads the table its geometry filled
  // (docs/assemblies/relations.md#contact).
  if (r.kind == RelationKind::Contact)
    return (r.contactModel ? contact::TableValue(r.contactModel->table, a)
                           : 0.0) +
           r.phase;
  // A cam's table is in the follower's unit, so a turning follower's lift is
  // DEGREES and becomes the radians a rotation DOF holds here.
  // A cam reading its CamProfile: the motion it was cut for, in b's unit
  // already (radians for a turn).
  if (r.kind == RelationKind::Cam && r.profileMotion)
    return r.profileMotion->At(angles::ToDegrees(a)).s + r.phase;
  if (r.kind == RelationKind::Cam) {
    const double lift = CamLift(r.lift, a);
    return (r.dofB == MateDof::Rotation ? angles::ToRadians(lift) : lift) +
           r.phase;
  }
  // A carry steps once a period of a and stands still between
  // (docs/assemblies/relations.md#intermittent).
  if (r.kind == RelationKind::Intermittent)
    return IntermittentValue(r, a) + r.phase;
  // A click stands at the mark it was last carried to or held at.
  if (r.kind == RelationKind::Click)
    return r.lead;
  // A freewheel carries its history in `lead`
  // (docs/assemblies/relations.md#freewheel).
  if (r.kind == RelationKind::Freewheel)
    return r.ratio * a + r.phase + FreewheelNotch(r);
  return r.ratio * a + r.phase;
}

namespace {
// How far a freewheel's output may stand behind its drive line and still
// count as ON it. The line is recomputed on every pass, and a loop solve may
// have moved the output by its closing tolerance since; without this slack a
// second propagation would read that as the input going backwards and grow
// `lead` by rounding noise on every pass.
double FreewheelSlack(double v) { return 1e-6 * std::max(1.0, std::abs(v)); }
} // namespace

int PropagateRelations(AssemblyDocument& asmDoc, uint64_t drivenMateId,
                       const PartResolver& partById) {
  int wrote = 0;
  std::vector<Relation>& rels = asmDoc.GetRelations();
  // Validate first: a relation with a problem is reported and skipped, and
  // never half-applied.
  std::vector<bool> usable(rels.size(), false);
  for (size_t i = 0; i < rels.size(); i++) {
    Relation& r = rels[i];
    r.error.clear();
    r.held.clear();
    r.heldTerms.clear();
    // Out of the active configuration: not applied, and not an error.
    if (asmDoc.RelationSuppressed(r.id))
      continue;
    if (r.HasTerms()) {
      // A relation over several mates (v171). A member inside a flexible
      // placement is applied only once the solve has expanded it, like a
      // binary relation's side named by path below.
      bool byPath = false;
      for (const RelationTerm& t : r.terms) {
        if (t.path.empty())
          continue;
        byPath = true;
        const Occurrence* o = asmDoc.FindById(t.path.front());
        if (o == nullptr)
          r.error = "the placement its path names is gone";
        else if (t.path.size() != 1)
          r.error = "its path goes more than one level down, and only one "
                    "level of sub-assembly is flexible";
        else if (!o->flexible)
          r.error = "it names a mate inside '" + o->name +
                    "', which is not flexible — set_occurrence_flexible to "
                    "let its mechanism move here";
      }
      if (byPath)
        continue;
      if (std::string why = RelationProblem(asmDoc, r); !why.empty()) {
        r.error = std::move(why);
        continue;
      }
      usable[i] = true;
      continue;
    }
    // A side named by a path (v129) is a mate inside a flexible placement.
    // Only the solve can apply it — it expands the placement first — so
    // here it is checked for its path and otherwise left alone.
    if (!r.mateAPath.empty() || !r.mateBPath.empty()) {
      for (const std::vector<uint64_t>* path : {&r.mateAPath, &r.mateBPath}) {
        if (path->empty())
          continue;
        const Occurrence* o = asmDoc.FindById(path->front());
        if (o == nullptr)
          r.error = "the placement its path names is gone";
        else if (path->size() != 1)
          r.error = "its path goes more than one level down, and only one "
                    "level of sub-assembly is flexible";
        else if (!o->flexible)
          r.error = "it names a mate inside '" + o->name +
                    "', which is not flexible — set_occurrence_flexible to "
                    "let its mechanism move here";
      }
      continue;
    }
    if (r.threadPart || r.threadFeature) {
      if (!partById) {
        r.error = "thread-backed relation needs a part resolver";
        continue;
      }
      const Document* doc = partById(r.threadPart, 0);
      const ThreadFeature* thread = nullptr;
      if (doc)
        for (size_t i = 0; i < doc->GetFeatures().size(); ++i) {
          if (doc->EffectiveRollback() >= 0 &&
              i >= static_cast<size_t>(doc->EffectiveRollback()))
            break;
          const auto& f = doc->GetFeatures()[i];
          if (f->GetId() == r.threadFeature && !doc->EffectiveSuppressed(i))
            thread = dynamic_cast<const ThreadFeature*>(f.get());
        }
      if (!thread || thread->IsSuppressed() || r.kind != RelationKind::Screw ||
          !thread->GetError().empty()) {
        r.error = "screw thread reference is missing, invalid or unsupported";
        continue;
      }
      // The table's pitch for a known designation, else the custom thread's
      // own -- what every printed thread is (PrintedThreadFor models one so).
      // A left-hand thread advances the other way for the same turn.
      const auto spec = threads::Lookup(thread->designation);
      const double pitch = spec ? spec->pitch : thread->pitch;
      if (!(pitch > 0.0)) {
        r.error = "the screw's thread has no pitch";
        continue;
      }
      r.ratio = (thread->rightHanded ? 1.0 : -1.0) * pitch /
                (2.0 * 3.14159265358979323846);
    }
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
    if (r.mateA == r.mateB && r.dofA == r.dofB &&
        r.kind != RelationKind::Click) {
      r.error = "a relation cannot couple a degree of freedom to itself";
      continue;
    }
    if (r.kind == RelationKind::Click && r.mateA != r.mateB) {
      r.error = "a click holds ONE mate's degree of freedom: a and b are the "
                "same mate";
      continue;
    }
    if ((r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click) &&
        r.drives != 1 && r.drives != -1) {
      r.error = "a freewheel drives positive or negative; this one has no "
                "direction";
      continue;
    }
    if (r.kind == RelationKind::Contact) {
      // The follower's pose comes from the two bodies' shapes: the table is
      // built once per geometry (cached) and read below like a cam's.
      r.contactModel.reset();
      std::string why;
      auto model = contact::ModelFor(asmDoc, r, partById, &why);
      if (!model) {
        r.error = std::move(why);
        continue;
      }
      r.contactModel = std::move(model);
      usable[i] = true;
      continue;
    }
    if (r.kind == RelationKind::Cam &&
        (r.profilePart != 0 || r.profileFeature != 0)) {
      // Re-read on every solve, as a thread-backed screw reads its pitch:
      // an edit to the profile moves the follower.
      r.profileMotion = CamProfileMotion(r, partById, &r.error);
      if (!r.profileMotion)
        continue;
    } else if (r.kind == RelationKind::Cam && r.lift.size() < 2) {
      r.error = "a cam relation needs a lift table of at least two "
                "(angle, lift) points";
      continue;
    }
    if (r.kind == RelationKind::Intermittent) {
      const Relation::Intermittent& im = r.intermittent;
      if (im.periodDeg <= 0.0 || im.durationDeg <= 0.0 ||
          im.durationDeg > im.periodDeg || im.step == 0.0) {
        r.error = "an intermittent relation needs a step, a period and a "
                  "window inside it (0 < duration <= period)";
        continue;
      }
      if (im.slots != 0 && (im.slots < 3 || r.dofB != MateDof::Rotation)) {
        r.error = "a Geneva has at least 3 slots and its wheel turns";
        continue;
      }
      if (!im.table.empty())
        if (std::string why = IntermittentTableError(im.table); !why.empty()) {
          r.error = std::move(why);
          continue;
        }
    }
    usable[i] = true;
  }
  // The graph's nodes are (mate, degree of freedom) PAIRS, not mates. A
  // cylindrical mate's turn and its slide are two nodes, so each takes its
  // own relation: the watch's clutch is turned by its stem and slid by its
  // yoke. Settling a whole MATE the first time any relation reached it
  // accepted the second relation and never applied it.
  //
  // Walk from the driven mate FIRST, and only then from whatever it did not
  // reach — ONE root at a time. A node reached once is settled; a later
  // relation that would move it again is either a loop row (the loop pass
  // balances it) or a conflict, which the residual check in Solve reports.
  //
  // The two-phase order is the point. Seeding every A at once and letting
  // the LIFO decide meant a relation whose A is a FREE mate could win the
  // race for a mate the driver was about to set — and then write it from a
  // stale value. A cam lifting a valve through a rocker is exactly that
  // shape: the cam relation drives the valve's slide, and the rack that
  // makes the rocker follow has the rocker as its A. Reached from the
  // rocker instead, the rack drove the VALVE from last sample's rocker
  // angle, and the pair ran away to 60 mm of lift on an 8 mm cam.
  //
  // And one root at a time, not every remaining A at once: seeding them all
  // settled every mate that is both a follower and a driver before anything
  // reached it, so a chain was applied at its two ends only — a plain
  // solve_assembly left a watch's keyless works off their phases until
  // something drove the stem.
  struct Node {
    uint64_t mate;
    MateDof dof;
    bool operator==(const Node& o) const {
      return mate == o.mate && dof == o.dof;
    }
  };
  // Whether each freewheel may DRAG its input when its output is pushed
  // back against it. A lone clutch does: spin a bicycle's wheel backwards
  // and the pedals turn with it. One inside a CYCLE must not — a reverser's
  // two paths reach the same output, so dragging one input turns the other
  // wheel forward, which drives the output further the way that caused the
  // drag, and the pose runs away (a watch's ratchet reached 5e10 degrees).
  // The real mechanism locks there, and a lock is a constraint, not a drive:
  // the relation says its output stands behind its line instead.
  // Every (mate, DOF) a relation touches: its two sides, or each of its
  // terms for a relation over several mates.
  const auto membersOf = [&](size_t k) {
    std::vector<Node> out;
    if (rels[k].HasTerms()) {
      for (const RelationTerm& t : rels[k].terms)
        out.push_back({t.mate, t.dof});
    } else {
      out.push_back({rels[k].mateA, rels[k].dofA});
      out.push_back({rels[k].mateB, rels[k].dofB});
    }
    return out;
  };
  std::vector<bool> mayDrag(rels.size(), false);
  for (size_t i = 0; i < rels.size(); i++) {
    if (!usable[i] || rels[i].kind != RelationKind::Freewheel)
      continue;
    // Is this freewheel's INPUT reachable from its output through the other
    // relations? Walk them as undirected edges: which end a relation is
    // driven from depends on what moves, and the question is only whether
    // the two are tied together some other way.
    const Node from{rels[i].mateB, rels[i].dofB};
    const Node goal{rels[i].mateA, rels[i].dofA};
    std::vector<Node> seen{from}, stack{from};
    bool cycle = false;
    while (!stack.empty() && !cycle) {
      const Node at = stack.back();
      stack.pop_back();
      for (size_t k = 0; k < rels.size() && !cycle; k++) {
        if (k == i || !usable[k])
          continue;
        // A relation over several mates ties every member to every other.
        const std::vector<Node> members = membersOf(k);
        if (std::find(members.begin(), members.end(), at) == members.end())
          continue;
        for (const Node& there : members) {
          if (there == at ||
              std::find(seen.begin(), seen.end(), there) != seen.end())
            continue;
          if (there == goal) {
            cycle = true;
            break;
          }
          seen.push_back(there);
          stack.push_back(there);
        }
      }
    }
    mayDrag[i] = !cycle;
  }
  std::vector<Node> frontier;
  std::vector<Node> settled;
  const auto isSettled = [&](const Node& n) {
    return std::find(settled.begin(), settled.end(), n) != settled.end();
  };
  const auto settle = [&](const Node& n) {
    settled.push_back(n);
    frontier.push_back(n);
  };
  if (drivenMateId != 0) {
    if (const Mate* m = asmDoc.FindMateById(drivenMateId)) {
      for (MateDof d : {MateDof::Rotation, MateDof::Offset}) {
        if (!MateHasDof(m->type, d))
          continue;
        // A screw couples the driven mate's own two DOFs; its B follows
        // its A (which the caller changed cannot be known), so it is not
        // pre-settled.
        const bool screwB =
            std::any_of(rels.begin(), rels.end(), [&](const Relation& r) {
              return r.mateA == drivenMateId && r.mateB == drivenMateId &&
                     r.dofB == d && r.dofA != d;
            });
        if (!screwB)
          settle({drivenMateId, d});
      }
    }
  }
  // A DOF the active configuration HOLDS (v101) is set, not solved: it
  // stands like the driver's, and its relations drive FROM it.
  if (const Configuration* cfg = asmDoc.ActiveConfig())
    for (const MateOverride& o : cfg->mateOverrides) {
      const Mate* m = asmDoc.FindMateById(o.mate);
      if (m == nullptr)
        continue;
      if (o.rotation && MateHasDof(m->type, MateDof::Rotation) &&
          !isSettled({o.mate, MateDof::Rotation}))
        settle({o.mate, MateDof::Rotation});
      if (o.offset && MateHasDof(m->type, MateDof::Offset) &&
          !isSettled({o.mate, MateDof::Offset}))
        settle({o.mate, MateDof::Offset});
    }
  // The value a node stands at: the configuration's hold when there is one.
  const auto valueOf = [&](const Mate& m, MateDof d) -> double {
    if (const MateOverride* o = asmDoc.MateOverrideFor(m.id)) {
      if (d == MateDof::Rotation && o->rotation)
        return *o->rotation;
      if (d == MateDof::Offset && o->offset)
        return *o->offset;
    }
    return MateDofValue(m, d);
  };
  // A member of a relation over several mates, as its error and note name it.
  const auto memberWord = [&](const Relation& r, size_t k) {
    const RelationTerm& t = r.terms[k];
    const Mate* m = asmDoc.FindMateById(t.mate);
    return RelationTermRole(r, k) + " (" +
           (m != nullptr && !m->name.empty()
                ? m->name
                : "mate " + std::to_string(t.mate)) +
           ")";
  };
  const auto nextRoot = [&]() {
    // A relation over several mates that the walk REACHED but could not
    // finish — a differential with one input driven — is completed first,
    // before any new component starts: its first unsettled member, in term
    // order, is held where it stands, and the relation says so. A new
    // component started first could claim that member from the far side
    // (a gear on the carrier taking the carrier) and turn the held one.
    for (size_t i = 0; i < rels.size(); i++) {
      if (!usable[i] || !rels[i].HasTerms())
        continue;
      const std::vector<Node> members = membersOf(i);
      size_t open = 0, first = members.size();
      for (size_t k = 0; k < members.size(); k++)
        if (!isSettled(members[k])) {
          open++;
          first = std::min(first, k);
        }
      if (open >= 2 && open < members.size()) {
        settle(members[first]);
        Relation& r = rels[i];
        r.held += (r.held.empty() ? "" : "; ") + memberWord(r, first) +
                  " held where it stands: nothing drives it";
        r.heldTerms.push_back(first);
        return true;
      }
    }
    // A component nothing has reached: its first relation's A is the root
    // (a relation over several mates: its first member).
    for (size_t i = 0; i < rels.size(); i++) {
      if (!usable[i])
        continue;
      const std::vector<Node> members = membersOf(i);
      if (std::none_of(members.begin(), members.end(), isSettled)) {
        settle(members.front());
        return true;
      }
    }
    // An A that was reached only as far as its B (a cam, which is never
    // driven backwards): it stands where it is.
    for (size_t i = 0; i < rels.size(); i++) {
      const Node a{rels[i].mateA, rels[i].dofA};
      if (usable[i] && !rels[i].HasTerms() && !isSettled(a)) {
        settle(a);
        return true;
      }
    }
    return false;
  };
  while (!frontier.empty() || nextRoot()) {
    const Node from = frontier.back();
    frontier.pop_back();
    for (size_t i = 0; i < rels.size(); i++) {
      if (!usable[i])
        continue;
      Relation& r = rels[i];
      if (r.HasTerms()) {
        // A HYPEREDGE: one equation over all its members. It settles the
        // last member once every other is settled, and otherwise waits —
        // for the walk to reach the rest, or for nextRoot to hold one.
        const std::vector<Node> members = membersOf(i);
        if (std::find(members.begin(), members.end(), from) == members.end())
          continue;
        size_t open = members.size(), count = 0;
        for (size_t k = 0; k < members.size(); k++)
          if (!isSettled(members[k])) {
            open = k;
            count++;
          }
        if (count != 1)
          continue;
        double sum = r.phase;
        for (size_t k = 0; k < members.size(); k++)
          if (k != open)
            sum -=
                r.terms[k].coefficient *
                valueOf(*asmDoc.FindMateById(members[k].mate), members[k].dof);
        SetMateDof(*asmDoc.FindMateById(members[open].mate), members[open].dof,
                   sum / r.terms[open].coefficient);
        settle(members[open]);
        wrote++;
        continue;
      }
      const Node an{r.mateA, r.dofA};
      const Node bn{r.mateB, r.dofB};
      Mate* a = asmDoc.FindMateById(r.mateA);
      Mate* b = asmDoc.FindMateById(r.mateB);
      if (an == from && !isSettled(bn)) {
        if (r.kind == RelationKind::Freewheel) {
          // The input moved. Where its drive line is at or ahead of b (in
          // the drive direction) it drives b. Behind, the clutch SLIPS: b
          // is left where it is and, just as important, left UNSETTLED —
          // another relation may be the one that moves it, which is how a
          // reverser works (docs/assemblies/relations.md#freewheel). The
          // history is reconciled once, after the walk.
          const double line = RelationValue(r, valueOf(*a, r.dofA));
          const double now = valueOf(*b, r.dofB);
          if ((line - now) * r.drives < -FreewheelSlack(now))
            continue; // slipping
          SetMateDof(*b, r.dofB, line);
        } else {
          SetMateDof(*b, r.dofB, RelationValue(r, valueOf(*a, r.dofA)));
        }
        settle(bn);
        wrote++;
      } else if (bn == from && !isSettled(an) &&
                 r.kind == RelationKind::Freewheel && r.ratio != 0.0) {
        // Something else moved the OUTPUT. Forward in the drive direction
        // it OVERRUNS: the input is not dragged, and is left unsettled for
        // whatever else may drive it. Backward, the clutch engages and
        // drags the input back with it.
        const double line = RelationValue(r, valueOf(*a, r.dofA));
        const double now = valueOf(*b, r.dofB);
        if ((now - line) * r.drives > FreewheelSlack(now) || !mayDrag[i])
          continue; // overrunning, or holding rather than dragging
        SetMateDof(*a, r.dofA, (now - r.phase - r.lead) / r.ratio);
        settle(an);
        wrote++;
      } else if (bn == from && !isSettled(an) && r.kind != RelationKind::Cam &&
                 r.kind != RelationKind::Intermittent &&
                 r.kind != RelationKind::Contact && r.ratio != 0.0) {
        // A cam is deliberately absent here: many angles give one lift, so
        // driving it backwards would have to pick a branch, and a plausible
        // branch is the substitution this project refuses. So is an
        // intermittent relation: a locked output says nothing about where
        // its input stands, and turning it is what a lock refuses — the
        // residual check reports the disagreement instead.
        SetMateDof(*a, r.dofA, (valueOf(*b, r.dofB) - r.phase) / r.ratio);
        settle(an);
        wrote++;
      }
    }
  }
  // The history, once the walk has finished moving things. Where the output
  // ended up, less where this freewheel's drive line puts it, is how far it
  // has slipped, and `lead` absorbs it — so the relation holds exactly and
  // the next pass finds nothing to do. Reconciling HERE rather than in the
  // walk is what lets two freewheels drive one output (a reverser): the
  // slipping one is reached with both its ends already settled by the path
  // that is driving, and it simply takes up the difference.
  //
  // The other way round — the output BEHIND the line, where this freewheel
  // would have been driving it — is two drivers that disagree, and is said
  // on the relation rather than absorbed.
  for (size_t i = 0; i < rels.size(); i++) {
    Relation& r = rels[i];
    if (!usable[i] ||
        (r.kind != RelationKind::Freewheel && r.kind != RelationKind::Click))
      continue;
    const Mate* a = asmDoc.FindMateById(r.mateA);
    Mate* b = asmDoc.FindMateById(r.mateB);
    if (a == nullptr || b == nullptr)
      continue;
    if (r.kind == RelationKind::Click) {
      // A pawl. Moved the way it lets go, the mark follows; moved against
      // it, the click HOLDS — the DOF goes back to the mark, and whatever
      // drove it is left disagreeing, which its own residual reports.
      const double v = valueOf(*b, r.dofB);
      if ((v - r.lead) * r.drives >= -FreewheelSlack(v)) {
        r.lead = v;
      } else {
        SetMateDof(*b, r.dofB, r.lead);
        wrote++;
      }
      continue;
    }
    const double now = valueOf(*b, r.dofB);
    // Where the output stands relative to the input, which is all `lead`
    // is — SET, not accumulated: adding the gap to the drive line would
    // count the part of a tooth already ridden over twice, and the lead
    // would drift a fraction of a pitch every pass.
    const double raw = now - (r.ratio * valueOf(*a, r.dofA) + r.phase);
    const double gap = raw - FreewheelNotch(r);
    if (gap * r.drives > FreewheelSlack(now)) {
      r.lead = raw;
      wrote++;
    } else if (gap * r.drives < -FreewheelSlack(now)) {
      r.error = "not satisfied: this freewheel's output stands behind the "
                "drive line it would be driven along — another relation "
                "drives it too, and they disagree";
    }
    // Riding over the teeth lifts the DOF the relation names, by where it
    // stands between notches. Applied HERE, after the walk, so it adds to
    // whatever drove that DOF — the yoke's rack, for a watch's clutch — and
    // nothing propagates from it. What the last pass added is taken back
    // where the walk did not recompute the base itself.
    if (r.teeth > 0 && r.rideMate != 0) {
      if (Mate* rm = asmDoc.FindMateById(r.rideMate)) {
        // OFF its notch the clutch stands CLEAR, and seated it stands
        // where it was drawn: dogs are solid, so one part way over another
        // would be inside it. (A real Breguet tooth ramps and its clutch
        // rides part way out; drawing that takes a swept helical tooth,
        // which generators/watch/README.md#approximate-and-not-modelled leaves
        // out.)
        const double lift =
            std::abs(r.lead - FreewheelNotch(r)) > FreewheelSlack(r.lead)
                ? r.rideAmount
                : 0.0;
        const bool fresh = isSettled({r.rideMate, r.rideDof});
        const double base =
            MateDofValue(*rm, r.rideDof) - (fresh ? 0.0 : r.rideApplied);
        if (std::abs(base + lift - MateDofValue(*rm, r.rideDof)) > 1e-12)
          wrote++;
        SetMateDof(*rm, r.rideDof, base + lift);
        r.rideApplied = lift;
      }
    }
  }
  return wrote;
}

static SolveReport SolveCore(AssemblyDocument& asmDoc,
                             const PartResolver& partById,
                             const AssemblyResolver& asmById,
                             const Driven& driven, bool diagnose) {
  SolveReport report;
  // The active configuration's held DOFs (v101) stand in for the mates' own
  // values for the whole solve — placement, relations, the loop pass — and
  // the authored values go back at the end. An overlay, never an edit: the
  // configuration can be left, and the mate is where it was.
  struct Held {
    size_t index;
    MateDof dof;
    double authored;
  };
  std::vector<Held> held;
  if (const Configuration* cfg = asmDoc.ActiveConfig())
    for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
      Mate& m = asmDoc.GetMates()[i];
      const MateOverride* o = cfg->OverrideForMate(m.id);
      if (o == nullptr)
        continue;
      if (o->rotation && MateHasDof(m.type, MateDof::Rotation)) {
        held.push_back({i, MateDof::Rotation, m.rotation});
        m.rotation = *o->rotation;
      }
      if (o->offset) { // a slide, or a revolute's axial seat
        held.push_back({i, MateDof::Offset, m.offset});
        m.offset = *o->offset;
      }
    }

  // Relations hold before anything is placed: in a mate tree this IS their
  // enforcement; in a loop they are rows below as well. A solve that KNOWS
  // what is being driven says so, so the driver's own chain settles before
  // anything else claims a mate on it.
  (void)PropagateRelations(asmDoc, driven.mate, partById);
  for (const auto& r : asmDoc.GetRelations())
    if (!r.error.empty())
      report.erroredMates++;

  // Pre-derive both connector frames for every mate; a mate whose connectors
  // don't resolve (dangling ref / missing part) is stamped errored and sits
  // out the propagation — its occurrences may still be placed by other mates
  // or keep their stored transforms.
  struct MateFrames {
    bool usable = false;
    glm::dmat4 fa{1.0};
    glm::dmat4 fb{1.0};
  };
  std::vector<MateFrames> frames(asmDoc.GetMates().size());

  for (size_t i = 0; i < asmDoc.GetMates().size(); i++) {
    Mate& mate = asmDoc.GetMates()[i];
    mate.error.clear();
    mate.residualMm = 0.0;
    // Out of the active configuration (v101): it sits out like an unusable
    // mate, without an error — so does one touching a hidden occurrence.
    if (asmDoc.MateSuppressed(mate.id))
      continue;
    std::string whyA, whyB;
    ConnectorPrints printsA, printsB;
    const auto fa =
        ConnectorLocalFrame(mate.a, asmDoc, partById, &whyA, asmById, false,
                            mate.a.anchor.takePrints ? &printsA : nullptr);
    const auto fb =
        ConnectorLocalFrame(mate.b, asmDoc, partById, &whyB, asmById, false,
                            mate.b.anchor.takePrints ? &printsB : nullptr);
    // A connector made now takes the prints of the imported elements it
    // names, as they are when it is first solved (v201).
    if (fa)
      TakePrints(mate.a, printsA);
    if (fb)
      TakePrints(mate.b, printsB);
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
  std::unordered_map<uint64_t, glm::dmat4> world;
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
      if (!r.error.empty() || asmDoc.RelationSuppressed(r.id))
        continue;
      // A freewheel is an inequality — it may slip — and the loop solve
      // holds only equations. It is left out, and reported below if the loop
      // moves one of its mates (docs/assemblies/relations.md#freewheel).
      if (r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click)
        continue;
      if (r.HasTerms()) {
        // One row over all its members (v171).
        LoopRelation lr;
        lr.phase = r.phase;
        bool all = true;
        for (const RelationTerm& t : r.terms) {
          const auto it = indexOfMate.find(t.mate);
          if (it == indexOfMate.end()) {
            all = false;
            break;
          }
          lr.terms.push_back({it->second, t.dof, t.coefficient});
        }
        if (all)
          loop.relations.push_back(std::move(lr));
        continue;
      }
      const auto ia = indexOfMate.find(r.mateA);
      const auto ib = indexOfMate.find(r.mateB);
      if (ia == indexOfMate.end() || ib == indexOfMate.end())
        continue;
      loop.relations.push_back(
          {ia->second, ib->second, r.dofA, r.dofB, r.ratio, r.phase, r.lift});
      if (r.kind == RelationKind::Intermittent ||
          r.kind == RelationKind::Contact || r.profileMotion)
        loop.relations.back().value = [r](double a) {
          return RelationValue(r, a);
        };
    }
  }
  // The driven DOF is PINNED while the loop closes: without it the solver
  // treats it as one more unknown and moves it, so a crank set to 90 degrees
  // solved to 67 (generators/README.md#single-cylinder-and-air-cooled-engines).
  if (driven.mate != 0) {
    std::unordered_map<uint64_t, size_t> indexOfMate;
    for (size_t i = 0; i < asmDoc.GetMates().size(); i++)
      indexOfMate[asmDoc.GetMates()[i].id] = i;
    const auto it = indexOfMate.find(driven.mate);
    if (it != indexOfMate.end() &&
        MateHasDof(asmDoc.GetMates()[it->second].type, driven.dof))
      loop.pinned.push_back(
          {it->second, driven.dof,
           MateDofValue(asmDoc.GetMates()[it->second], driven.dof)});
  }
  // So is every member a relation over several mates HELD in propagation:
  // the loop's least squares would otherwise share the turn among them all.
  {
    std::unordered_map<uint64_t, size_t> indexOfMate;
    for (size_t i = 0; i < asmDoc.GetMates().size(); i++)
      indexOfMate[asmDoc.GetMates()[i].id] = i;
    for (const Relation& r : asmDoc.GetRelations())
      for (size_t k : r.heldTerms) {
        if (k >= r.terms.size())
          continue;
        const auto it = indexOfMate.find(r.terms[k].mate);
        if (it != indexOfMate.end())
          loop.pinned.push_back(
              {it->second, r.terms[k].dof,
               MateDofValue(asmDoc.GetMates()[it->second], r.terms[k].dof)});
      }
  }
  // So is every DOF the configuration holds.
  for (const Held& h : held)
    if (MateHasDof(asmDoc.GetMates()[h.index].type, h.dof))
      loop.pinned.push_back(
          {h.index, h.dof, MateDofValue(asmDoc.GetMates()[h.index], h.dof)});

  std::vector<bool> classified(asmDoc.GetMates().size(), false);
  const auto pin = [&](uint64_t occId, const glm::dmat4& at) {
    world[occId] = at;
    loop.anchor.emplace_back(occId, at);
  };

  // Anchors: every `fixed` occurrence starts at its stored transform — unless
  // the active configuration hides it.
  for (const Occurrence& o : asmDoc.GetOccurrences())
    if (o.fixed && !asmDoc.OccurrenceSuppressed(o.id))
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
        const glm::dmat4 adjust = MateAdjust(mate);

        if (aP && !bP) {
          // b's connector frame = a's connector world frame · adjust.
          const glm::dmat4 wa = world[aId] * frames[i].fa;
          world[bId] = wa * adjust * glm::inverse(frames[i].fb);
          loop.tree.push_back({i, aId, bId, false});
          classified[i] = true;
          progress = true;
        } else if (bP && !aP) {
          const glm::dmat4 wb = world[bId] * frames[i].fb;
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
  loop.diagnose = diagnose;
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

      // A freewheel's mates as propagation left them: if the loop moves one,
      // the freewheel was inside the loop, which it cannot be (it was left
      // out of the rows above) — said on the relation, never held silently.
      struct FreewheelAt {
        Relation* r;
        double a, b;
      };
      std::vector<FreewheelAt> freewheels;
      for (Relation& r : asmDoc.GetRelations())
        if (r.kind == RelationKind::Freewheel && r.error.empty() &&
            !asmDoc.RelationSuppressed(r.id))
          if (const Mate* ma = asmDoc.FindMateById(r.mateA))
            if (const Mate* mb = asmDoc.FindMateById(r.mateB))
              freewheels.push_back(
                  {&r, MateDofValue(*ma, r.dofA), MateDofValue(*mb, r.dofB)});
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
      for (const FreewheelAt& f : freewheels) {
        const Mate* ma = asmDoc.FindMateById(f.r->mateA);
        const Mate* mb = asmDoc.FindMateById(f.r->mateB);
        if (std::abs(MateDofValue(*ma, f.r->dofA) - f.a) >
                FreewheelSlack(f.a) ||
            std::abs(MateDofValue(*mb, f.r->dofB) - f.b) >
                FreewheelSlack(f.b)) {
          f.r->error = "a freewheel cannot be inside a mate loop: it may "
                       "slip, and the loop solve holds only equations — it "
                       "was left out of the loop";
          report.erroredMates++;
        }
      }

      // Only a loop that could NOT close is an error, and it says by how
      // much — its OWN residual, not the worst in the document. The mate
      // that carries the error is only the one the tree walk happened to
      // close the loop with, which depends on anchors and creation order,
      // so when the diagnosis found the connector that is actually wrong
      // the message names it.
      //
      // The diagnosis runs HERE, after the solve, and not when the mate is
      // created: before the first solve, the angle between two connectors
      // is wherever the parts happened to be dropped, so a check at
      // add_mate would fire on nearly every correct mate. What does carry
      // information is the failed loop itself — which single tilt would
      // close it
      // (docs/assemblies/solver.md#naming-the-mate-that-keeps-a-loop-open).
      report.diagnoses = closed.diagnoses;
      for (size_t k = 0; k < closed.conflicting.size(); k++) {
        const uint64_t id = closed.conflicting[k];
        Mate* m = asmDoc.FindMateById(id);
        if (m == nullptr)
          continue;
        m->residualMm = k < closed.conflictingMm.size()
                            ? closed.conflictingMm[k]
                            : closed.residualMm;
        m->error =
            "loop does not close (off by " + FormatMm(m->residualMm) + " mm)";
        for (const LoopDiagnosis& d : closed.diagnoses)
          if (std::find(d.conflicting.begin(), d.conflicting.end(), id) !=
              d.conflicting.end())
            m->error += LoopHint(d, asmDoc);
        report.erroredMates++;
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
    glm::dvec3 pos;
    glm::dquat rot;
    Decompose(it->second, pos, rot);
    if (pos != o.position || rot != o.orientation)
      report.placed++;
    o.position = pos;
    o.orientation = rot;
  }

  // Riders last: their parents have just been placed, and an attachment is a
  // composition off the result rather than an input to it.
  report.placed +=
      ApplyAttachments(asmDoc, partById, asmById, &report.erroredAttachments);

  // Every relation the solve could apply must now HOLD. One that does not is
  // a degree of freedom driven twice by relations that disagree — in a tree
  // the second is never applied, in a loop the pass settles on a compromise
  // — and either way the document claims a coupling the model does not
  // have. Reported on the relation, never left silent.
  // What a clutch or a pawl riding over its teeth has LIFTED, per DOF: the
  // relation that drives that DOF gave it its base, and the ride was added
  // on top, so the base is what this check must compare against. Without
  // this, a watch's yoke reports a fault every time its clutch rides up a
  // dog (generators/watch/README.md#keyless-works-and-winding).
  std::vector<std::pair<std::pair<uint64_t, MateDof>, double>> ridden;
  for (const Relation& r : asmDoc.GetRelations())
    if (r.rideMate != 0 && r.rideApplied != 0.0 &&
        !asmDoc.RelationSuppressed(r.id))
      ridden.push_back({{r.rideMate, r.rideDof}, r.rideApplied});
  const auto rideOn = [&](uint64_t mate, MateDof dof) {
    double sum = 0.0;
    for (const auto& [key, amount] : ridden)
      if (key.first == mate && key.second == dof)
        sum += amount;
    return sum;
  };
  for (Relation& r : asmDoc.GetRelations()) {
    if (!r.error.empty() || asmDoc.RelationSuppressed(r.id))
      continue;
    // A freewheel and a click are inequalities — one may slip, the other
    // holds — and their own pass says when they disagree. An equality test
    // here would read a clutch riding over a tooth as a fault.
    if (r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click)
      continue;
    uint64_t mateB = r.mateB;
    MateDof dofB = r.dofB;
    double want = 0.0, got = 0.0;
    if (r.HasTerms()) {
      // Over several mates: what the LAST member must be for the others
      // as they stand (an epicyclic's carrier, a planet's carrier). Any
      // member off the equation shows here, whichever one it is.
      if (r.terms.empty())
        continue;
      bool all = true;
      double sum = r.phase;
      for (size_t k = 0; k + 1 < r.terms.size(); k++) {
        const Mate* m = asmDoc.FindMateById(r.terms[k].mate);
        if (m == nullptr) {
          all = false;
          break;
        }
        sum -= r.terms[k].coefficient * MateDofValue(*m, r.terms[k].dof);
      }
      const RelationTerm& last = r.terms.back();
      const Mate* m = asmDoc.FindMateById(last.mate);
      if (!all || m == nullptr)
        continue;
      mateB = last.mate;
      dofB = last.dof;
      want = sum / last.coefficient + rideOn(mateB, dofB);
      got = MateDofValue(*m, dofB);
    } else {
      const Mate* a = asmDoc.FindMateById(r.mateA);
      const Mate* b = asmDoc.FindMateById(r.mateB);
      if (a == nullptr || b == nullptr)
        continue;
      want =
          RelationValue(r, MateDofValue(*a, r.dofA)) + rideOn(r.mateB, r.dofB);
      got = MateDofValue(*b, r.dofB);
    }
    // A loop closes to a tolerance, not exactly: allow it at the magnitude.
    if (std::abs(want - got) > 1e-4 * std::max(1.0, std::abs(want))) {
      const bool rot = dofB == MateDof::Rotation;
      const double k = rot ? angles::kDegPerRad : 1.0;
      // Name the other driver only when there is one. Said of every miss,
      // it sent the woodcutter's author looking for a second relation when
      // a loop's solve had moved the value (generators/woodcutter).
      bool another = false;
      for (const Relation& o : asmDoc.GetRelations()) {
        if (o.id == r.id || asmDoc.RelationSuppressed(o.id))
          continue;
        if (o.HasTerms()) {
          for (const RelationTerm& t : o.terms)
            another = another || (t.mate == mateB && t.dof == dofB);
        } else {
          another = another || (o.mateB == mateB && o.dofB == dofB);
        }
      }
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "not satisfied: mate %llu's %s is %.4f %s where this "
                    "relation wants %.4f — %s",
                    static_cast<unsigned long long>(mateB),
                    rot ? "rotation" : "offset", got * k, rot ? "deg" : "mm",
                    want * k,
                    another ? "another relation drives it too, and they "
                              "disagree"
                            : "the mates round it hold it elsewhere (a "
                              "loop that closes only there, or a held "
                              "value)");
      r.error = buf;
      report.erroredMates++;
    }
  }
  // The held DOFs go back to their authored values (see the top).
  for (const Held& h : held)
    SetMateDof(asmDoc.GetMates()[h.index], h.dof, h.authored);
  return report;
}

namespace {
std::atomic<uint64_t> g_solveCount{0};
} // namespace

uint64_t SolveCount() { return g_solveCount.load(); }

SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById,
                  const AssemblyResolver& asmById, const Driven& driven,
                  bool diagnose) {
  g_solveCount.fetch_add(1, std::memory_order_relaxed);
  // Flexible sub-assemblies (v129) are solved by WRAPPING: the solve runs on
  // a copy with every flexible placement's inside added, and the answer is
  // written back. Nothing flexible, nothing to wrap.
  bool flexible = false;
  for (const Occurrence& o : asmDoc.GetOccurrences())
    flexible = flexible || o.flexible;
  if (!flexible || !asmById) {
    Driven own = driven;
    if (!driven.path.empty())
      own.mate = 0; // a path with nothing to expand names no mate here
    return SolveCore(asmDoc, partById, asmById, own, diagnose);
  }
  FlexibleExpansion ex = ExpandFlexible(asmDoc, partById, asmById);
  Driven inner = driven;
  inner.mate = ex.MateId(driven.path, driven.mate);
  inner.path.clear();
  SolveReport report = SolveCore(ex.doc, partById, asmById, inner, diagnose);
  CollapseFlexible(ex, asmDoc, &report);
  return report;
}

glm::dmat3 InertiaAbout(const std::vector<PointInertia>& parts,
                        const glm::dvec3& centre) {
  glm::dmat3 out(0.0);
  for (const PointInertia& p : parts) {
    const glm::dvec3 d = p.centre - centre;
    out += p.inertiaGmm2 + p.massG * (glm::dot(d, d) * glm::dmat3(1.0) -
                                      glm::outerProduct(d, d));
  }
  return out;
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
    const auto mass =
        Measure::MassOfBodies(inst.part->GetBodies(), &err, inst.material);
    if (!mass) {
      out.skipped.push_back(inst.path + " (" + err + ")");
      out.massKnown = false;
      continue;
    }
    const glm::dmat4& xf = inst.world;
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
    if (om.massKnown) {
      // Each body's own, by its density (g/mm^3 = kg/m^3 * 1e-6), about the
      // part's centre, then turned with the placement.
      std::vector<PointInertia> bodies;
      for (const Measure::BodyMass& bm : mass->bodies) {
        glm::dmat3 own(0.0);
        for (int r = 0; r < 3; r++)
          for (int c = 0; c < 3; c++)
            own[c][r] = bm.inertia[r][c] * bm.density * 1e-6;
        bodies.push_back({bm.massG, bm.centerOfMass, own});
      }
      const glm::dmat3 rot(xf);
      om.inertiaGmm2 =
          rot * InertiaAbout(bodies, mass->centerOfMass) * glm::transpose(rot);
    }
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
