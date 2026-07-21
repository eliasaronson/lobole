#include "core/features/LoftFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include <BRepOffsetAPI_ThruSections.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Wire.hxx>

#include <vector>

LoftFeature::LoftFeature(const std::string& name,
                         std::vector<uint64_t> sketchIds, ExtrudeMode mode)
    : sketchIds(std::move(sketchIds)), mode(mode) {
  m_name = name;
}

bool LoftFeature::Execute(RegenContext& ctx) {
  if (sketchIds.size() < 2) {
    SetError("loft needs at least two profile sketches");
    return false;
  }

  // Build every section wire first so we can pick the skinning mode from their
  // topology (below), then feed them to a freshly-configured ThruSections.
  std::vector<TopoDS_Wire> wires;
  for (uint64_t id : sketchIds) {
    const auto* sk =
        dynamic_cast<const SketchFeature*>(ctx.FindById(id));
    if (!sk) {
      SetError("loft profile sketch is missing (or suppressed)");
      return false;
    }
    const auto loops = sketch::BuildLoops(sk->entities);
    const TopoDS_Wire wire = sketch::BuildProfileWire(sk->plane, loops);
    if (wire.IsNull()) {
      SetError("loft section '" + sk->GetName() +
               "' has no single closed profile");
      return false;
    }
    wires.push_back(wire);
  }

  // Smooth vs. ruled skinning. A smoothed loft fits ONE BSpline surface through
  // ALL sections at once, so OCC must find a single consistent vertex/seam
  // correspondence across them. When the sections share topology (e.g. every
  // section a circle, or every section a 4-line rectangle) that correspondence
  // is unambiguous and the smooth skin is exact — two coaxial circles loft to a
  // true cylinder. But when vertex counts differ (a 4-vertex square paired with
  // a 1-seam circle), the correspondence is arbitrary: OCC lands the square's
  // corners ~45deg off the circle's seam and the resulting twist propagates
  // along the WHOLE surface, pinching even the circle->circle span into an
  // hourglass (the reported bug: square->r5->r5 came out below the r5 cylinder).
  // Ruled skinning sidesteps this by fitting an INDEPENDENT patch between each
  // consecutive pair, so a twist in one span can't bleed into another and each
  // matched pair (circle->circle) stays exact. So: smooth when all sections have
  // equal edge counts, ruled when they differ.
  auto edgeCount = [](const TopoDS_Wire& w) {
    int n = 0;
    for (TopExp_Explorer e(w, TopAbs_EDGE); e.More(); e.Next()) n++;
    return n;
  };
  bool sameTopology = true;
  const int firstCount = edgeCount(wires.front());
  for (const TopoDS_Wire& w : wires)
    if (edgeCount(w) != firstCount) sameTopology = false;

  BRepOffsetAPI_ThruSections thru(/*isSolid=*/Standard_True,
                                  /*ruled=*/sameTopology ? Standard_False
                                                         : Standard_True);
  for (const TopoDS_Wire& w : wires)
    thru.AddWire(w);

  try {
    thru.Build();
    if (!thru.IsDone()) {
      SetError("loft failed to skin the sections");
      return false;
    }
    CADShape result(thru.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("loft produced an invalid solid");
      return false;
    }
    // Fresh primitive-style names: the lofted skin is new topology (a later
    // fillet references these faces).
    ElementMap map;
    topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    return ApplyBodyMode(ctx.bodies, std::move(result), std::move(map), mode,
                         m_name, m_material, GetId());
  } catch (const Standard_Failure& e) {
    SetError(std::string("loft failed: ") + e.GetMessageString());
    LOG_ERROR("LoftFeature '{}': {}", m_name, GetError());
    return false;
  }
}
