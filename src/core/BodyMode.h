#pragma once

#include "CADKernel.h"
#include "core/ExtrudeFeature.h" // ExtrudeMode
#include "core/Feature.h"        // Body

#include <string>
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
bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape,
                   ElementMap shapeMap, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId);

// Fuses `instanceShapes` (a pattern/mirror's seed + transformed copies), splits
// the result back into its connected solids, erases every body created by
// `sourceId`, and appends each solid as a body owned by `ownerId`. Instances
// that touch merge into one body; disjoint instances become separate bodies.
// Returns false if no source body was found or the fuse failed.
bool FuseInstancesReplacingSource(std::vector<Body>& bodies, uint64_t sourceId,
                                  std::vector<CADShape> instanceShapes,
                                  const std::string& name,
                                  const std::string& material,
                                  uint64_t ownerId);
