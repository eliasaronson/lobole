#include "core/BodyMode.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BOPTools_AlgoTools3D.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IntTools_Context.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {
// Do two axis-aligned bounding boxes overlap?
bool BoxesOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                  const glm::vec3& minB, const glm::vec3& maxB) {
  return minA.x <= maxB.x && maxA.x >= minB.x && minA.y <= maxB.y &&
         maxA.y >= minB.y && minA.z <= maxB.z && maxA.z >= minB.z;
}
} // namespace

namespace {
// Two volumes that agree to a relative tolerance — the boolean did nothing.
bool VolumeUnchanged(double before, double after) {
  return before > 1e-9 &&
         std::abs(before - after) < 1e-6 * std::max(before, 1.0);
}

// Route a failure reason to the caller when it wants one, else to the log
// (Document logs the caller-surfaced text again, so never both).
void Fail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  else
    LOG_ERROR("{}", message);
}
} // namespace

namespace {
// How far two volumes may differ and still be "the same": relative to the
// larger, because a kernel's volume integral is good to a fraction of the
// volume, not to a fixed number of cubic millimetres.
constexpr double kVolumeRel = 1e-6;

const char* OpNoun(BooleanOp op) {
  switch (op) {
  case BooleanOp::Union:
    return "add";
  case BooleanOp::Subtract:
    return "cut";
  case BooleanOp::Intersect:
    return "intersect";
  }
  return "boolean";
}

std::string Mm3(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.1f mm3", v);
  return buf;
}

std::string At(const gp_Pnt& p) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "(%.2f, %.2f, %.2f)", p.X(), p.Y(), p.Z());
  return buf;
}

Bnd_Box BoxOf(const CADShape& shape) {
  glm::vec3 lo, hi;
  shape.GetBoundingBox(lo, hi); // memoised on the shape
  Bnd_Box box;
  box.Update(lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
  return box;
}

// Where a point is relative to a body that may be several solids: IN any of
// them wins, then ON any, else OUT. The classifiers are built on first use.
class Where {
public:
  Where(const TopoDS_Shape& shape, double tol) : m_shape(shape), m_tol(tol) {}
  TopAbs_State Of(const gp_Pnt& p) {
    if (!m_built) {
      for (TopExp_Explorer ex(m_shape, TopAbs_SOLID); ex.More(); ex.Next())
        m_solids.push_back(
            std::make_unique<BRepClass3d_SolidClassifier>(ex.Current()));
      m_built = true;
    }
    TopAbs_State best = TopAbs_OUT;
    for (auto& c : m_solids) {
      c->Perform(p, m_tol);
      if (c->State() == TopAbs_IN)
        return TopAbs_IN;
      if (c->State() == TopAbs_ON)
        best = TopAbs_ON;
    }
    return best;
  }

private:
  TopoDS_Shape m_shape;
  double m_tol;
  bool m_built = false;
  std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> m_solids;
};

// A closed shell the wrong way round. Per solid, exactly one shell encloses
// positive volume (the outer one) and every other (a void) negative. The
// shells' volumes sum to the total, which is already known, so the shell
// with the most faces — almost always the outer one, and the costliest to
// integrate — is inferred from the rest instead of integrated; the others
// go through CADShape::Volume, whose cache makes a void the boolean did not
// touch free. A single solid of a single shell is covered by the total's
// sign and costs nothing here.
std::string ShellDefect(const TopoDS_Shape& shape, double total, double tol) {
  struct Shell {
    TopoDS_Shape shape;
    int solid = 0;
    int faces = 0;
    double volume = 0.0;
  };
  std::vector<Shell> shells;
  int solids = 0;
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
    for (TopExp_Explorer sh(ex.Current(), TopAbs_SHELL); sh.More(); sh.Next()) {
      Shell s{sh.Current(), solids, 0, 0.0};
      for (TopExp_Explorer f(sh.Current(), TopAbs_FACE); f.More(); f.Next())
        s.faces++;
      shells.push_back(std::move(s));
    }
    solids++;
  }
  if (shells.size() <= 1)
    return {};
  const auto biggest = std::max_element(
      shells.begin(), shells.end(),
      [](const Shell& a, const Shell& b) { return a.faces < b.faces; });
  double rest = 0.0;
  for (auto it = shells.begin(); it != shells.end(); ++it) {
    if (it == biggest)
      continue;
    it->volume = CADShape(it->shape).Volume();
    rest += it->volume;
  }
  biggest->volume = total - rest;
  for (int s = 0; s < solids; s++) {
    int outward = 0;
    double inward = 0.0;
    for (const Shell& sh : shells) {
      if (sh.solid != s)
        continue;
      if (sh.volume > tol)
        outward++;
      else
        inward = std::min(inward, sh.volume);
    }
    if (outward == 0)
      return "a solid whose outer shell is oriented INWARD (it encloses " +
             Mm3(inward) + ")";
    if (outward > 1)
      return "a solid with a void oriented OUTWARD, adding material where "
             "there should be a hole";
  }
  return {};
}

