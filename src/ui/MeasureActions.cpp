#include "ui/AppActions.h"

#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <chrono>
#include <utility>

#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/ElementHistory.h" // topo::ResolveEdgeOnBody (blend edge picks)
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"

#include "core/features/DraftFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>

// What a measurement MEANS: resolving what was picked, computing the distance
// or the angle, naming it, pinning it, and keeping pins fresh across edits.
//
// MeasureOverlay.cpp is the other half -- what a measurement LOOKS like. Both
// read the same state.measure.dimension, so the picture and the readout are
// one computation and cannot disagree; the split is meaning vs. appearance.
// Split out of AppActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 3). AppActions.h still declares everything, so no caller changed.

namespace ui {

namespace {
// Below this separation two elements are touching, not "a very small
// distance apart", and the informative number is the angle between them
// rather than the zero. Same 1 um the contact checks use to tell a designed
// touch from a sliver-sized clash (Measure.cpp's kContactFuzz).
constexpr double kTouchingMm = 1.0e-3;

// A measured element as a one-line readout, in the DOCUMENT's units — this
// printed "%.3f mm" regardless, so a part authored in inches measured in
// millimetres, and three fixed decimals claimed precision a tessellated
// solid does not have. units::Format* applies the significant-figures and
// thousands-separator rules the feature tree and the BOM already use.
std::string FormatElement(const Measure::Element& e, const units::System& u) {
  std::string out;
  if (e.kind == "face") {
    out = "Face area: " + units::FormatArea(e.area, u);
  } else {
    out = "Edge length: " + units::FormatLength(e.length, u);
  }
  // A radius wherever the surface or curve defines one — including a
  // cylindrical FACE, where a bore used to report only its surface area.
  // Diameter first: it is the number on the drawing.
  if (e.hasRadius) {
    out += "   \xc3\x98" + units::FormatLength(e.radius * 2.0, u) + " (R" +
           units::FormatLength(e.radius, u) + ")";
  }
  return out;
}
} // namespace

namespace {
// The bodies + rigid world transform (+ a display prefix) one measured
// element resolves against: the active part's bodies at identity, or — on an
// assembly tab — the occurrence's part bodies at the SOLVED occurrence
// transform (real geometry, not the exploded display offset).
struct MeasureSource {
  const std::vector<Body>* bodies = nullptr;
  glm::mat4 xf{1.0f};
  std::string prefix; // "OccurrenceName: " on assemblies, "" on parts
};
// An OCCURRENCE measures as every leaf body under it, already placed in
// world space — the shape assembly::Flatten produces and contact_report
// already uses, so a sub-assembly measures as the one thing it looks like
// rather than as its parts.
std::optional<MeasureSource> ResolveOccurrenceBodies(AppState& state,
                                                     uint64_t occurrenceId,
                                                     std::vector<Body>& store) {
  if (!state.assembly)
    return std::nullopt;
  const Occurrence* occ = state.assembly->FindById(occurrenceId);
  if (!occ)
    return std::nullopt;
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (inst.rootOccurrenceId != occurrenceId || !inst.visible)
      continue;
    for (const Body& b : inst.part->GetBodies()) {
      Body nb = b;
      nb.shape = b.shape.Transformed(inst.world);
      store.push_back(std::move(nb));
    }
  }
  if (store.empty())
    return std::nullopt;
  // Already in world space, so the transform is identity.
  return MeasureSource{&store, glm::mat4(1.0f), occ->name + ": "};
}

std::optional<MeasureSource>
ResolveMeasureSource(AppState& state, const MeasureState::Element& e) {
  if (e.occurrenceId == 0) {
    if (state.document)
      return MeasureSource{&state.document->GetBodies(), glm::mat4(1.0f), ""};
    // A JOB tab: its parts, laid out -- which is what the viewport draws, so
    // no transform. The tool was greyed out here for want of this branch,
    // while the pick pipeline underneath it already knew the job's bodies.
    if (state.job)
      return MeasureSource{&JobPlacedBodies(state), glm::mat4(1.0f), ""};
    return std::nullopt;
  }
  if (!state.assembly)
    return std::nullopt;
  const Occurrence* occ = state.assembly->FindById(e.occurrenceId);
  if (!occ)
    return std::nullopt;
  Document* part = PartDocById(state, occ->partId);
  if (!part)
    return std::nullopt;
  return MeasureSource{&part->GetBodies(), OccurrenceMatrix(*occ),
                       occ->name + ": "};
}
} // namespace

