#pragma once

#include "CADKernel.h"
#include "core/ExtrudeFeature.h" // ExtrudeMode
#include "core/Feature.h"        // Body

#include <string>
#include <vector>

// Applies a freshly built solid to the body list according to `mode`, shared
// by the solid-producing features (extrude, revolve):
//   NewBody: append `shape` as its own body.
//   Add:     fuse into the last body whose bbox overlaps (else the last).
//   Cut:     subtract from every body, dropping any the cut fully consumes.
// Returns false only on an operation that genuinely failed (a fuse/cut error,
// or Cut with no bodies). `shape` is moved from.
bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape, ExtrudeMode mode,
                   const std::string& name, const std::string& material);