bool AnyVertexIn(const TopoDS_Face& face, const Bnd_Box& box) {
  for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More(); v.Next())
    if (!box.IsOut(BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))))
      return true;
  return false;
}
} // namespace

std::string BooleanRefusal(const std::string& feature, BooleanOp op,
                           const std::string& body, const CADShape& target,
                           const CADShape& tool, const CADShape& result) {
  if (!result.IsValid())
    return {}; // a failed boolean is the caller's to report
  const double before = target.Volume();
  const double after = result.Volume();
  const std::string head = std::string(OpNoun(op)) + " '" + feature + "' ";
  const std::string volumes = " (body '" + body + "': " + Mm3(before) +
                              " before, " + Mm3(after) + " after)";
  const double scale = std::max(std::abs(before), std::abs(after));
  const double tol = kVolumeRel * scale;

  // 1. Volume, from the cached integrals (the callers need the result's
  // volume anyway, so this adds no integral of its own).
  if (after < -tol)
    return head + "turned body '" + body +
           "' INSIDE OUT: its volume is negative" + volumes +
           ". Nothing measured on a shape that encloses less than nothing "
           "would be right";
  if (op == BooleanOp::Subtract && after > before + tol)
    return head + "ADDED material to body '" + body + "'" + volumes +
           ": a cut cannot make a body larger, so the kernel got this cut "
           "wrong. A tool sharing a surface with the body is the usual "
           "cause: stop the tool at that surface, or move it clear";
  if (op == BooleanOp::Union && before > 1e-9 && after < before - tol)
    return "add LOST material from body '" + body + "' (" + Mm3(before) +
           " to " + Mm3(after) + ") in '" + feature +
           "': a union cannot shrink what it joins, so the kernel could "
           "not do this fuse; move the tool so it meets the body across a "
           "face rather than ending inside it";
  if (op == BooleanOp::Intersect) {
    const double toolVol = tool.Volume();
    if (after > std::min(before, toolVol) +
                    kVolumeRel * std::max(scale, std::abs(toolVol)))
      return head + "came out LARGER than " +
             (toolVol < before ? "the tool (" + Mm3(toolVol) + ")"
                               : std::string("the body")) +
             volumes +
             ": an intersection lies inside both, so the kernel got it "
             "wrong";
  }
  try {
    if (const std::string why = ShellDefect(result.GetShape(), after, tol);
        !why.empty())
      return head + "left body '" + body + "' with " + why + volumes;

    // 2. Where the faces are, judged against the TOOL: a sound solid, and
    // for a feature's cut a small one, so asking it is cheap where asking
    // the target (hundreds of faces) is not. A cut or a union keeps no
    // face strictly inside its tool, and an intersection none outside it.
    // Judged: every face the boolean made, and every face of the target it
    // kept whole that reaches into the tool's box — which is where the
    // dogfood's mistake was: the bores' floors kept inside the slot, and
    // "removed no material" from a slot that enclosed them.
    const Bnd_Box toolBox = BoxOf(tool);
    Bnd_Box both = BoxOf(target);
    both.Add(toolBox);
    const double diag = both.IsVoid() ? 1.0 : std::sqrt(both.SquareExtent());
    const double ptol = std::max(1e-6 * diag, 10.0 * Precision::Confusion());
    Bnd_Box reach = toolBox;
    reach.Enlarge(ptol);
    Where inTool(tool.GetShape(), ptol);
    Handle(IntTools_Context) ctx = new IntTools_Context();

    TopTools_IndexedMapOfShape fromTarget, fromTool;
    TopExp::MapShapes(target.GetShape(), TopAbs_FACE, fromTarget);
    TopExp::MapShapes(tool.GetShape(), TopAbs_FACE, fromTool);
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(result.GetShape(), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); i++) {
      const TopoDS_Face face = TopoDS::Face(faces(i));
      if (fromTool.Contains(face))
        continue; // the tool's own face, on the tool by definition
      const bool kept = fromTarget.Contains(face);
      if (kept && (op == BooleanOp::Intersect || !AnyVertexIn(face, reach)))
        continue;
      gp_Pnt p;
      gp_Pnt2d uv;
      if (BOPTools_AlgoTools3D::PointInFace(face, p, uv, ctx) != 0)
        continue; // no interior point to judge it by
      const TopAbs_State where = inTool.Of(p);
      std::string why;
      if (op == BooleanOp::Intersect && where == TopAbs_OUT)
        why = "a face at " + At(p) +
              " lies OUTSIDE the tool it was intersected with";
      else if (op != BooleanOp::Intersect && where == TopAbs_IN)
        why = std::string(kept ? "a face of the body" : "a face") + " at " +
              At(p) + " lies INSIDE the tool, where the " + OpNoun(op) +
              " should have " +
              (op == BooleanOp::Subtract ? "removed it" : "absorbed it");
      if (!why.empty())
        return head + "made body '" + body +
               "' WRONG though it passes BRepCheck: " + why + volumes +
               ". A tool that shares a surface with the body (a slot whose "
               "round end is a bore's own cylinder) is the usual cause: "
               "stop the tool at the face where the surfaces meet, or make "
               "it clearly larger or smaller than what it coincides with";
    }
  } catch (const Standard_Failure& e) {
    LOG_WARN("BooleanRefusal '{}': could not check the result: {}", feature,
             e.GetMessageString());
  }
  return {};
}

bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape,
                   ElementMap shapeMap, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId, std::string* warning, std::string* error) {
  switch (mode) {
  case ExtrudeMode::NewBody:
    bodies.push_back({name, std::move(shape), material, ownerId});
    bodies.back().elementMap = std::move(shapeMap);
    return true;

  case ExtrudeMode::Add: {
    // Fuse into the last body whose bbox overlaps the shape, else the last.
    // A reference body (a Derived ghost) is never a target: the first solid
    // drawn on a derived face becomes this part's own body, which is the
    // whole point of drawing there.
    glm::vec3 pmin, pmax;
    shape.GetBoundingBox(pmin, pmax);
    int target = -1;
    for (int i = static_cast<int>(bodies.size()) - 1; i >= 0; i--) {
      if (bodies[i].reference)
        continue;
      if (target < 0)
        target = i; // the last non-reference body, unless one overlaps
      glm::vec3 bmin, bmax;
      bodies[i].shape.GetBoundingBox(bmin, bmax);
      if (BoxesOverlap(pmin, pmax, bmin, bmax)) {
        target = i;
        break;
      }
    }
    if (target < 0) {
      if (!bodies.empty())
        LOG_INFO("ApplyBodyMode '{}': Add with only reference bodies, "
                 "creating one",
                 name);
      else
        LOG_WARN("ApplyBodyMode '{}': Add with no bodies, creating one", name);
      bodies.push_back({name, std::move(shape), material, ownerId});
      bodies.back().elementMap = std::move(shapeMap);
      return true;
    }
    ElementMap fusedMap;
    std::string boolErr;
    CADShape fused = topo::BooleanNamed(BooleanOp::Union, bodies[target].shape,
                                        bodies[target].elementMap, shape,
                                        shapeMap, ownerId, fusedMap, &boolErr);
    if (!fused.IsValid()) {
      Fail(error, "add failed to fuse with body '" + bodies[target].name + "'" +
                      (boolErr.empty() ? "" : ": " + boolErr));
      return false;
    }
    // A union can only ADD. OCC will occasionally hand back a shape that is
    // valid and smaller — an empty one, even — for a fuse it could not do,
    // and accepting it silently deletes the body: a swept tube whose end cap
    // lands inside the block it is being added to did exactly that, and the
    // manifold it belonged to came back as one flange with no log. That and
    // the rest of what a boolean must keep are BooleanRefusal's.
    if (std::string why =
            BooleanRefusal(name, BooleanOp::Union, bodies[target].name,
                           bodies[target].shape, shape, fused);
        !why.empty()) {
      Fail(error, std::move(why));
      return false;
    }
    const double before = bodies[target].shape.Volume();
    const double after = fused.Volume();
    if (warning && VolumeUnchanged(before, after)) {
      *warning = "add merged no new material — the tool lies entirely inside "
                 "the target body (check the sketch plane and profile)";
    }
    bodies[target].shape = std::move(fused);
    bodies[target].elementMap = std::move(fusedMap); // propagated (T5)
    return true;
  }

  case ExtrudeMode::Cut: {
    // Reference bodies are not matter: a cut passes through a Derived ghost
    // as if it were not there, and a part that has only ghosts has nothing
    // to cut into.
    const bool anySolid =
        std::any_of(bodies.begin(), bodies.end(),
                    [](const Body& b) { return !b.reference; });
    if (!anySolid) {
      Fail(error, bodies.empty()
                      ? "cut with no bodies — nothing exists yet to cut into "
                        "(did an earlier feature fail?)"
                      : "cut with only reference bodies — a derived ghost is "
                        "not cut into (set reference: false to make it this "
                        "part's body)");
      return false;
    }
    double volBefore = 0.0;
    for (const auto& body : bodies) {
      if (!body.reference)
        volBefore += body.shape.Volume();
    }
    // Every body's cut is built and CHECKED before any is kept: a refusal
    // leaves the bodies exactly as they were, which is what a broken feature
    // contributing nothing means.
    std::vector<std::pair<CADShape, ElementMap>> cuts(bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
      Body& body = bodies[i];
      if (body.reference)
        continue;
      ElementMap cutMap;
      CADShape cut =
          topo::BooleanNamed(BooleanOp::Subtract, body.shape, body.elementMap,
                             shape, shapeMap, ownerId, cutMap);
      if (!cut.IsValid())
        continue;
      if (std::string why = BooleanRefusal(name, BooleanOp::Subtract, body.name,
                                           body.shape, shape, cut);
          !why.empty()) {
        Fail(error, std::move(why));
        return false;
      }
      cuts[i] = {std::move(cut), std::move(cutMap)};
    }
    for (size_t i = 0; i < bodies.size(); i++) {
      if (!cuts[i].first.IsValid())
        continue;
      bodies[i].shape = std::move(cuts[i].first);
      bodies[i].elementMap = std::move(cuts[i].second); // propagated (T5)
    }
    // Drop bodies the cut fully consumed.
    bodies.erase(std::remove_if(bodies.begin(), bodies.end(),
                                [](const Body& b) {
                                  return !b.shape.IsValid() ||
                                         b.shape.Volume() < 1e-9;
                                }),
                 bodies.end());
    double volAfter = 0.0;
    for (const auto& body : bodies) {
      if (!body.reference)
        volAfter += body.shape.Volume();
    }
    if (warning && volBefore > 1e-9 && volAfter < 1e-9) {
      // EVERYTHING went. A cut may legitimately consume one body of several
      // -- that is what the erase above is for -- but a cut that leaves the
      // part with nothing is either a tool the wrong way round or a kernel
      // boolean that failed and returned an empty solid, and neither said a
      // word before this. Downstream you get "no bodies" from
      // mass_properties with no clue which feature did it, which cost two
      // rounds of bisecting a twelve-groove helix chain by hand
      // (docs/modeling/README.md#features).
      *warning = "this cut consumed EVERY body — nothing is left of the "
                 "part. A tool that swallows the whole model is usually one "
                 "the wrong way round, or a boolean the kernel could not do";
    } else if (warning && VolumeUnchanged(volBefore, volAfter)) {
      *warning = "cut removed no material — the tool profile does not "
                 "intersect any body (check the sketch plane orientation and "
                 "position)";
    }
    return true;
  }
  }
  return false;
}