namespace {
// Measure one or two elements into a readout line and a drawable dimension.
//
// The committed pair and the hover preview are the SAME measurement, so they
// share this: a preview computed by any other path would be a preview of
// something slightly else, which is worse than no preview.
struct MeasureOut {
  // What was measured, in words ("Board - top face"). Kept apart from the
  // readout because the tool window already NAMES each pick in its rows —
  // repeating it under them said "top face / top face" — while the hover
  // pill has no rows and needs the name and the number together.
  std::string name;
  std::string readout;
  MeasureState::Dimension dimension;
  bool hasLine = false;
  glm::vec3 lineA{0.0f}, lineB{0.0f};
};

// Everything needed to measure one pick, with the world-placed bodies of an
// occurrence kept alive alongside the Target that points at them.
struct Resolved {
  std::vector<Body> owned; // occurrence picks only
  Measure::Target target;
  std::string prefix;
  bool ok = false;
};

Resolved ResolveTargetFor(AppState& state, const MeasureState::Element& e) {
  Resolved r;
  if (e.kind == MeasureState::Element::Kind::Occurrence) {
    const auto src = ResolveOccurrenceBodies(state, e.occurrenceId, r.owned);
    if (!src)
      return r;
    r.target.bodies = &r.owned;
    r.target.xf = glm::mat4(1.0f);
    r.target.ref = ElementRef{0, ""}; // whole set
    r.prefix = src->prefix;
    r.ok = true;
    return r;
  }
  const auto src = ResolveMeasureSource(state, e);
  if (!src)
    return r;
  r.target.bodies = src->bodies;
  r.target.xf = src->xf;
  r.prefix = src->prefix;
  // A job pick is measured against ITS part's bodies only, the way an
  // occurrence pick is: the ref names a face on every part that happens to
  // share the feature id and the name (v76), and the first to answer is
  // not necessarily the one that was clicked.
  if (state.job && e.occurrenceId == 0 && e.jobPartId != 0) {
    for (const Body& b : *src->bodies)
      if (b.jobPartId == e.jobPartId)
        r.owned.push_back(b);
    if (r.owned.empty())
      return r;
    r.target.bodies = &r.owned;
    if (state.job->PartCount() > 1)
      if (const cam::JobPart* part = state.job->FindPart(e.jobPartId))
        r.prefix = part->name + ": ";
  }
  // A BODY pick filters the set to the one body that feature created; a
  // face/edge names the element.
  r.target.ref = e.kind == MeasureState::Element::Kind::Body
                     ? ElementRef{e.ref.featureId, ""}
                     : e.ref;
  r.prefix = src->prefix;
  r.ok = true;
  return r;
}

// The name of a whole part/body, for the readout.
std::string WholeName(AppState& state, const MeasureState::Element& e) {
  if (e.kind == MeasureState::Element::Kind::Occurrence) {
    if (state.assembly)
      if (const Occurrence* occ = state.assembly->FindById(e.occurrenceId))
        return occ->name;
    return "occurrence";
  }
  if (state.document)
    for (const Body& b : state.document->GetBodies())
      if (b.createdBy == e.ref.featureId && !b.name.empty())
        return b.name;
  if (state.job)
    for (const Body& b : JobPlacedBodies(state))
      if (b.createdBy == e.ref.featureId && !b.name.empty() &&
          (e.jobPartId == 0 || b.jobPartId == e.jobPartId))
        return b.name;
  return "body";
}

// The world-space point a CORNER measurable resolves to, if it still does.
std::optional<glm::vec3> CornerWorldPoint(AppState& state,
                                          const MeasureState::Element& e) {
  const auto src = ResolveMeasureSource(state, e);
  if (!src)
    return std::nullopt;
  const auto p = Measure::CornerOf(*src->bodies, e.ref, e.ref2);
  if (!p)
    return std::nullopt;
  return glm::vec3(src->xf * glm::vec4(*p, 1.0f));
}

MeasureOut MeasureOne(AppState& state, const MeasureState::Element& e,
                      const units::System& u) {
  MeasureOut out;
  if (e.IsCorner()) {
    // A corner IS a coordinate — the one thing no other measurable could
    // report, and the reason point picking was wanted at all.
    const auto p = CornerWorldPoint(state, e);
    if (!p) {
      out.readout = "Those two edges no longer meet at one point.";
      return out;
    }
    out.name = "corner";
    out.readout = "At  x " + units::FormatLength(p->x, u) + "   y " +
                  units::FormatLength(p->y, u) + "   z " +
                  units::FormatLength(p->z, u);
    return out;
  }
  if (e.Whole()) {
    // A whole part reports what a part HAS: what it weighs out of, what it
    // takes to coat, and the box it needs to ship in.
    const Resolved r = ResolveTargetFor(state, e);
    const auto d = r.ok ? Measure::DescribeTarget(r.target) : std::nullopt;
    if (!d) {
      out.readout = "Could not measure that part.";
      return out;
    }
    out.name = WholeName(state, e);
    out.readout = "Volume: " + units::FormatVolume(d->volume, u) +
                  "\nSurface: " + units::FormatArea(d->surfaceArea, u) +
                  "\nBounding box: " + units::FormatLength(d->size.x, u) +
                  " x " + units::FormatLength(d->size.y, u) + " x " +
                  units::FormatLength(d->size.z, u);
    if (d->solids > 1)
      out.readout += "\n" + std::to_string(d->solids) + " separate solids";
    return out;
  }
  const auto src = ResolveMeasureSource(state, e);
  if (src) {
    if (const auto d = Measure::Describe(*src->bodies, e.ref)) {
      out.name = Measure::HumanName(*src->bodies, e.ref, u);
      out.readout = src->prefix + FormatElement(*d, u);
      return out;
    }
  }
  out.readout = "Could not measure that element.";
  return out;
}

// Distance when at least one side is a CORNER. A corner is a point, so its
// side of the measurement is exact: the nearest point on the other element to
// it, or — corner to corner — simply the gap between them. Every distance
// kind agrees, because a point has no near and far end.
std::optional<Measure::DistanceResult>
CornerPairDistance(AppState& state, const MeasureState::Element& ea,
                   const MeasureState::Element& eb,
                   Measure::DistanceKind kind) {
  const MeasureState::Element& corner = ea.IsCorner() ? ea : eb;
  const MeasureState::Element& other = ea.IsCorner() ? eb : ea;
  const auto cp = CornerWorldPoint(state, corner);
  if (!cp)
    return std::nullopt;
  Measure::DistanceResult out;
  if (other.IsCorner()) {
    const auto op = CornerWorldPoint(state, other);
    if (!op)
      return std::nullopt;
    out.pointA = ea.IsCorner() ? *cp : *op;
    out.pointB = ea.IsCorner() ? *op : *cp;
    out.distance = glm::distance(*cp, *op);
    return out;
  }
  // Point to element: measure against a zero-length "edge" at the corner by
  // asking the other side for its nearest point. CentreOf/AnchorOf would
  // answer a different question, so this goes through the extrema solver
  // with the corner as one degenerate side.
  const Resolved ro = ResolveTargetFor(state, other);
  if (!ro.ok)
    return std::nullopt;
  const auto nearest = Measure::NearestPointOn(ro.target, *cp, kind);
  if (!nearest)
    return std::nullopt;
  out.pointA = ea.IsCorner() ? *cp : *nearest;
  out.pointB = ea.IsCorner() ? *nearest : *cp;
  out.distance = glm::distance(*cp, *nearest);
  return out;
}

MeasureOut MeasurePair(AppState& state, const MeasureState::Element& ea,
                       const MeasureState::Element& eb,
                       const units::System& u) {
  MeasureOut out;
  const Resolved ra = ResolveTargetFor(state, ea);
  const Resolved rb = ResolveTargetFor(state, eb);
  if (!ra.ok || !rb.ok) {
    out.readout = "Could not measure between those two.";
    return out;
  }
  const Measure::DistanceKind kind = state.measure.distanceKind;
  // A CORNER is a single point, so every distance kind collapses to the same
  // exact answer and no extrema solve is needed. Mixed pairs (a corner to a
  // face) fall through to the general path with the corner as a degenerate
  // target — handled below.
  std::optional<Measure::DistanceResult> d;
  if (ea.IsCorner() || eb.IsCorner()) {
    d = CornerPairDistance(state, ea, eb, kind);
    if (!d) {
      out.readout = "Could not measure from that corner.";
      return out;
    }
  } else {
    d = Measure::DistanceBetween(ra.target, rb.target, kind);
  }
  // An angle needs a DIRECTION. A whole part has none — a bracket points
  // every way at once — and neither does a POINT. A corner carries its first
  // edge in `ref`, so without this the angle would be taken between the two
  // corners' edges and, worse, re-anchor the dimension onto them: two
  // diagonal corners of a face read as the 2 mm between their edges instead
  // of the 2.83 mm between the points that were picked.
  const bool pointInvolved = ea.IsCorner() || eb.IsCorner();
  const auto ang =
      (ea.Whole() || eb.Whole() || pointInvolved)
          ? std::optional<Measure::AngleResult>{}
          : Measure::Angle(*ra.target.bodies, ea.ref, ra.target.xf,
                           *rb.target.bodies, eb.ref, rb.target.xf);
  // Two whole shapes that interpenetrate are 0 apart, which is true and the
  // least useful available fact. Ask whether they actually overlap.
  const auto overlap = (ea.Whole() && eb.Whole())
                           ? Measure::OverlapBetween(ra.target, rb.target)
                           : std::optional<Measure::SetContact>{};
  if (!d && !ang) {
    out.readout = "Could not measure between those two elements.";
    return out;
  }
  if (d) {
    out.hasLine = true;
    out.lineA = d->pointA;
    out.lineB = d->pointB;
  }

  // BOTH quantities are computed; the readout leads with whichever one is
  // not degenerate. This is the fix for the report that started this work:
  // two edges meeting at a corner are 0.000 mm apart, which is true and
  // useless, and 90 degrees apart, which is the answer. On a single solid
  // most element pairs touch, so distance-only made zero the tool's most
  // common output.
  //
  // - touching  -> the angle leads, and the distance says "touching"
  // - parallel  -> the distance leads, LABELLED parallel: for parallel
  //                elements the minimum separation is the separation,
  //                everywhere, rather than "the nearest of many"
  // - otherwise -> distance, then angle
  const bool clashing = overlap && overlap->volume > 0.0;
  const bool touching = d && d->distance <= kTouchingMm && !clashing;
  const bool parallel = ang && ang->parallel;
  std::string distLine;
  if (clashing) {
    // Lead with the clash. "0 mm apart" describes a clash and a touch
    // identically, and they are not the same thing at all.
    out.readout =
        "CLASH: overlapping by " + units::FormatVolume(overlap->volume, u);
    out.hasLine = false;
    return out;
  }
  if (d) {
    // Say WHICH distance this is, always. "45 mm" is three different facts
    // depending on the qualifier, and the tool used to print all three the
    // same way.
    const char* qualifier =
        kind == Measure::DistanceKind::Maximum   ? " (maximum)"
        : kind == Measure::DistanceKind::Centres ? " (centre to centre)"
        : touching                               ? " (touching)"
        : parallel                               ? " (parallel)"
                                                 : " (nearest)";
    distLine = "Distance: " + units::FormatLength(d->distance, u) + qualifier;
    // The axis breakdown only where there is one to break down: on a
    // touching pair it is three zeros, which is a line of text that says
    // nothing. Signed, not absolute — "40 mm in X" and "-40 mm in X" are a
    // part and its mirror image, and std::abs threw away which.
    if (!touching) {
      const glm::vec3 delta = d->pointB - d->pointA;
      distLine += "\ndx " + units::FormatLength(delta.x, u) + "  dy " +
                  units::FormatLength(delta.y, u) + "  dz " +
                  units::FormatLength(delta.z, u);
    }
  }
  std::string angLine;
  if (ang) {
    angLine =
        "Angle: " + units::FormatAngle(ang->degrees, u) + "  " + ang->basis;
    if (ang->parallel)
      angLine += " - parallel";
    else if (ang->perpendicular)
      angLine += " - perpendicular";
  } else if (touching) {
    // Leading with an angle we could not take would be silence about the
    // only interesting thing here, so say why there is none — and say the
    // REASON that applies, not a list of every reason there could be.
    angLine = pointInvolved ? "No angle: a corner is a point, and a point "
                              "has no direction."
              : (ea.Whole() || eb.Whole())
                  ? "No angle: a whole part has no single direction."
                  : "No angle: one of these defines no direction (a sphere "
                    "or a spline has none).";
  }

  if (touching && !angLine.empty())
    out.readout = angLine + (distLine.empty() ? "" : "\n" + distLine);
  else if (angLine.empty())
    out.readout = distLine;
  else if (distLine.empty())
    out.readout = angLine;
  else
    out.readout = distLine + "\n" + angLine;

  // The picture, from the same numbers as the text.
  if (!d)
    return out;
  MeasureState::Dimension& dim = out.dimension;
  const bool wholeInvolved = ea.Whole() || eb.Whole() || pointInvolved;
  // Where to ATTACH the dimension, which is not always where the minimum
  // was measured: for a parallel pair every point pair is equidistant, so
  // OCC's answer is an arbitrary corner.
  // Re-anchoring is a MINIMUM-only correction. DimensionAnchors resolves the
  // extrema itself, which for any other kind would draw the dimension at the
  // MINIMUM points while the readout reported the maximum — the picture and
  // the number describing two different pairs of points. Maximum and centres
  // already name the points they mean.
  const std::optional<Measure::DistanceResult> anchors =
      (parallel && kind == Measure::DistanceKind::Minimum && !wholeInvolved)
          ? Measure::DimensionAnchors(*ra.target.bodies, ea.ref, ra.target.xf,
                                      *rb.target.bodies, eb.ref, rb.target.xf,
                                      true)
          : std::nullopt;
  const Measure::DistanceResult& at = anchors ? *anchors : *d;
  dim.valid = !touching; // a zero-length dimension is not a picture
  dim.a = at.pointA;
  dim.b = at.pointB;
  dim.label = units::FormatLength(at.distance, u);
  // Witness lines back to where the extrema actually landed, so a
  // re-anchored dimension still says which two things it spans.
  dim.hasWitness = anchors && (glm::distance(at.pointA, d->pointA) > 1e-4f ||
                               glm::distance(at.pointB, d->pointB) > 1e-4f);
  dim.witnessA = d->pointA;
  dim.witnessB = d->pointB;

  // The angle arc, drawn between the directions the angle was TAKEN
  // between — so the picture and the stated basis are the same thing.
  // Only where it reads: the elements have to meet somewhere for an arc
  // about that point to mean anything.
  if (ang && !ang->parallel && touching && !wholeInvolved) {
    const auto da = Measure::DirectionOf(*ra.target.bodies, ea.ref);
    const auto db = Measure::DirectionOf(*rb.target.bodies, eb.ref);
    const auto anchorA = Measure::AnchorOf(*ra.target.bodies, ea.ref);
    const auto anchorB = Measure::AnchorOf(*rb.target.bodies, eb.ref);
    if (da && db && anchorA && anchorB) {
      const glm::vec3 at3 = d->pointA; // the contact point
      // An unsigned direction (a line edge) gets the sign that points
      // along the element and away from the contact, so the ray lies on
      // the edge you can see rather than off into space.
      const auto orient = [&](const Measure::Direction& dir,
                              const glm::vec3& anchor, const glm::mat4& xf) {
        glm::vec3 v =
            glm::normalize(glm::vec3(glm::mat3(xf) * glm::vec3(dir.dir)));
        if (!dir.oriented && glm::dot(v, anchor - at3) < 0.0f)
          v = -v;
        return v;
      };
      dim.arcAt = at3;
      dim.arcFrom =
          orient(*da, glm::vec3(ra.target.xf * glm::vec4(*anchorA, 1.0f)),
                 ra.target.xf);
      dim.arcTo =
          orient(*db, glm::vec3(rb.target.xf * glm::vec4(*anchorB, 1.0f)),
                 rb.target.xf);
      dim.arcLabel = units::FormatAngle(ang->degrees, u);
      dim.hasArc = true;
    }
  }
  return out;
}

// Everything a cached hover measurement depends on. Recomputing an OCC
// extrema call sixty times a second for a cursor that has not moved is pure
// waste; recomputing it when the model changes under a still cursor is
// required.
std::string HoverCacheKey(AppState& state) {
  const MeasureState& m = state.measure;
  std::string key = std::to_string(m.hover.element.ref.featureId) + "/" +
                    m.hover.element.ref.name + "#" +
                    std::to_string(m.hover.element.occurrenceId) + "/" +
                    std::to_string(m.hover.element.jobPartId);
  if (!m.picked.empty()) {
    key += "|" + std::to_string(m.picked[0].ref.featureId) + "/" +
           m.picked[0].ref.name + "#" +
           std::to_string(m.picked[0].occurrenceId) + "/" +
           std::to_string(m.picked[0].jobPartId);
  }
  key += "@" + std::to_string(m.picked.size()) + "@" +
         std::to_string(state.document ? state.document->Version() : 0) + "@" +
         std::to_string(state.assembly ? state.assembly->Version() : 0) + "@" +
         // A job's geometry moves when any part does AND when a part is
         // re-clamped or re-placed; JobRenderVersion keys on all of it.
         std::to_string(state.job ? JobRenderVersion(state) : 0);
  return key;
}
} // namespace

