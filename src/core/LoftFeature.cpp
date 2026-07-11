#include "core/LoftFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <BRepOffsetAPI_ThruSections.hxx>
#include <Standard_Failure.hxx>

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

  BRepOffsetAPI_ThruSections thru(/*isSolid=*/Standard_True);
  int sections = 0;
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
    thru.AddWire(wire);
    sections++;
  }

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
