#include "core/SweepFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <Standard_Failure.hxx>

SweepFeature::SweepFeature(const std::string& name, uint64_t profileSketchId,
                           uint64_t pathSketchId, ExtrudeMode mode)
    : profileSketchId(profileSketchId), pathSketchId(pathSketchId),
      mode(mode) {
  m_name = name;
}

bool SweepFeature::Execute(RegenContext& ctx) {
  const auto* prof =
      dynamic_cast<const SketchFeature*>(ctx.FindById(profileSketchId));
  const auto* path =
      dynamic_cast<const SketchFeature*>(ctx.FindById(pathSketchId));
  if (!prof || !path) {
    SetError("sweep needs a profile and a path sketch (missing or "
             "suppressed)");
    return false;
  }

  const auto loops = sketch::BuildLoops(prof->entities);
  const TopoDS_Wire profileWire =
      sketch::BuildProfileWire(prof->plane, loops);
  if (profileWire.IsNull()) {
    SetError("sweep profile has no single closed loop");
    return false;
  }
  const TopoDS_Wire spine =
      sketch::BuildOpenPathWire(path->plane, path->entities);
  if (spine.IsNull()) {
    SetError("sweep path must be one open chain of lines/arcs");
    return false;
  }

  try {
    // MakePipeShell (not MakePipe) so sharp path corners are handled: a plain
    // MakePipe truncates the sweep at the first corner. RoundCorner rounds the
    // spine's corners; the profile is swept keeping its orientation to the
    // spine, then closed into a solid.
    BRepOffsetAPI_MakePipeShell shell(spine);
    shell.SetTransitionMode(BRepBuilderAPI_RoundCorner);
    shell.Add(profileWire);
    shell.Build();
    if (!shell.IsDone() || !shell.MakeSolid()) {
      SetError("sweep failed (profile not perpendicular to the path start?)");
      return false;
    }
    CADShape result(shell.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("sweep produced an invalid solid");
      return false;
    }
    ElementMap map;
    topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    return ApplyBodyMode(ctx.bodies, std::move(result), std::move(map), mode,
                         m_name, m_material, GetId());
  } catch (const Standard_Failure& e) {
    SetError(std::string("sweep failed: ") + e.GetMessageString());
    LOG_ERROR("SweepFeature '{}': {}", m_name, GetError());
    return false;
  }
}