void UpdateMeasureReadout(AppState& state) {
  auto& m = state.measure;
  m.hasDistanceLine = false;
  m.dimension = MeasureState::Dimension{};
  if ((!state.document && !state.assembly && !state.job) || m.picked.empty()) {
    m.readout.clear();
    return;
  }
  const units::System u = state.ActiveUnits();
  if (m.picked.size() == 1) {
    m.readout = MeasureOne(state, m.picked[0], u).readout;
    return;
  }
  if (m.picked.size() == 2) {
    const MeasureOut r = MeasurePair(state, m.picked[0], m.picked[1], u);
    m.readout = r.readout;
    m.dimension = r.dimension;
    m.hasDistanceLine = r.hasLine;
    m.distA = r.lineA;
    m.distB = r.lineB;
    return;
  }
  // Three or more: the total. This used to read "3 elements selected - keep
  // two for a distance", which is a refusal with no reason and no
  // alternative — and weld length, cut length and coating area are all real
  // questions that a pair cannot answer.
  std::vector<ElementRef> refs;
  const std::vector<Body>* bodies = nullptr;
  bool oneSource = true;
  for (const MeasureState::Element& e : m.picked) {
    const auto src = ResolveMeasureSource(state, e);
    if (!src) {
      m.readout = "Could not measure one of those elements.";
      return;
    }
    if (bodies && bodies != src->bodies)
      oneSource = false;
    bodies = src->bodies;
    refs.push_back(e.ref);
  }
  if (!oneSource) {
    m.readout = std::to_string(m.picked.size()) +
                " elements across different parts - a total is per part; "
                "keep two for a distance between parts.";
    return;
  }
  const auto total = bodies ? Measure::TotalOf(*bodies, refs) : std::nullopt;
  if (!total) {
    // The mixed case is the one worth naming: an area plus a length is not a
    // quantity, so there is nothing to print rather than something wrong.
    m.readout = std::to_string(m.picked.size()) +
                " elements selected - faces and edges do not add up "
                "(an area plus a length is not a quantity). Keep one kind, "
                "or two elements for a distance.";
    return;
  }
  m.readout = total->kind == "face"
                  ? "Total area of " + std::to_string(total->count) +
                        " faces: " + units::FormatArea(total->total, u)
                  : "Total length of " + std::to_string(total->count) +
                        " edges: " + units::FormatLength(total->total, u);
}

