// A study on an assembly (S1, docs/simulation/assembly-studies-plan.md): one
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
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
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

} // namespace

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
  out.type = study.type;
  out.numModes = study.numModes;
  out.gravity = study.gravity;

  // Roles. Exactly one occurrence is meshed in S1; the rest are supports or
  // absent, and a study that names none says so rather than solving nothing.
  const Participant* meshed = nullptr;
  const Occurrence* meshedOcc = nullptr;
  struct RigidSet {
    const Participant* p = nullptr;
    const Occurrence* occ = nullptr;
    std::vector<Body> bodies;
  };
  std::vector<RigidSet> rigid;
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

  // Pre-flight: a support that interpenetrates the part is a press fit, and
  // P0 showed what fusing one does (the port tube's slivers). Refuse, naming
  // the pair, with `ignored` as the named way out.
  const glm::mat4 I(1.0f);
  for (const RigidSet& r : rigid) {
    const auto c = Measure::ContactBetween(meshedBodies, I, r.bodies, I,
                                           /*wantDistance=*/false);
    if (!c)
      return fail("could not test whether " + Quote(meshedOcc->name) + " and " +
                  Quote(r.occ->name) + " overlap");
    if (c->volume > kOverlapRefuseMm3) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.3g", c->volume);
      return fail(Quote(meshedOcc->name) + " and " + Quote(r.occ->name) +
                  " INTERPENETRATE by " + buf +
                  " mm^3; a rigid support must touch the part, not overlap "
                  "it. Mark it ignored, or fix the placement");
    }
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
  };
  std::map<int, BodyPlanning> perBody;
  for (size_t i = 0; i < study.loads.size(); ++i) {
    const Load& l = study.loads[i];
    if (l.contactWith != 0) {
      if (l.type != LoadType::Fixed)
        return fail("load " + std::to_string(i + 1) +
                    ": only a fixed support anchors on a contact");
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
        perBody[b].contactLoads.push_back(i);
      continue;
    }
    Anchor a;
    if (!resolveOn(l.face, l.occurrenceId, "load " + std::to_string(i + 1), a))
      return false;
    perBody[a.body].loads.push_back({a.face, i});
  }
  if (perBody.empty())
    return fail("study has no loads to solve");
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    Anchor a;
    if (!resolveOn(study.refinements[i].face, study.refinements[i].occurrenceId,
                   "refinement " + std::to_string(i + 1), a))
      return false;
    if (perBody.count(a.body))
      perBody[a.body].faceSizes.push_back({a.face, study.refinements[i].size});
  }

  // Imprint, per meshed body, against every rigid body; then map anchors.
  std::set<size_t> contactLoadsSatisfied;
  for (auto& [bi, planning] : perBody) {
    const Body& body = meshedBodies[bi];
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    const std::string bodyLabel =
        meshedOcc->name + (meshedBodies.size() > 1 ? "/" + body.name : "");
    if (mp.youngGPa <= 0.0f)
      return fail(Quote(bodyLabel) + ": material '" +
                  (body.material.empty() ? "default" : body.material) +
                  "' has no stiffness data");
    const bool needsDensity =
        study.type == AnalysisType::Modal ||
        (study.gravity.x != 0 || study.gravity.y != 0 || study.gravity.z != 0);
    if (needsDensity && mp.density <= 0.0f)
      return fail(Quote(bodyLabel) + ": material has no density (needed for " +
                  std::string(study.type == AnalysisType::Modal
                                  ? "modal analysis"
                                  : "gravity") +
                  ")");

    // The fuse never touches the documents' shapes: copies in, and
    // NonDestructive besides — a boolean WRITES onto its arguments
    // (docs/performance/query-booleans.md).
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
    bp.mat = Material{mp.youngGPa * 1000.0, mp.poisson, mp.density * 1e-12};
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
        for (int piece : pieces)
          bp.loads.push_back({piece, l});
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
    // A body only a contact could have clamped, and nothing touches: it is
    // not in this analysis (a multi-body part's loose piece). A body that
    // carries loads and no support is the same refusal a part study makes.
    if (bp.loads.empty())
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
    if (l.contactWith == 0 || contactLoadsSatisfied.count(li))
      continue;
    const Occurrence* o = asmDoc.FindById(l.contactWith);
    return fail("load " + std::to_string(li + 1) + ": " +
                Quote(o ? o->name : std::to_string(l.contactWith)) +
                " does not touch " + Quote(meshedOcc->name) +
                " (no shared face after imprinting at 1e-3 mm), so there is "
                "nothing to clamp");
  }
  if (out.bodies.empty())
    return fail(Quote(meshedOcc->name) + ": nothing to solve");
  return true;
}

} // namespace fem
