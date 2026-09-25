#pragma once

#include "cad/CADKernel.h"
#include "core/Feature.h"                 // Body
#include "core/features/BooleanFeature.h" // BooleanOp
#include "core/features/ExtrudeFeature.h" // ExtrudeMode

#include <string>
#include <utility>
#include <vector>

// Applies a freshly built solid to the body list according to `mode`, shared
// by the solid-producing features (extrude, revolve):
//   NewBody: append `shape` as its own body, stamped with `ownerId` as its
//            createdBy and carrying `shapeMap` as its element map.
//   Add:     fuse into the last body whose bbox overlaps (else the last),
//            leaving that body's createdBy (and material) untouched; element
//            names propagate through the fuse (toponaming T5) — the target's
//            names plus `shapeMap`'s, rewritten onto the fused shape.
//   Cut:     subtract from every body, dropping any the cut fully consumes;
//            each surviving body's names propagate through its cut.
// Returns false only on an operation that genuinely failed (a fuse/cut error,
// or Cut with no bodies). `shape` and `shapeMap` are moved from.
//
// `warning` (optional out-param): set to a human-readable message when the
// operation SUCCEEDED but did nothing useful — a Cut whose tool intersected no
// body (total volume unchanged), or an Add whose tool lay entirely inside its
// target. These are not errors (the result is valid), but almost always signal
// a mis-placed sketch/plane, so callers surface them via Feature::SetWarning.
//
// `error` (optional out-param): the reason for a false return, in user-grade
// prose, so callers can surface it via Feature::SetError instead of the
// generic "regeneration failed". Without it the reason is only logged.
bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape,
                   ElementMap shapeMap, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId, std::string* warning = nullptr,
                   std::string* error = nullptr);

// What a body boolean must keep, checked on its RESULT before it is committed
// (docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept). A kernel
// boolean can return a shape that is non-null, passes BRepCheck, and is still
// wrong: a slot whose round ends were the same cylinders as two bores came
// back with the bores' floors kept inside the slot and its far end standing
// in the empty bores — the bores INSIDE OUT, a section solid where the holes
// are, and the part 13,800 mm3 lighter than any cut could have made it.
// Checked:
//   - the result's volume is not negative, and no closed shell of it is the
//     wrong way round (a solid's outer shell inward, a void outward);
//   - a Subtract is no larger than `target`, an Intersect no larger than
//     either operand, a Union no smaller than `target`;
//   - no face of a cut or a union lies strictly INSIDE the tool, and none of
//     an intersection outside it — judged for every face the boolean made
//     and every face of the target it kept whole that reaches into the
//     tool's box, at one point inside each, against the tool (a sound solid,
//     and usually a small one).
// The volumes are the ones the callers need anyway; the rest cost the
// OM615's build about 2%. Tolerances are relative to the sizes compared.
// Returns the refusal in user-grade prose naming `feature`, the operation,
// `body` and both volumes; empty when the result is sound.
[[nodiscard]] std::string BooleanRefusal(const std::string& feature,
                                         BooleanOp op, const std::string& body,
                                         const CADShape& target,
                                         const CADShape& tool,
                                         const CADShape& result);

// Convenience for the solid-producing features: run ApplyBodyMode and forward
// any advisory onto `owner` (Feature::SetWarning) and any failure reason onto
// Feature::SetError, so every cut/add feature reports the same way without
// repeating the plumbing.
bool ApplyBodyModeReporting(std::vector<Body>& bodies, CADShape shape,
                            ElementMap shapeMap, ExtrudeMode mode,
                            Feature& owner);

// Fuses `instances` (a pattern/mirror's seed + transformed copies, each with
// its ElementMap), splits the result back into its connected solids, erases
// every body created by `sourceId`, and appends each solid as a body owned by
// `ownerId`. Instances that touch merge into one body; disjoint instances
// become separate bodies. Each instance's names travel with it through the
// fuse, so a patterned body's faces can be picked, threaded and fitted like
// the seed's — the caller suffixes them per instance ("4/g7" -> "4/g7#2"),
// the convention patterned CUTS already use. Returns false if no source body
// was found or the fuse failed; `error` (optional) receives the reason for
// callers to SetError.
bool FuseInstancesReplacingSource(
    std::vector<Body>& bodies, uint64_t sourceId,
    std::vector<std::pair<CADShape, ElementMap>> instances,
    const std::string& name, const std::string& material, uint64_t ownerId,
    std::string* error = nullptr);