bool ApplyBodyModeReporting(std::vector<Body>& bodies, CADShape shape,
                            ElementMap shapeMap, ExtrudeMode mode,
                            Feature& owner) {
  std::string warning, error;
  const bool ok = ApplyBodyMode(bodies, std::move(shape), std::move(shapeMap),
                                mode, owner.GetName(), owner.GetMaterial(),
                                owner.GetId(), &warning, &error);
  if (!warning.empty()) {
    owner.SetWarning(warning);
  }
  if (!ok) {
    owner.SetError(error.empty() ? "body operation failed" : error);
  }
  return ok;
}

bool FuseInstancesReplacingSource(
    std::vector<Body>& bodies, uint64_t sourceId,
    std::vector<std::pair<CADShape, ElementMap>> instances,
    const std::string& name, const std::string& material, uint64_t ownerId,
    std::string* error) {
  if (instances.empty()) {
    Fail(error, "pattern produced no instances");
    return false;
  }
  const bool hasSource =
      std::any_of(bodies.begin(), bodies.end(),
                  [&](const Body& b) { return b.createdBy == sourceId; });
  if (!hasSource) {
    Fail(error, "source body (feature " + std::to_string(sourceId) +
                    ") not found — did the source feature fail?");
    return false;
  }

  // Fuse all instances, then split back into connected solids. The union runs
  // through the naming boolean so each instance's names survive it: a face of
  // the fourth bar of a grate is then as referenceable as one of the first,
  // which is what makes a patterned body pickable at all.
  CADShape fused = std::move(instances[0].first);
  ElementMap fusedMap = std::move(instances[0].second);
  for (size_t i = 1; i < instances.size(); i++) {
    ElementMap out;
    CADShape next = topo::BooleanNamed(BooleanOp::Union, fused, fusedMap,
                                       instances[i].first, instances[i].second,
                                       ownerId, out, nullptr);
    if (!next.IsValid()) {
      Fail(error,
           "fusing pattern instance " + std::to_string(i + 1) + " failed");
      return false;
    }
    fused = std::move(next);
    fusedMap = std::move(out);
  }
  std::vector<CADShape> solids = fused.SplitSolids();

  // Replace the seed body(ies) with the pattern's solids.
  bodies.erase(
      std::remove_if(bodies.begin(), bodies.end(),
                     [&](const Body& b) { return b.createdBy == sourceId; }),
      bodies.end());
  for (size_t i = 0; i < solids.size(); i++) {
    std::string bodyName =
        solids.size() > 1 ? name + " " + std::to_string(i + 1) : name;
    // The pieces of the split share their faces with the shape they came out
    // of, so each one's names are a lookup, not a second history pass.
    ElementMap pieceMap;
    topo::NamesOfSubShape(fused, fusedMap, solids[i], pieceMap);
    bodies.push_back(
        {std::move(bodyName), std::move(solids[i]), material, ownerId});
    bodies.back().elementMap = std::move(pieceMap);
  }
  return true;
}