void UpdateMeasureHover(AppState& state) {
  auto& h = state.measure.hover;
  if (!state.measure.active || !h.has || state.measure.picked.size() >= 2 ||
      (!state.document && !state.assembly && !state.job)) {
    h.readout.clear();
    h.dimension = MeasureState::Dimension{};
    h.cacheKey.clear();
    return;
  }
  // Hovering something already picked would preview measuring it against
  // itself, which is a zero nobody asked for.
  for (const MeasureState::Element& p : state.measure.picked) {
    if (p.ref.featureId == h.element.ref.featureId &&
        p.ref.name == h.element.ref.name &&
        p.occurrenceId == h.element.occurrenceId &&
        p.jobPartId == h.element.jobPartId) {
      h.readout.clear();
      h.dimension = MeasureState::Dimension{};
      h.cacheKey.clear();
      return;
    }
  }
  const std::string key = HoverCacheKey(state);
  if (key == h.cacheKey)
    return; // same element, same anchor, same document — same answer
  h.cacheKey = key;
  const units::System u = state.ActiveUnits();
  const MeasureOut r =
      state.measure.picked.empty()
          ? MeasureOne(state, h.element, u)
          : MeasurePair(state, state.measure.picked[0], h.element, u);
  // The hover pill has no row above it to say what this is, so it carries
  // the name itself — which is the whole "what is this" gesture when nothing
  // is picked yet.
  h.readout = r.name.empty() ? r.readout : r.name + "\n" + r.readout;
  h.dimension = r.dimension;
}

