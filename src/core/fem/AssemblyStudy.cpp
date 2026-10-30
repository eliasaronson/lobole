// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A study on an assembly (docs/simulation/README.md#assembly-studies): one
// occurrence meshed, its rigid neighbours clamped where they touch it. The
// technique is P0's (tests/test_fem.cpp, "a mesh spans two solids only when
// they SHARE the face"): a fuzzy general fuse imprints the contact patch as
// a face of the meshed shape, and the existing mesher and solver take it from
// there. Everything here is computed into the plan and discarded — no part
// document is ever edited by a simulation.

#include "cad/CADMaterials.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <BOPAlgo_Builder.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem {
namespace {

// Parts are PLACED at float32, so nominally coincident faces sit microns
// apart; 1e-3 imprints across a 3 um gap (P0). Below this a strict boolean
// finds nothing and the assembly meshes as loose pieces, which LOOKS like a
// working study.
constexpr double kImprintFuzzMm = 1e-3;
// A rigid support that OVERLAPS the meshed part by more than this is not a
// support but a press fit, and fusing it makes slivers.
constexpr double kOverlapRefuseMm3 = 1e-6;

struct SolidCentroid {
  glm::dvec3 at{0.0};
  double volume = 0.0;
};

SolidCentroid CentroidOf(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  const gp_Pnt c = props.CentreOfMass();
  return {glm::dvec3(c.X(), c.Y(), c.Z()), props.Mass()};
}

// The solid of `result` that came from `input`: after a general fuse of
// non-overlapping arguments each result solid IS one input, re-faced. The
// builder's own Modified() answers for faces and edges; for solids the
// centroid is the unambiguous key, since the pre-flight has already refused
// any two that overlap.
TopoDS_Shape ResultSolidOf(const TopoDS_Shape& input,
                           const TopoDS_Shape& result) {
  const SolidCentroid want = CentroidOf(input);
  const double scale = std::cbrt(std::max(want.volume, 1e-9));
  TopoDS_Shape best;
  double bestDist = 1e300;
  for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) {
    const SolidCentroid have = CentroidOf(ex.Current());
    const double d = glm::length(have.at - want.at) +
                     std::abs(have.volume - want.volume) / (scale * scale);
    if (d < bestDist) {
      bestDist = d;
      best = ex.Current();
    }
  }
  return bestDist < 1e-2 * scale ? best : TopoDS_Shape();
}

std::vector<TopoDS_Shape> FacesOf(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
    out.push_back(ex.Current());
  return out;
}

int IndexOfFace(const std::vector<TopoDS_Shape>& faces,
                const TopoDS_Shape& face) {
  for (size_t i = 0; i < faces.size(); ++i)
    if (faces[i].IsSame(face))
      return static_cast<int>(i);
  return -1;
}

// The faces of the imprinted shape that an ORIGINAL face became: itself,
// when the imprint left it alone, else the pieces it was split into.
std::vector<int> ImprintedFaces(BOPAlgo_Builder& gf,
                                const TopoDS_Shape& originalFace,
                                const std::vector<TopoDS_Shape>& imprinted) {
  std::vector<int> out;
  const TopTools_ListOfShape& mod = gf.Modified(originalFace);
  if (mod.IsEmpty()) {
    const int i = IndexOfFace(imprinted, originalFace);
    if (i >= 0)
      out.push_back(i);
    return out;
  }
  for (TopTools_ListOfShape::Iterator it(mod); it.More(); it.Next()) {
    const int i = IndexOfFace(imprinted, it.Value());
    if (i >= 0)
      out.push_back(i);
  }
  return out;
}

double AreaOf(const TopoDS_Shape& face) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  return props.Mass();
}

std::string Quote(const std::string& s) { return "«" + s + "»"; }

// Contact (docs/simulation/README.md#contact). Two faces are a contact's
// candidates when they come within this fraction of the smaller body's
// bounding-box diagonal of each other and face each other there (their
// outward normals more than kContactFacing opposed): a sphere on a block
// touches at a point and shares no face, which an imprint would need, and a
// block's side meets a plate it stands on along an edge without facing it.
constexpr double kContactClearanceFraction = 0.05;
constexpr double kContactFacing = 0.5; // cos 60 deg
// A rigid partner is meshed this many times coarser than the meshed body,
// its contact faces at the body's size.
constexpr double kRigidCoarsen = 2.0;

double DiagonalOf(const TopoDS_Shape& s) {
  Bnd_Box b;
  BRepBndLib::Add(s, b);
  if (b.IsVoid())
    return 0.0;
  return std::sqrt(b.SquareExtent());
}

// The outward normal of `face` at the point of it nearest `p`; zero when
// the surface has none there (a pole), looked for a little way off it.
glm::dvec3 OutwardNormalNear(const TopoDS_Shape& face, const gp_Pnt& p) {
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(TopoDS::Face(face));
  if (surf.IsNull())
    return glm::dvec3(0.0);
  GeomAPI_ProjectPointOnSurf proj(p, surf);
  if (proj.NbPoints() == 0)
    return glm::dvec3(0.0);
  double u = 0, v = 0;
  proj.LowerDistanceParameters(u, v);
  double u0, u1, v0, v1;
  surf->Bounds(u0, u1, v0, v1);
  const double cu = std::isfinite(u0 + u1) ? 0.5 * (u0 + u1) : u;
  const double cv = std::isfinite(v0 + v1) ? 0.5 * (v0 + v1) : v;
  for (double t : {0.0, 1e-4, 1e-3, 1e-2}) {
    GeomLProp_SLProps props(surf, u + t * (cu - u), v + t * (cv - v), 1, 1e-9);
    if (!props.IsNormalDefined())
      continue;
    gp_Dir n = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED)
      n.Reverse();
    return glm::dvec3(n.X(), n.Y(), n.Z());
  }
  return glm::dvec3(0.0);
}

// The faces of `a` and of `b` that are a contact's candidates.
void FacingFaces(const std::vector<TopoDS_Shape>& a,
                 const std::vector<TopoDS_Shape>& b, double clearance,
                 std::set<int>& onA, std::set<int>& onB) {
  std::vector<Bnd_Box> boxA(a.size()), boxB(b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    BRepBndLib::Add(a[i], boxA[i]);
    boxA[i].Enlarge(clearance);
  }
  for (size_t j = 0; j < b.size(); ++j)
    BRepBndLib::Add(b[j], boxB[j]);
  for (size_t i = 0; i < a.size(); ++i)
    for (size_t j = 0; j < b.size(); ++j) {
      if (boxA[i].IsOut(boxB[j]))
        continue;
      BRepExtrema_DistShapeShape d(a[i], b[j]);
      if (!d.IsDone() || d.NbSolution() == 0 || d.Value() > clearance)
        continue;
      for (int k = 1; k <= d.NbSolution(); ++k) {
        const glm::dvec3 na = OutwardNormalNear(a[i], d.PointOnShape1(k));
        const glm::dvec3 nb = OutwardNormalNear(b[j], d.PointOnShape2(k));
        if (glm::dot(na, nb) < -kContactFacing) {
          onA.insert(static_cast<int>(i));
          onB.insert(static_cast<int>(j));
          break;
        }
      }
    }
}

