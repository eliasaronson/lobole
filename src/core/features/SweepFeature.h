#pragma once

#include "core/BodyMode.h" // ExtrudeMode
#include "core/Feature.h"

#include <TopoDS_Wire.hxx>

#include <cstdint>
#include <string>
#include <vector>

// How the swept profile is carried along the spine (OCC MakePipeShell trihedron
// mode). CorrectedFrenet is the robust default — the section stays normal to
// the path and can't self-twist on a helix; Frenet is the raw Frenet frame;
// Fixed keeps the profile's world orientation constant.
enum class SweepFrame { CorrectedFrenet, Frenet, Fixed };

// Resolves a sweep path id to an open spine wire: a SketchFeature (chain of
// lines/arcs/splines) or a CurveFeature (3D curve). Null wire if unusable.
// Shared by SweepFeature and PipeFeature (curves-3d Phase 1/2).
TopoDS_Wire ResolveSweepPathWire(const RegenContext& ctx, uint64_t pathId);

// Skins one or more closed profile sections along an open spine into a solid
// (MakePipeShell, rounded corners). With one section it's a plain sweep; with
// several it's a multi-section "loft along a path". An optional `guide`
// (auxiliary spine) shapes the sections as they travel — a second curve giving
// the "two directions" sweep/loft: when `scaleToGuide` the sections grow/shrink
// to keep contact with the guide (BRepFill_ContactOnBorder), else the guide
// only reorients them (BRepFill_NoContact). `frame` applies only when there is
// no guide. Null CADShape on failure; throws no OCC exception. Shared by
// SweepFeature, PipeFeature, and LoftFeature (curves-3d Phase 2/3).
CADShape SweepSectionsAlongSpine(const std::vector<TopoDS_Wire>& profiles,
                                 const TopoDS_Wire& spine, SweepFrame frame,
                                 const TopoDS_Wire& guide = TopoDS_Wire(),
                                 bool scaleToGuide = false);

// Single-profile convenience over SweepSectionsAlongSpine (orient-only guide).
CADShape SweepProfileAlongSpine(const TopoDS_Wire& profile,
                                const TopoDS_Wire& spine, SweepFrame frame,
                                const TopoDS_Wire& guide = TopoDS_Wire());

// Sweeps a closed profile sketch along an open path (modeling M8):
// BRepOffsetAPI_MakePipeShell skins the profile down the spine — a pipe from a
// circle down a bent path, a moulding along an edge, a coil along a helix, ...
// The path (`pathSketchId`) can be either a sketch (an open chain of
// lines/arcs/splines) OR a Curve3D feature (fit-spline / helix / control-poly)
// — curves-3d Phase 1. The profile should sit at the path's start and roughly
// perpendicular to it (as when a profile sketch and a path sketch share the
// start point on perpendicular planes). Modes reuse ApplyBodyMode.
// Regeneration fails per-feature when the profile is missing or not one closed
// loop, the path is missing or not one open chain/curve, or OCC rejects the
// sweep.
class SweepFeature : public Feature {
public:
  SweepFeature(const std::string& name, uint64_t profileSketchId,
               uint64_t pathSketchId, ExtrudeMode mode = ExtrudeMode::NewBody);

  std::string TypeName() const override { return "Sweep"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<SweepFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  uint64_t profileSketchId = 0;
  uint64_t pathSketchId = 0; // a sketch OR a Curve3D (see ResolveSweepPathWire)
  uint64_t guideId = 0;      // optional guide sketch/Curve3D, 0 = none (v44)
  ExtrudeMode mode = ExtrudeMode::NewBody;
  SweepFrame frame = SweepFrame::CorrectedFrenet; // v43
};