std::string MeasureElementLabel(AppState& state,
                                const MeasureState::Element& e) {
  if (e.Whole()) {
    // A whole part is named by its own name — there is no sub-element to
    // describe, and "Bracket" is already what a person calls it.
    return WholeName(state, e) +
           (e.kind == MeasureState::Element::Kind::Occurrence ? " (part)"
                                                              : " (body)");
  }
  if (e.IsCorner()) {
    std::string where;
    if (e.occurrenceId != 0 && state.assembly)
      if (const Occurrence* occ = state.assembly->FindById(e.occurrenceId))
        where = occ->name + " - ";
    return where + "corner";
  }
  std::string out;
  if (e.occurrenceId != 0 && state.assembly)
    if (const Occurrence* occ = state.assembly->FindById(e.occurrenceId))
      out = occ->name + " - ";
  const auto src = ResolveMeasureSource(state, e);
  std::string what;
  if (src)
    what = Measure::HumanName(*src->bodies, e.ref, state.ActiveUnits());
  if (what.empty()) {
    // A ref that no longer resolves has no shape to name. Say so plainly
    // rather than falling back to the toponame, which would look like an
    // ordinary row while meaning "this is gone".
    return out + (e.IsEdge() ? "edge" : "face") + " (no longer in the model)";
  }
  // The BODY it belongs to, on a part tab where there is no occurrence name
  // to carry that: "Board - top face" beats "top face" the moment a part has
  // two bodies.
  if (e.occurrenceId == 0 && src) {
    for (const Body& b : *src->bodies) {
      if (b.createdBy == e.ref.featureId && !b.name.empty()) {
        out = b.name + " - ";
        break;
      }
    }
  }
  return out + what;
}