// A participant with its placed bodies: a rigid support, or a bonded member.
struct RigidSet {
  const Participant* p = nullptr;
  const Occurrence* occ = nullptr;
  std::vector<Body> bodies;
};

// One member of a bonded group: a placed body of the meshed or a bonded
// occurrence.
struct GroupMember {
  const Participant* p = nullptr;
  const Occurrence* occ = nullptr;
  int bodyIndex = 0; // within its occurrence's placed bodies
  const Body* body = nullptr;
  TopoDS_Shape original;  // a copy
  TopoDS_Shape imprinted; // after the fuse
  std::vector<TopoDS_Shape> originalFaces;
  Material mat;
};

struct RigidBodyRef {
  size_t rigidSet = 0;
  int bodyIndex = 0;
  TopoDS_Shape original;
  TopoDS_Shape imprinted;
};

bool MaterialOf(const Body& body, const std::string& label, AnalysisType type,
                const glm::dvec3& gravity, bool plastic, Material& out,
                std::string* error) {
  const auto matType =
      CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
  if (const std::string why = CADMaterials::ElasticDataRefusal(mp);
      !why.empty()) {
    if (error)
      *error = Quote(label) + ": material '" +
               (body.material.empty() ? "default" : body.material) + "' " + why;
    return false;
  }
  const bool needsDensity =
      type == AnalysisType::Modal ||
      (gravity.x != 0 || gravity.y != 0 || gravity.z != 0);
  if (needsDensity && mp.density <= 0.0) {
    if (error)
      *error = Quote(label) + ": material has no density (needed for " +
               std::string(type == AnalysisType::Modal ? "modal analysis"
                                                       : "gravity") +
               ")";
    return false;
  }
  out = MaterialFromLibrary(mp);
  // A plastic study runs each material along its own curve, or not at all
  // (docs/simulation/README.md#plasticity).
  if (plastic)
    if (const std::string why = HardeningRefusal(
            out, Quote(label) + "'s material " + mp.name,
            CADMaterials::MaterialLibrary::HardeningSlugList());
        !why.empty()) {
      if (error)
        *error = why;
      return false;
    }
  return true;
}

} // namespace

// S2: the meshed occurrence and its BONDED neighbours as ONE mesh. Every
// member solid and every rigid body go through one fuzzy general fuse, so
// the faces two members share are imprinted once and the mesher meshes
// them once (P0: one connected component, no doubled nodes); each member
// keeps its own material by domain. Face indices are the mesher's own —
// TopExp::MapShapes over the compound, which counts a shared face once.
static bool PlanBondedGroup(const AssemblyDocument& asmDoc, Study& study,
                            const Participant* meshed,
                            const Occurrence* meshedOcc,
                            const std::vector<Body>& meshedBodies,
                            const std::vector<RigidSet>& bonded,
                            const std::vector<RigidSet>& rigid, StudyPlan& out,
                            std::string* error);

