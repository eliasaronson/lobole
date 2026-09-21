#pragma once

#include "core/cam/CamJob.h"    // Tool, TipHeight
#include "core/cam/CamRotary.h" // RadiusMap
#include "core/cam/CamVerify.h" // HeightMap

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// A surface for the height map, so the stock can be LOOKED at
// (docs/cam/simulation.md, S2).
//
// One vertex per cell is 750k vertices on a 200x150 mm plate at 0.2 mm, and
// almost all of them would be co-planar: milled 2.5D stock is an uncut top, a
// few floors, and walls between them. So equal-height cells are merged into
// rectangles first — the same move FreeCAD's `cStock::Tessellate` makes
// (`TesselTop`/`TesselSidesX`), for the same reason.
//
// Flat-shaded on purpose. A milled step IS a wall; smoothing it into a ramp
// would draw a part that was never cut.
namespace cam {

struct StockVertex {
  glm::vec3 position;
  glm::vec3 normal;
};

struct StockMesh {
  std::vector<StockVertex> vertices;
  std::vector<uint32_t> indices;
  glm::vec3 bboxMin{0.0f};
  glm::vec3 bboxMax{0.0f};

  [[nodiscard]] bool Empty() const { return indices.empty(); }
  // The volume the mesh encloses, by the divergence theorem. The gate: run
  // merging changes how many triangles come out and must never change this.
  [[nodiscard]] double Volume() const;
};

// The stock as a closed solid: the machined top, vertical walls wherever
// neighbouring cells differ, the four sides down to `floorZ`, and the floor.
// In SETUP space, like everything the simulation touches — the caller rotates
// it into the model's frame for drawing (C9).
//
// `floorZ` is the bottom of the block. A map whose cells have been cut BELOW
// it (a through cut with a sacrificial pass) still meshes: the floor follows
// the lowest point rather than clipping the cut away.
[[nodiscard]] StockMesh BuildStockMesh(const HeightMap& map, double floorZ);

// The ROTARY stock as a closed solid: a tube over the radius field, one vertex
// per cell at (x, r cos theta, r sin theta), and a flat cap at each end.
// In the PART's frame -- the playback turns it by the cursor's A, which is
// what "the part turns" means (docs/cam/rotary-plan.md, "The picture").
//
// Smooth-shaded, where the planar block is flat-shaded, and for the reason
// the planar comment gives in reverse: milled 2.5D stock is floors and walls,
// and smoothing a wall into a ramp draws a part that was never cut; a turned
// part is overwhelmingly CURVED, and flat-shading 200,000 cells of it draws
// facets that were never cut either. Normals come from the surface's own
// partial derivatives, so a shoulder still leans the way it faces.
[[nodiscard]] StockMesh BuildRotaryStockMesh(const RadiusMap& map);

// The cutter itself, as a solid of revolution about +Z with its TIP AT THE
// ORIGIN — the same point every `Move` in a toolpath addresses, so placing it
// is a translation and nothing else.
//
// The profile is `TipHeight`, the one function that already decides what a
// tool's end looks like: the ball's bowl, the V-bit's cone, the drill's
// point, the flat mill's nothing. Drawing the tool from anywhere else would
// let the picture show a cutter that removes different material than the
// simulation does.
//
// Above the flutes it grows a holder, because the reach of the thing is part
// of what a person is looking for when they watch a job run.
[[nodiscard]] StockMesh BuildToolMesh(const Tool& tool);

} // namespace cam