void SeedMeasureFromSelection(AppState& state) {
  if (state.selection.Empty())
    return;
  for (const SelectionItem& it : state.selection.items) {
    const uint64_t occ = state.assembly ? it.id : 0;
    switch (it.kind) {
    case SelectionItem::Kind::Face:
      state.measure.picked.push_back(
          MeasureState::Element::OfFace(it.element, occ));
      break;
    case SelectionItem::Kind::Edge:
      state.measure.picked.push_back(
          MeasureState::Element::OfEdge(it.element, occ));
      break;
    case SelectionItem::Kind::Body:
      // Selecting a body in the feature tree and then measuring is how you
      // pick a WHOLE body on a part tab, where a viewport click lands on
      // whichever face is under the cursor.
      state.measure.picked.push_back(MeasureState::Element::OfBody(it.id));
      break;
    case SelectionItem::Kind::Occurrence:
      state.measure.picked.push_back(
          MeasureState::Element::OfOccurrence(it.id));
      break;
    default:
      break; // features, mates, sketch regions are not measurable
    }
  }
  UpdateMeasureReadout(state);
}

namespace {
// The measurement's headline NUMBER, in millimetres (or degrees), separate
// from the sentence around it. The readout is prose — "Distance: 45 mm
// (parallel)" — and a clipboard or an expression wants the value.
std::optional<double> HeadlineValue(const AppState& state, bool* isAngle) {
  const MeasureState& m = state.measure;
  if (m.picked.size() != 2)
    return std::nullopt;
  const Resolved ra =
      ResolveTargetFor(const_cast<AppState&>(state), m.picked[0]);
  const Resolved rb =
      ResolveTargetFor(const_cast<AppState&>(state), m.picked[1]);
  if (!ra.ok || !rb.ok)
    return std::nullopt;
  const auto d = Measure::DistanceBetween(ra.target, rb.target, m.distanceKind);
  // Touching pairs lead with the angle, and so does the value they hand out
  // — the same rule the readout follows, so what you copy is what you read.
  if (d && d->distance > kTouchingMm) {
    if (isAngle)
      *isAngle = false;
    return d->distance;
  }
  if (!m.picked[0].Whole() && !m.picked[1].Whole()) {
    if (const auto ang =
            Measure::Angle(*ra.target.bodies, m.picked[0].ref, ra.target.xf,
                           *rb.target.bodies, m.picked[1].ref, rb.target.xf)) {
      if (isAngle)
        *isAngle = true;
      return ang->degrees;
    }
  }
  if (d) {
    if (isAngle)
      *isAngle = false;
    return d->distance;
  }
  return std::nullopt;
}
} // namespace