bool PlanAssemblyStudy(const AssemblyDocument& asmDoc, Study& study,
                       const AssemblyStudyContext& ctx, StudyPlan& out,
                       std::string* error) {
  out = StudyPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };
  if (!ctx.placedBodies)
    return fail("assembly study: no geometry source");
  std::string err;
  const SymbolTable none;
  if (!EvaluateStudyExprs(study, ctx.symbols ? *ctx.symbols : none, &err))
    return fail(err);
  if (study.type == AnalysisType::Topology)
    return fail("topology optimisation is a part study; an assembly study is "
                "static or modal");
  for (const Load& l : study.loads)
    if (l.type == LoadType::Pin)
      return fail("load " + std::to_string(l.id) +
                  ": a pin support is a part study's; an assembly study holds "
                  "its part with fixed faces or a rigid neighbour's contact");
  if (const std::string why = ContactRefusal(study); !why.empty())
    return fail(why);
  // Heat is a part study's (docs/simulation/README.md#not-done); a uniform
  // thermal load on a static study is taken, each member expanding by its
  // own material (docs/simulation/README.md#a-thermal-load-on-an-assembly).
  if (const std::string why = AssemblyThermalRefusal(study); !why.empty())
    return fail(why);
  if (const std::string why = ThermalRefusal(study); !why.empty())
    return fail(why);
  out.type = study.type;
  out.thermalLoad = study.thermalLoad;
  out.solver = EffectiveSolver(study);
  out.nonlinear = study.nonlinear;
  out.numModes = study.numModes;
  out.gravity = study.gravity;

  // Roles. Exactly one occurrence is meshed in S1; the rest are supports or
  // absent, and a study that names none says so rather than solving nothing.
  const Participant* meshed = nullptr;
  const Occurrence* meshedOcc = nullptr;
  std::vector<RigidSet> rigid;
  std::vector<RigidSet> bonded; // S2: meshed AND glued to the group
  std::vector<RigidSet> touching; // meshed apart, in contact with it
  for (const Participant& pt : study.participants) {
    const Occurrence* occ = asmDoc.FindById(pt.occurrenceId);
    if (!occ)
      return fail("participant occurrence " + std::to_string(pt.occurrenceId) +
                  " is not in this assembly (list_occurrences names them)");
    switch (pt.role) {
    case ParticipantRole::Meshed:
      if (meshed)
        return fail("this round meshes ONE occurrence, and both " +
                    Quote(meshedOcc->name) + " and " + Quote(occ->name) +
                    " are marked meshed; make one of them rigid or ignored");
      meshed = &pt;
      meshedOcc = occ;
      break;
    case ParticipantRole::Rigid:
      rigid.push_back({&pt, occ, {}});
      break;
    case ParticipantRole::Bonded:
      bonded.push_back({&pt, occ, {}});
      break;
    case ParticipantRole::Contact:
      touching.push_back({&pt, occ, {}});
      break;
    case ParticipantRole::Ignored:
      break;
    }
  }
  if (!meshed)
    return fail("no occurrence is marked meshed; name the one to analyse "
                "(participants: [{occurrence, role: meshed}, ...])");

  std::vector<Body> meshedBodies = ctx.placedBodies(meshed->occurrenceId);
  if (meshedBodies.empty())
    return fail(Quote(meshedOcc->name) + " has no solid body to mesh");
  for (RigidSet& r : rigid) {
    r.bodies = ctx.placedBodies(r.p->occurrenceId);
    if (r.bodies.empty())
      return fail("rigid support " + Quote(r.occ->name) + " has no bodies");
  }
  for (RigidSet& b : bonded) {
    b.bodies = ctx.placedBodies(b.p->occurrenceId);
    if (b.bodies.empty())
      return fail("bonded participant " + Quote(b.occ->name) +
                  " has no bodies");
  }
  for (RigidSet& c : touching) {
    c.bodies = ctx.placedBodies(c.p->occurrenceId);
    if (c.bodies.empty())
      return fail("contact participant " + Quote(c.occ->name) +
                  " has no bodies");
  }

  // Pre-flight: a support that interpenetrates the part is a press fit, and
  // P0 showed what fusing one does (the port tube's slivers). Refuse, naming
  // the pair, with `ignored` as the named way out.
  const glm::dmat4 I(1.0);
  const auto overlapCheck = [&](const std::string& nameA,
                                const std::vector<Body>& a,
                                const std::string& nameB,
                                const std::vector<Body>& b, const char* what) {
    const auto c = Measure::ContactBetween(a, I, b, I, /*wantDistance=*/false);
    if (!c)
      return fail("could not test whether " + Quote(nameA) + " and " +
                  Quote(nameB) + " overlap");
    if (c->volume > kOverlapRefuseMm3) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.3g", c->volume);
      return fail(Quote(nameA) + " and " + Quote(nameB) +
                  " INTERPENETRATE by " + buf + " mm^3; " + what);
    }
    return true;
  };
  constexpr const char* kRigidRule =
      "a rigid support must touch the part, not overlap it. Mark it ignored, "
      "or fix the placement";
  constexpr const char* kBondRule =
      "bonded parts share a face, they do not share volume (a press fit is "
      "not glue). Fix the placement";
  for (const RigidSet& r : rigid) {
    if (!overlapCheck(meshedOcc->name, meshedBodies, r.occ->name, r.bodies,
                      kRigidRule))
      return false;
    for (const RigidSet& b : bonded)
      if (!overlapCheck(b.occ->name, b.bodies, r.occ->name, r.bodies,
                        kRigidRule))
        return false;
  }
  for (size_t i = 0; i < bonded.size(); ++i) {
    if (!overlapCheck(meshedOcc->name, meshedBodies, bonded[i].occ->name,
                      bonded[i].bodies, kBondRule))
      return false;
    for (size_t j = i + 1; j < bonded.size(); ++j)
      if (!overlapCheck(bonded[i].occ->name, bonded[i].bodies,
                        bonded[j].occ->name, bonded[j].bodies, kBondRule))
        return false;
  }
  constexpr const char* kContactRule =
      "parts in contact touch, they do not share volume (a press fit is an "
      "interference, which a contact does not model). Fix the placement";
  for (const RigidSet& c : touching) {
    if (!overlapCheck(meshedOcc->name, meshedBodies, c.occ->name, c.bodies,
                      kContactRule))
      return false;
    for (const RigidSet& r : rigid)
      if (!overlapCheck(c.occ->name, c.bodies, r.occ->name, r.bodies,
                        kContactRule))
        return false;
  }
  // S2: with bonded neighbours the group is ONE mesh; its own planner.
  if (!bonded.empty()) {
    if (HasContact(study))
      return fail("a contact beside bonded participants is not built: the "
                  "bonded group is meshed as one shape, and its contacts are "
                  "not looked for on it. Make the bonded neighbours rigid or "
                  "ignored, or solve the contact in a study without them");
    return PlanBondedGroup(asmDoc, study, meshed, meshedOcc, meshedBodies,
                           bonded, rigid, out, error);
  }

  // Resolve every anchor before any geometry work, so a bad reference is
  // refused in a millisecond rather than after a fuse.
  struct Anchor {
    int body = -1; // index into meshedBodies
    int face = -1; // ORIGINAL face index on that body
  };
  auto resolveOn = [&](const ElementRef& ref, uint64_t occurrenceId,
                       const std::string& what, Anchor& a) -> bool {
    if (occurrenceId != 0 && occurrenceId != meshed->occurrenceId) {
      const Occurrence* o = asmDoc.FindById(occurrenceId);
      return fail(what + " is on " + Quote(o ? o->name : "?") +
                  ", which is not the meshed occurrence (" +
                  Quote(meshedOcc->name) +
                  "); a load or refinement belongs on the part being solved");
    }
    std::vector<int> hits;
    for (int i = 0; i < static_cast<int>(meshedBodies.size()); ++i) {
      if (ref.featureId != 0 && meshedBodies[i].createdBy != ref.featureId)
        continue;
      if (const auto idx = meshedBodies[i].elementMap.Resolve(ref.name)) {
        hits.push_back(i);
        a.body = i;
        a.face = *idx;
        if (ref.featureId != 0)
          return true;
      }
    }
    if (hits.size() == 1)
      return true;
    if (hits.size() > 1)
      return fail(what + ": '" + ref.name + "' is on " +
                  std::to_string(hits.size()) + " bodies of " +
                  Quote(meshedOcc->name) + "; say which with feature_id");
    return fail(what + ": face '" + ref.name + "' is not on " +
                Quote(meshedOcc->name));
  };
  struct BodyPlanning {
    std::vector<std::pair<int, size_t>> loads; // (original face, load index)
    std::vector<std::pair<int, double>> faceSizes;
    std::vector<size_t> contactLoads; // loads anchored on a contact
    std::vector<size_t> restsOn;      // contact supports on a rigid one
    std::vector<std::pair<int, size_t>> grounds; // (face, contact support)
  };
  std::map<int, BodyPlanning> perBody;
  // A contact participant's anchors (docs/simulation/README.md#contact):
  // the loads and refinements on its faces, by (set, body).
  struct Touching {
    std::vector<std::pair<int, size_t>> loads; // (face, load index)
    std::vector<std::pair<int, double>> faceSizes;
  };
  std::map<std::pair<size_t, int>, Touching> onTouching;
  const auto touchingOf = [&](uint64_t occurrenceId) -> int {
    for (size_t k = 0; k < touching.size(); ++k)
      if (touching[k].p->occurrenceId == occurrenceId)
        return static_cast<int>(k);
    return -1;
  };
  // A face of a contact participant: its body and face index.
  const auto resolveTouching = [&](const ElementRef& ref, size_t set,
                                   const std::string& what, Anchor& a) {
    const std::vector<Body>& bodies = touching[set].bodies;
    for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
      if (ref.featureId != 0 && bodies[i].createdBy != ref.featureId)
        continue;
      if (const auto idx = bodies[i].elementMap.Resolve(ref.name)) {
        a.body = i;
        a.face = *idx;
        return true;
      }
    }
    return fail(what + ": face '" + ref.name + "' is not on " +
                Quote(touching[set].occ->name));
  };
  for (size_t i = 0; i < study.loads.size(); ++i) {
    const Load& l = study.loads[i];
    if (l.type == LoadType::Contact && l.contactWith == 0 &&
        touchingOf(l.occurrenceId) < 0) {
      // The ground under a flat face of the meshed occurrence.
      Anchor a;
      if (!resolveOn(l.face, l.occurrenceId, "load " + std::to_string(i + 1),
                     a))
        return false;
      perBody[a.body].grounds.push_back({a.face, i});
      continue;
    }
    if (l.contactWith == 0 && touchingOf(l.occurrenceId) >= 0) {
      if (l.type == LoadType::Contact)
        return fail("load " + std::to_string(i + 1) +
                    ": a contact participant takes its contact from its "
                    "role; a contact support belongs on the meshed "
                    "occurrence");
      Anchor a;
      const size_t set = static_cast<size_t>(touchingOf(l.occurrenceId));
      if (!resolveTouching(l.face, set, "load " + std::to_string(i + 1), a))
        return false;
      onTouching[{set, a.body}].loads.push_back({a.face, i});
      continue;
    }
    if (l.contactWith != 0) {
      if (l.type != LoadType::Fixed && l.type != LoadType::Contact)
        return fail("load " + std::to_string(i + 1) +
                    ": only a fixed support or a contact support anchors "
                    "on a contact");
      bool known = false;
      for (const RigidSet& r : rigid)
        known = known || r.p->occurrenceId == l.contactWith;
      if (!known) {
        const Occurrence* o = asmDoc.FindById(l.contactWith);
        return fail("load " + std::to_string(i + 1) + ": contact with " +
                    Quote(o ? o->name : std::to_string(l.contactWith)) +
                    ", which is not a rigid participant of this study");
      }
      // Every meshed body may touch it; assigned after the imprint.
      for (int b = 0; b < static_cast<int>(meshedBodies.size()); ++b)
        (l.type == LoadType::Contact ? perBody[b].restsOn
                                     : perBody[b].contactLoads)
            .push_back(i);
      continue;
    }
    Anchor a;
    if (!resolveOn(l.face, l.occurrenceId, "load " + std::to_string(i + 1), a))
      return false;
    perBody[a.body].loads.push_back({a.face, i});
  }
  if (perBody.empty()) {
    // A rigid participant is not a fixture until it is CLAMPED, and there is
    // nothing in "no loads to solve" to say so — a caller who has declared
    // six supports reads it as a contradiction. The GUI has a Clamp here
    // button on every rigid row; over the RPC the same thing is a load.
    int rigid = 0;
    for (const Participant& pt : study.participants)
      if (pt.role == ParticipantRole::Rigid)
        rigid++;
    if (rigid > 0)
      return fail("study has no loads to solve — it has " +
                  std::to_string(rigid) +
                  " rigid participant(s), and a rigid support holds nothing "
                  "until it is clamped: add_load {type: fixed, contact_with: "
                  "<that occurrence>}");
    return fail("study has no loads to solve");
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    Anchor a;
    if (const int set = touchingOf(study.refinements[i].occurrenceId);
        set >= 0) {
      if (!resolveTouching(study.refinements[i].face, static_cast<size_t>(set),
                           "refinement " + std::to_string(i + 1), a))
        return false;
      onTouching[{static_cast<size_t>(set), a.body}].faceSizes.push_back(
          {a.face, study.refinements[i].size});
      continue;
    }
    if (!resolveOn(study.refinements[i].face, study.refinements[i].occurrenceId,
                   "refinement " + std::to_string(i + 1), a))
      return false;
    if (perBody.count(a.body))
      perBody[a.body].faceSizes.push_back({a.face, study.refinements[i].size});
  }

  // Imprint, per meshed body, against every rigid body; then map anchors.
  std::set<size_t> contactLoadsSatisfied;
  std::set<std::pair<size_t, int>> touchingPlaced; // (set, body)
  for (auto& [bi, planning] : perBody) {
    const Body& body = meshedBodies[bi];
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    const std::string bodyLabel =
        meshedOcc->name + (meshedBodies.size() > 1 ? "/" + body.name : "");
    if (const std::string why = CADMaterials::ElasticDataRefusal(mp);
        !why.empty())
      return fail(Quote(bodyLabel) + ": material '" +
                  (body.material.empty() ? "default" : body.material) + "' " +
                  why);
    const bool needsDensity =
        study.type == AnalysisType::Modal ||
        (study.gravity.x != 0 || study.gravity.y != 0 || study.gravity.z != 0);
    if (needsDensity && mp.density <= 0.0)
      return fail(Quote(bodyLabel) + ": material has no density (needed for " +
                  std::string(study.type == AnalysisType::Modal
                                  ? "modal analysis"
                                  : "gravity") +
                  ")");

    // The fuse never touches the documents' shapes: copies in, and
    // NonDestructive besides — a boolean WRITES onto its arguments
    // (docs/inspect/README.md#query-booleans-are-non-destructive).
    const TopoDS_Shape original =
        BRepBuilderAPI_Copy(body.shape.GetShape()).Shape();
    std::vector<TopoDS_Shape> rigidOriginals;
    std::vector<std::pair<int, int>> rigidOwner; // (rigid set, body index)
    BOPAlgo_Builder gf;
    gf.SetNonDestructive(Standard_True);
    gf.SetFuzzyValue(kImprintFuzzMm);
    gf.AddArgument(original);
    for (size_t ri = 0; ri < rigid.size(); ++ri)
      for (size_t rb = 0; rb < rigid[ri].bodies.size(); ++rb) {
        rigidOriginals.push_back(
            BRepBuilderAPI_Copy(rigid[ri].bodies[rb].shape.GetShape()).Shape());
        rigidOwner.push_back({static_cast<int>(ri), static_cast<int>(rb)});
        gf.AddArgument(rigidOriginals.back());
      }
    TopoDS_Shape imprinted = original;
    std::vector<TopoDS_Shape> originalFaces = FacesOf(original);
    std::vector<TopoDS_Shape> imprintedFaces = originalFaces;
    const bool anyRigid = !rigidOriginals.empty();
    if (anyRigid) {
      gf.Perform();
      if (gf.HasErrors())
        return fail(Quote(bodyLabel) +
                    ": imprinting its supports failed in the kernel");
      imprinted = ResultSolidOf(original, gf.Shape());
      if (imprinted.IsNull())
        return fail(Quote(bodyLabel) +
                    ": the imprint did not leave one solid where the part "
                    "was (are two participants overlapping?)");
      imprintedFaces = FacesOf(imprinted);
    }
    auto piecesOf = [&](int originalFace) -> std::vector<int> {
      if (originalFace < 0 || originalFace >= (int)originalFaces.size())
        return {};
      if (!anyRigid)
        return {originalFace};
      return ImprintedFaces(gf, originalFaces[originalFace], imprintedFaces);
    };

    StudyBodyPlan bp;
    bp.bodyIndex = bi;
    bp.bodyName = bodyLabel;
    bp.mat = MaterialFromLibrary(mp);
    if (study.nonlinear.plasticity)
      if (const std::string why = HardeningRefusal(
              bp.mat, Quote(bodyLabel) + "'s material " + mp.name,
              CADMaterials::MaterialLibrary::HardeningSlugList());
          !why.empty())
        return fail(why);
    if (study.thermalLoad.On())
      if (const std::string why = ThermalMaterialRefusal(
              bp.mat, Quote(bodyLabel) + "'s material " + mp.name, false, true,
              CADMaterials::MaterialLibrary::ExpansionSlugList());
          !why.empty())
        return fail(why);
    bp.shape = imprinted;
    bp.settings.maxSize =
        meshed->meshSize > 0.0 ? meshed->meshSize : study.meshSize;
    for (const auto& [face, size] : planning.faceSizes)
      for (int piece : piecesOf(face))
        bp.settings.faceSizes.push_back({piece, size});

    bool hasFixed = false;
    for (const auto& [face, li] : planning.loads) {
      const Load& l = study.loads[li];
      const std::vector<int> pieces = piecesOf(face);
      if (pieces.empty())
        return fail("load " + std::to_string(li + 1) +
                    ": its face vanished in the imprint");
      if (l.type == LoadType::Fixed)
        hasFixed = true;
      if (l.type == LoadType::Force && pieces.size() > 1) {
        // A force is a TOTAL: a face the imprint split carries it by area.
        double total = 0.0;
        for (int piece : pieces)
          total += AreaOf(imprintedFaces[piece]);
        for (int piece : pieces) {
          Load part = l;
          part.magnitude =
              total > 0.0 ? l.magnitude * AreaOf(imprintedFaces[piece]) / total
                          : 0.0;
          bp.loads.push_back({piece, part});
        }
      } else {
        // A torque or a remote load is a field over the whole face: every
        // piece carries it whole, and MeshPlannedStudy makes the pieces of
        // one load one FaceLoad again.
        Load whole = l;
        std::string why;
        if (!CompleteLoadGeometry(imprintedFaces[pieces.front()], whole, &why))
          return fail(why);
        for (int piece : pieces)
          bp.loads.push_back({piece, whole});
      }
    }

    // Contacts: the faces this body's imprint shares with each rigid body.
    for (size_t ri = 0; ri < rigid.size(); ++ri) {
      ContactPatch patch;
      patch.rigidOccurrenceId = rigid[ri].p->occurrenceId;
      patch.rigidName = rigid[ri].occ->name;
      patch.meshedBody = static_cast<int>(out.bodies.size());
      std::set<int> meshedFaces;
      std::set<int> rigidFaces;
      for (size_t k = 0; k < rigidOriginals.size(); ++k) {
        if (rigidOwner[k].first != static_cast<int>(ri))
          continue;
        const TopoDS_Shape rigidResult =
            ResultSolidOf(rigidOriginals[k], gf.Shape());
        if (rigidResult.IsNull())
          continue;
        TopTools_MapOfShape shared;
        for (TopExp_Explorer ex(rigidResult, TopAbs_FACE); ex.More(); ex.Next())
          shared.Add(ex.Current());
        for (size_t f = 0; f < imprintedFaces.size(); ++f)
          if (shared.Contains(imprintedFaces[f]))
            meshedFaces.insert(static_cast<int>(f));
        // The rigid body's ORIGINAL faces that carry a shared piece.
        const std::vector<TopoDS_Shape> rf = FacesOf(rigidOriginals[k]);
        std::vector<TopoDS_Shape> rigidResultFaces = FacesOf(rigidResult);
        for (size_t f = 0; f < rf.size(); ++f) {
          for (int piece : ImprintedFaces(gf, rf[f], rigidResultFaces)) {
            bool onMeshed = false;
            for (const TopoDS_Shape& mf : imprintedFaces)
              onMeshed = onMeshed || mf.IsSame(rigidResultFaces[piece]);
            if (onMeshed) {
              rigidFaces.insert(static_cast<int>(f));
              patch.rigidBody = rigidOwner[k].second;
            }
          }
        }
      }
      patch.meshedFaces.assign(meshedFaces.begin(), meshedFaces.end());
      patch.rigidFaces.assign(rigidFaces.begin(), rigidFaces.end());
      // Contact anchors on this support clamp exactly those faces.
      for (size_t li : planning.contactLoads) {
        const Load& l = study.loads[li];
        if (l.contactWith != patch.rigidOccurrenceId ||
            patch.meshedFaces.empty())
          continue;
        hasFixed = true;
        contactLoadsSatisfied.insert(li);
        for (int piece : patch.meshedFaces)
          bp.loads.push_back({piece, l});
      }
      if (!patch.meshedFaces.empty())
        out.contacts.push_back(std::move(patch));
    }
    // Contact (docs/simulation/README.md#contact): each pair this body's
    // faces against a partner's that come near and face them.
    const double h =
        bp.settings.maxSize > 0 ? bp.settings.maxSize : AutoMeshSize(imprinted);
    const double bodyDiagonal = DiagonalOf(original);
    int faceOffset = static_cast<int>(imprintedFaces.size());
    const auto addContact = [&](uint64_t id, bool participant, double friction,
                                const std::set<int>& mine,
                                StudyBodyPlan::ContactPartner partner) {
      StudyBodyPlan::Contact c;
      c.id = id;
      c.participant = participant;
      c.faces.assign(mine.begin(), mine.end());
      c.partner = static_cast<int>(bp.partners.size());
      c.friction = friction;
      bp.partners.push_back(std::move(partner));
      bp.contacts.push_back(std::move(c));
      hasFixed = true;
    };
    // On a rigid occurrence: its bodies, meshed coarse and fixed.
    for (size_t li : planning.restsOn) {
      const Load& l = study.loads[li];
      for (size_t k = 0; k < rigidOriginals.size(); ++k) {
        const RigidSet& rs = rigid[rigidOwner[k].first];
        if (rs.p->occurrenceId != l.contactWith)
          continue;
        const std::vector<TopoDS_Shape> theirs = FacesOf(rigidOriginals[k]);
        std::set<int> mine, onTheirs;
        FacingFaces(imprintedFaces, theirs,
                    kContactClearanceFraction *
                        std::min(bodyDiagonal, DiagonalOf(rigidOriginals[k])),
                    mine, onTheirs);
        if (mine.empty())
          continue;
        StudyBodyPlan::ContactPartner pt;
        pt.rigid = true;
        pt.name = rs.occ->name;
        pt.shape = rigidOriginals[k];
        pt.settings.maxSize = kRigidCoarsen * h;
        for (int f : onTheirs)
          pt.settings.faceSizes.push_back({f, h});
        pt.faces.assign(onTheirs.begin(), onTheirs.end());
        contactLoadsSatisfied.insert(li);
        addContact(l.id, false, l.friction, mine, std::move(pt));
      }
    }
    // On the ground under a flat face.
    for (const auto& [face, li] : planning.grounds) {
      const Load& l = study.loads[li];
      StudyBodyPlan::ContactPartner ground;
      std::string why;
      if (face < 0 || face >= static_cast<int>(originalFaces.size()) ||
          !GroundUnder(originalFaces[face], ground, &why))
        return fail("load " + std::to_string(li + 1) + ": " + why);
      const std::vector<int> pieces = piecesOf(face);
      addContact(l.id, false, l.friction,
                 std::set<int>(pieces.begin(), pieces.end()),
                 std::move(ground));
    }
    // Against a contact participant: meshed with this body, apart from it.
    for (size_t set = 0; set < touching.size(); ++set) {
      const RigidSet& ts = touching[set];
      for (int tb = 0; tb < static_cast<int>(ts.bodies.size()); ++tb) {
        const TopoDS_Shape theirShape =
            BRepBuilderAPI_Copy(ts.bodies[tb].shape.GetShape()).Shape();
        const std::vector<TopoDS_Shape> theirs = FacesOf(theirShape);
        std::set<int> mine, onTheirs;
        FacingFaces(imprintedFaces, theirs,
                    kContactClearanceFraction *
                        std::min(bodyDiagonal, DiagonalOf(theirShape)),
                    mine, onTheirs);
        if (mine.empty())
          continue;
        const std::string label =
            ts.occ->name +
            (ts.bodies.size() > 1 ? "/" + ts.bodies[tb].name : "");
        if (!touchingPlaced.insert({set, tb}).second)
          return fail(Quote(label) + " touches two bodies of " +
                      Quote(meshedOcc->name) +
                      "; a contact participant is in contact with one");
        StudyBodyPlan::ContactPartner pt;
        pt.rigid = false;
        pt.name = label;
        pt.shape = theirShape;
        std::string why;
        if (!MaterialOf(ts.bodies[tb], label, study.type, study.gravity,
                        study.nonlinear.plasticity, pt.mat, &why))
          return fail(why);
        pt.settings.maxSize =
            ts.p->meshSize > 0.0 ? ts.p->meshSize : study.meshSize;
        pt.faces.assign(onTheirs.begin(), onTheirs.end());
        pt.faceOffset = faceOffset;
        faceOffset += static_cast<int>(theirs.size());
        if (const auto it = onTouching.find({set, tb});
            it != onTouching.end()) {
          pt.settings.faceSizes = it->second.faceSizes;
          for (const auto& [face, li] : it->second.loads) {
            Load ld = study.loads[li];
            if (ld.type != LoadType::Force && face >= 0 &&
                face < static_cast<int>(theirs.size()) &&
                !CompleteLoadGeometry(theirs[face], ld, &why))
              return fail(why);
            bp.loads.push_back({face + pt.faceOffset, ld});
          }
        }
        addContact(ts.p->occurrenceId, true, ts.p->friction, mine,
                   std::move(pt));
      }
    }

    // A body only a contact could have clamped, and nothing touches: it is
    // not in this analysis (a multi-body part's loose piece). A body that
    // carries loads and no support is the same refusal a part study makes.
    if (bp.loads.empty() && bp.contacts.empty())
      continue;
    if (!hasFixed) {
      // The honest reason first: a contact anchor whose support does not
      // reach this body is "does not touch", not "no fixed support".
      for (size_t li : planning.contactLoads)
        if (!contactLoadsSatisfied.count(li)) {
          const Occurrence* o = asmDoc.FindById(study.loads[li].contactWith);
          return fail("load " + std::to_string(li + 1) + ": " +
                      Quote(o ? o->name : "?") + " does not touch " +
                      Quote(bodyLabel) +
                      " (no shared face after imprinting at 1e-3 mm), so "
                      "there is nothing to clamp");
        }
      return fail(Quote(bodyLabel) + " has loads but no fixed support");
    }
    out.bodies.push_back(std::move(bp));
  }
  for (size_t li = 0; li < study.loads.size(); ++li) {
    const Load& l = study.loads[li];
    if (l.contactWith == 0 || l.type == LoadType::Contact ||
        contactLoadsSatisfied.count(li))
      continue;
    const Occurrence* o = asmDoc.FindById(l.contactWith);
    return fail("load " + std::to_string(li + 1) + ": " +
                Quote(o ? o->name : std::to_string(l.contactWith)) +
                " does not touch " + Quote(meshedOcc->name) +
                " (no shared face after imprinting at 1e-3 mm), so there is "
                "nothing to clamp");
  }
  for (size_t li = 0; li < study.loads.size(); ++li) {
    const Load& l = study.loads[li];
    if (l.type != LoadType::Contact || l.contactWith == 0 ||
        contactLoadsSatisfied.count(li))
      continue;
    const Occurrence* o = asmDoc.FindById(l.contactWith);
    return fail("load " + std::to_string(li + 1) + ": " +
                Quote(o ? o->name : std::to_string(l.contactWith)) +
                " does not touch " + Quote(meshedOcc->name) +
                " where a face of it faces one of its own, so there is "
                "nothing for it to rest on");
  }
  for (size_t set = 0; set < touching.size(); ++set) {
    bool placed = false;
    for (const auto& [ts, tb] : touchingPlaced)
      placed = placed || ts == set;
    if (!placed)
      return fail("contact participant " + Quote(touching[set].occ->name) +
                  " does not touch " + Quote(meshedOcc->name) +
                  " where a face of it faces one of its own; mark it "
                  "ignored, or fix the placement");
  }
  if (out.bodies.empty())
    return fail(Quote(meshedOcc->name) + ": nothing to solve");
  return true;
}