std::string MeasuredValueForClipboard(const AppState& state) {
  bool isAngle = false;
  const auto v = HeadlineValue(state, &isAngle);
  if (!v)
    return {};
  const units::System u = state.ActiveUnits();
  const double shown = isAngle ? (u.angle == units::Angle::Radian
                                      ? *v * 3.14159265358979323846 / 180.0
                                      : *v)
                               : *v / units::MmPerLength(u.length);
  // PLAIN, not units::FormatNumber: that groups thousands with a space for
  // readability ("10 080 000"), and this string is pasted into a dimension
  // field, which parses it as an EXPRESSION. A readable number that will not
  // parse is worse than an unreadable one that will.
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", shown);
  return buf;
}

bool CreateVariableFromMeasurement(AppState& state, const std::string& name,
                                   std::string* why) {
  const auto fail = [&](const char* reason) {
    if (why)
      *why = reason;
    return false;
  };
  if (name.empty())
    return fail("A variable needs a name.");
  if (!state.document)
    return fail("Variables live on a part document; this tab has none.");
  bool isAngle = false;
  const auto v = HeadlineValue(state, &isAngle);
  if (!v)
    return fail("Measure two things first — there is no value to bind yet.");
  // STORAGE units, like every other expression in the document: millimetres
  // and degrees. A variable written in the reader's display unit would
  // change meaning when the document's units did.
  //
  // And plain, for the same reason the clipboard value is: an expression
  // with a thousands separator in it does not parse.
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", *v);
  if (!SetProjectVariable(state, name, buf,
                          isAngle ? "measured angle (degrees)"
                                  : "measured distance (mm)")) {
    // SetProjectVariable refuses an unusable name; passing that on beats
    // reporting success over a variable that was never created.
    return fail("That is not a usable variable name (letters, digits and "
                "underscores; not starting with a digit).");
  }
  return true;
}

void PinMeasurement(AppState& state) {
  MeasureState& m = state.measure;
  if (m.picked.empty() || m.readout.empty())
    return;
  MeasureState::Pinned p;
  p.id = m.nextPinnedId++;
  p.elements = m.picked;
  p.readout = m.readout;
  p.dimension = m.dimension;
  p.kind = m.distanceKind;
  m.pinned.push_back(std::move(p));
  // Freshly computed against this version, so the next frame's refresh has
  // nothing to do.
  m.pinnedVersion = (state.document ? state.document->Version() : 0) ^
                    ((state.assembly ? state.assembly->Version() : 0) << 1);
  // Clear the picks, not the tool: the point of pinning is to go straight on
  // to the next measurement.
  m.picked.clear();
  UpdateMeasureReadout(state);
}

void UnpinMeasurement(AppState& state, uint64_t id) {
  auto& v = state.measure.pinned;
  if (id == 0) {
    v.clear();
    return;
  }
  v.erase(std::remove_if(
              v.begin(), v.end(),
              [id](const MeasureState::Pinned& p) { return p.id == id; }),
          v.end());
}

void RefreshPinnedMeasurements(AppState& state) {
  MeasureState& m = state.measure;
  if (m.pinned.empty())
    return;
  // Only when the model actually moved under them.
  const uint64_t version =
      (state.document ? state.document->Version() : 0) ^
      ((state.assembly ? state.assembly->Version() : 0) << 1);
  if (version == m.pinnedVersion)
    return;
  m.pinnedVersion = version;
  const units::System u = state.ActiveUnits();
  // A pinned measurement whose element no longer resolves is DROPPED, not
  // left drawing its last known number over geometry that has changed shape
  // underneath it — the same drop-on-edit discipline the compare_geometry
  // overlay follows.
  const Measure::DistanceKind live = m.distanceKind;
  std::vector<MeasureState::Pinned> kept;
  for (MeasureState::Pinned& p : m.pinned) {
    bool resolves = true;
    for (const MeasureState::Element& e : p.elements) {
      const auto src = ResolveMeasureSource(state, e);
      if (!src || !Measure::Describe(*src->bodies, e.ref)) {
        resolves = false;
        break;
      }
    }
    if (!resolves)
      continue;
    m.distanceKind = p.kind; // each pin keeps the qualifier it was taken with
    const MeasureOut r =
        p.elements.size() == 1
            ? MeasureOne(state, p.elements[0], u)
            : MeasurePair(state, p.elements[0], p.elements[1], u);
    p.readout = r.readout;
    p.dimension = r.dimension;
    kept.push_back(p);
  }
  m.distanceKind = live;
  m.pinned = std::move(kept);
}

void MeasureElementClicked(AppState& state, const MeasureState::Element& e) {
  if (!state.measure.active ||
      (!state.document && !state.assembly && !state.job))
    return;
  auto& picked = state.measure.picked;
  const auto it = std::find(picked.begin(), picked.end(), e);
  if (it != picked.end())
    picked.erase(it); // clicking a measured element again deselects it
  else
    picked.push_back(e);
  UpdateMeasureReadout(state);
}

} // namespace ui