static bool PlanBondedGroup(const AssemblyDocument& asmDoc, Study& study,
                            const Participant* meshed,
                            const Occurrence* meshedOcc,
                            const std::vector<Body>& meshedBodies,
                            const std::vector<RigidSet>& bonded,
                            const std::vector<RigidSet>& rigid, StudyPlan& out,
                            std::string* error) {
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };
  // The members, with materials validated before any geometry work.
  std::vector<GroupMember> members;
  const auto addMembers = [&](const Participant* p, const Occurrence* occ,
                              const std::vector<Body>& bodies) -> bool {
    for (int bi = 0; bi < static_cast<int>(bodies.size()); ++bi) {
      GroupMember m;
      m.p = p;
      m.occ = occ;
      m.bodyIndex = bi;
      m.body = &bodies[bi];
      std::string why;
      if (!MaterialOf(bodies[bi],
                      occ->name +
                          (bodies.size() > 1 ? "/" + bodies[bi].name : ""),
                      study.type, study.gravity, study.nonlinear.plasticity,
                      m.mat, &why))
        return fail(why);
      // Under a thermal load each member expands by its own coefficient,
      // which reaches the solver as its domain's material.
      if (study.thermalLoad.On())
        if (const std::string refusal = ThermalMaterialRefusal(
                m.mat,
                Quote(occ->name +
                      (bodies.size() > 1 ? "/" + bodies[bi].name : "")) +
                    "'s material " +
                    CADMaterials::MaterialLibrary::GetMaterialOrDefault(
                        bodies[bi].material)
                        .name,
                false, true,
                CADMaterials::MaterialLibrary::ExpansionSlugList());
            !refusal.empty())
          return fail(refusal);
      m.original = BRepBuilderAPI_Copy(bodies[bi].shape.GetShape()).Shape();
      m.originalFaces = FacesOf(m.original);
      members.push_back(std::move(m));
    }
    return true;
  };
  if (!addMembers(meshed, meshedOcc, meshedBodies))
    return false;
  for (const RigidSet& b : bonded)
    if (!addMembers(b.p, b.occ, b.bodies))
      return false;

  // One fuse for everything that touches: members and supports.
  BOPAlgo_Builder gf;
  gf.SetNonDestructive(Standard_True);
  gf.SetFuzzyValue(kImprintFuzzMm);
  for (GroupMember& m : members)
    gf.AddArgument(m.original);
  std::vector<RigidBodyRef> rigidBodies;
  for (size_t ri = 0; ri < rigid.size(); ++ri)
    for (size_t rb = 0; rb < rigid[ri].bodies.size(); ++rb) {
      RigidBodyRef r;
      r.rigidSet = ri;
      r.bodyIndex = static_cast<int>(rb);
      r.original =
          BRepBuilderAPI_Copy(rigid[ri].bodies[rb].shape.GetShape()).Shape();
      gf.AddArgument(r.original);
      rigidBodies.push_back(std::move(r));
    }
  gf.Perform();
  if (gf.HasErrors())
    return fail("imprinting the bonded group failed in the kernel");
  for (GroupMember& m : members) {
    m.imprinted = ResultSolidOf(m.original, gf.Shape());
    if (m.imprinted.IsNull())
      return fail(Quote(m.occ->name) +
                  ": the imprint did not leave one solid where the part was "
                  "(are two participants overlapping?)");
  }
  for (RigidBodyRef& r : rigidBodies)
    r.imprinted = ResultSolidOf(r.original, gf.Shape());

  // The group as one shape, its faces indexed the way the mesher indexes
  // them (a shared face once).
  TopoDS_Compound group;
  BRep_Builder builder;
  builder.MakeCompound(group);
  for (const GroupMember& m : members)
    builder.Add(group, m.imprinted);
  TopTools_IndexedMapOfShape faceMap;
  TopExp::MapShapes(group, TopAbs_FACE, faceMap);
  const auto indexIn = [&](const TopoDS_Shape& face) -> int {
    const int i = faceMap.FindIndex(face);
    return i > 0 ? i - 1 : -1;
  };
  // The group faces an ORIGINAL face of a member became.
  const auto piecesOf = [&](const GroupMember& m, int originalFace) {
    std::vector<int> outIdx;
    if (originalFace < 0 || originalFace >= (int)m.originalFaces.size())
      return outIdx;
    const TopoDS_Shape& of = m.originalFaces[originalFace];
    const TopTools_ListOfShape& mod = gf.Modified(of);
    if (mod.IsEmpty()) {
      const int i = indexIn(of);
      if (i >= 0)
        outIdx.push_back(i);
      return outIdx;
    }
    for (TopTools_ListOfShape::Iterator it(mod); it.More(); it.Next()) {
      const int i = indexIn(it.Value());
      if (i >= 0)
        outIdx.push_back(i);
    }
    return outIdx;
  };

  StudyBodyPlan bp;
  bp.bodyIndex = 0;
  bp.bodyName = meshedOcc->name;
  for (const RigidSet& b : bonded)
    bp.bodyName += " + " + b.occ->name;
  bp.shape = group;
  bp.mat = members.front().mat;
  bp.settings.maxSize =
      meshed->meshSize > 0.0 ? meshed->meshSize : study.meshSize;
  for (const GroupMember& m : members) {
    bp.members.push_back({m.imprinted, m.mat, m.occ->name});
    // A bonded member's own mesh size lands on every face of its solid.
    if (m.p != meshed && m.p->meshSize > 0.0)
      for (TopExp_Explorer ex(m.imprinted, TopAbs_FACE); ex.More(); ex.Next())
        bp.settings.faceSizes.push_back({indexIn(ex.Current()), m.p->meshSize});
  }

  // Anchors: a load or refinement on any member's face.
  const auto resolveMember = [&](const ElementRef& ref, uint64_t occurrenceId,
                                 const std::string& what, const GroupMember*& m,
                                 int& face) -> bool {
    std::vector<const GroupMember*> hits;
    for (const GroupMember& cand : members) {
      if (occurrenceId != 0 && cand.occ->id != occurrenceId)
        continue;
      if (ref.featureId != 0 && cand.body->createdBy != ref.featureId)
        continue;
      if (const auto idx = cand.body->elementMap.Resolve(ref.name)) {
        hits.push_back(&cand);
        m = &cand;
        face = *idx;
      }
    }
    if (hits.size() == 1)
      return true;
    if (hits.size() > 1)
      return fail(what + ": '" + ref.name + "' is on " +
                  std::to_string(hits.size()) +
                  " bodies of the group; say which with occurrence and "
                  "feature_id");
    const Occurrence* o = asmDoc.FindById(occurrenceId);
    return fail(what + ": face '" + ref.name + "' is not on " +
                Quote(o ? o->name : "a meshed or bonded occurrence"));
  };
  bool hasFixed = false;
  std::vector<size_t> contactLoads;
  for (size_t li = 0; li < study.loads.size(); ++li) {
    const Load& l = study.loads[li];
    if (l.contactWith != 0) {
      if (l.type != LoadType::Fixed)
        return fail("load " + std::to_string(li + 1) +
                    ": only a fixed support anchors on a contact");
      bool known = false;
      for (const RigidSet& r : rigid)
        known = known || r.p->occurrenceId == l.contactWith;
      if (!known) {
        const Occurrence* o = asmDoc.FindById(l.contactWith);
        return fail("load " + std::to_string(li + 1) + ": contact with " +
                    Quote(o ? o->name : std::to_string(l.contactWith)) +
                    ", which is not a rigid participant of this study");
      }
      contactLoads.push_back(li);
      continue;
    }
    const GroupMember* m = nullptr;
    int face = -1;
    if (!resolveMember(l.face, l.occurrenceId, "load " + std::to_string(li + 1),
                       m, face))
      return false;
    const std::vector<int> pieces = piecesOf(*m, face);
    if (pieces.empty())
      return fail("load " + std::to_string(li + 1) +
                  ": its face vanished in the imprint");
    if (l.type == LoadType::Fixed)
      hasFixed = true;
    if (l.type == LoadType::Force && pieces.size() > 1) {
      double total = 0.0;
      for (int piece : pieces)
        total += AreaOf(faceMap(piece + 1));
      for (int piece : pieces) {
        Load part = l;
        part.magnitude = total > 0.0
                             ? l.magnitude * AreaOf(faceMap(piece + 1)) / total
                             : 0.0;
        bp.loads.push_back({piece, part});
      }
    } else {
      // A torque or remote load: whole on every piece (see the meshed
      // occurrence's planning above).
      Load whole = l;
      std::string why;
      if (!CompleteLoadGeometry(faceMap(pieces.front() + 1), whole, &why))
        return fail(why);
      for (int piece : pieces)
        bp.loads.push_back({piece, whole});
    }
  }
  for (size_t ri = 0; ri < study.refinements.size(); ++ri) {
    const GroupMember* m = nullptr;
    int face = -1;
    if (!resolveMember(study.refinements[ri].face,
                       study.refinements[ri].occurrenceId,
                       "refinement " + std::to_string(ri + 1), m, face))
      return false;
    for (int piece : piecesOf(*m, face))
      bp.settings.faceSizes.push_back({piece, study.refinements[ri].size});
  }

  // Contacts with the rigid supports: faces the group shares with them.
  std::set<size_t> contactSatisfied;
  for (size_t ri = 0; ri < rigid.size(); ++ri) {
    ContactPatch patch;
    patch.rigidOccurrenceId = rigid[ri].p->occurrenceId;
    patch.rigidName = rigid[ri].occ->name;
    patch.meshedBody = 0;
    std::set<int> meshedFaces, rigidFaces;
    for (const RigidBodyRef& r : rigidBodies) {
      if (r.rigidSet != ri || r.imprinted.IsNull())
        continue;
      const std::vector<TopoDS_Shape> rigidResultFaces = FacesOf(r.imprinted);
      for (const TopoDS_Shape& rf : rigidResultFaces) {
        const int i = indexIn(rf);
        if (i >= 0)
          meshedFaces.insert(i);
      }
      const std::vector<TopoDS_Shape> rfOrig = FacesOf(r.original);
      for (size_t f = 0; f < rfOrig.size(); ++f)
        for (int piece : ImprintedFaces(gf, rfOrig[f], rigidResultFaces))
          if (indexIn(rigidResultFaces[piece]) >= 0) {
            rigidFaces.insert(static_cast<int>(f));
            patch.rigidBody = r.bodyIndex;
          }
    }
    patch.meshedFaces.assign(meshedFaces.begin(), meshedFaces.end());
    patch.rigidFaces.assign(rigidFaces.begin(), rigidFaces.end());
    for (size_t li : contactLoads) {
      const Load& l = study.loads[li];
      if (l.contactWith != patch.rigidOccurrenceId || patch.meshedFaces.empty())
        continue;
      hasFixed = true;
      contactSatisfied.insert(li);
      for (int piece : patch.meshedFaces)
        bp.loads.push_back({piece, l});
    }
    if (!patch.meshedFaces.empty())
      out.contacts.push_back(std::move(patch));
  }
  for (size_t li : contactLoads)
    if (!contactSatisfied.count(li)) {
      const Occurrence* o = asmDoc.FindById(study.loads[li].contactWith);
      return fail("load " + std::to_string(li + 1) + ": " +
                  Quote(o ? o->name : "?") +
                  " does not touch the group (no "
                  "shared face after imprinting at 1e-3 mm), so there is "
                  "nothing to clamp");
    }

  // Bonds: the faces two members share. A bonded member that shares
  // nothing is not bonded to anything, and says so.
  std::vector<bool> touched(members.size(), false);
  for (size_t a = 0; a < members.size(); ++a) {
    TopTools_MapOfShape facesA;
    for (TopExp_Explorer ex(members[a].imprinted, TopAbs_FACE); ex.More();
         ex.Next())
      facesA.Add(ex.Current());
    for (size_t b = a + 1; b < members.size(); ++b) {
      const std::vector<TopoDS_Shape> bFaces = FacesOf(members[b].imprinted);
      bool shared = false;
      for (const TopoDS_Shape& f : bFaces)
        shared = shared || facesA.Contains(f);
      if (!shared)
        continue;
      touched[a] = touched[b] = true;
      BondPatch bond;
      bond.occurrenceA = members[a].occ->id;
      bond.occurrenceB = members[b].occ->id;
      bond.bodyA = members[a].bodyIndex;
      bond.bodyB = members[b].bodyIndex;
      const auto originalsCarrying = [&](const GroupMember& m,
                                         const TopTools_MapOfShape& other) {
        std::vector<int> outFaces;
        const std::vector<TopoDS_Shape> resultFaces = FacesOf(m.imprinted);
        for (size_t f = 0; f < m.originalFaces.size(); ++f)
          for (int piece : ImprintedFaces(gf, m.originalFaces[f], resultFaces))
            if (other.Contains(resultFaces[piece])) {
              outFaces.push_back(static_cast<int>(f));
              break;
            }
        return outFaces;
      };
      TopTools_MapOfShape facesB;
      for (const TopoDS_Shape& f : bFaces)
        facesB.Add(f);
      bond.facesA = originalsCarrying(members[a], facesB);
      bond.facesB = originalsCarrying(members[b], facesA);
      out.bonds.push_back(std::move(bond));
    }
  }
  for (size_t i = 0; i < members.size(); ++i)
    if (!touched[i] && members[i].p != meshed)
      return fail(Quote(members[i].occ->name) +
                  " is marked bonded but shares no face with the group "
                  "(nothing to glue it to at 1e-3 mm); make it rigid, or "
                  "ignored, or fix the placement");
  if (members.size() > 1 && !touched[0])
    return fail(Quote(meshedOcc->name) +
                " shares no face with its bonded neighbours");

  if (bp.loads.empty())
    return fail(Quote(bp.bodyName) + ": nothing to solve");
  if (!hasFixed)
    return fail(Quote(bp.bodyName) + " has loads but no fixed support");
  out.bodies.push_back(std::move(bp));
  return true;
}

} // namespace fem
