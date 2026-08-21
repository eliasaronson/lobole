#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <vector>

// Verifying the G-code we emit (docs/cam/plan.md, C5).
//
// The loop runs on the POSTED FILE, not on the in-memory toolpath: parse the
// G-code back, simulate the material it removes, and compare the result with
// the model that was designed. That independence is the point — a bug in the
// post, in modal state, or in linking is invisible to anything that inspects
// `std::vector<Move>`, and one of those (a NaN retract) had already shipped.
//
// Surveyed before writing (~/Documents/CAD/CAM): FreeCAD's PathSimulator is a
// height-map (`Array2D<float>` + a tool profile function) and CAMotics is a
// full implicit-field/marching-cubes simulator needing its own build system.
// For 3-axis verification the height map is the right model, and it needs no
// dependency at all. Where this differs from FreeCAD's: it tests cells
// analytically against the swept tool rather than oversampling the path at
// 0.6-pixel steps — for a check that has to be trustworthy, exact beats
// "small enough steps that we probably hit every pixel".
namespace cam {

// --- parsing --------------------------------------------------------------

struct GMove {
  bool rapid = false;
  glm::dvec3 to{0.0};
  int tool = 1;
  int line = 0; // 1-based, for messages that point at the file
};

struct GProgram {
  std::vector<GMove> moves;
  std::vector<std::string> errors; // malformed words, NaN coordinates, …
};

// Modal G-code: G0/G1 with X/Y/Z/F, T for the tool, parenthesised comments.
// Anything it does not understand is reported rather than skipped silently.
[[nodiscard]] GProgram ParseGcode(std::string_view text);

// --- height maps ----------------------------------------------------------

struct HeightMap {
  double x0 = 0.0, y0 = 0.0; // corner of cell (0,0)
  double res = 0.25;         // mm per cell
  int nx = 0, ny = 0;
  std::vector<float> z;

  [[nodiscard]] float At(int i, int j) const {
    return z[static_cast<size_t>(j) * static_cast<size_t>(nx) +
             static_cast<size_t>(i)];
  }
  void Set(int i, int j, float v) {
    z[static_cast<size_t>(j) * static_cast<size_t>(nx) +
      static_cast<size_t>(i)] = v;
  }
  [[nodiscard]] double CellX(int i) const { return x0 + (i + 0.5) * res; }
  [[nodiscard]] double CellY(int j) const { return y0 + (j + 0.5) * res; }
};

// The stock block, uncut.
[[nodiscard]] HeightMap MakeStock(const glm::dvec3& min, const glm::dvec3& max,
                                  double res);

// Runs `program` against `stock`, lowering it where the tool passes. Tool
// radii come from the setup's tool table (the file carries only T-words).
void Simulate(const GProgram& program, const Setup& setup, HeightMap& stock);

// The designed part's top surface on the same grid: the highest point of the
// bodies above each cell, or the stock floor where there is no material.
[[nodiscard]] HeightMap SampleModel(const std::vector<Body>& bodies,
                                    const HeightMap& like, double floorZ);

// --- the verdict ----------------------------------------------------------

struct Verdict {
  double maxGouge = 0.0;    // mm cut BELOW the designed surface (scrap)
  double maxLeftover = 0.0; // mm of stock still above it (unfinished)
  glm::dvec2 gougeAt{0.0};
  glm::dvec2 leftoverAt{0.0};
  int rapidsThroughStock = 0; // rapid moves that pass through material
  int firstBadLine = 0;       // the G-code line of the first such rapid
  size_t cells = 0;

  // A job is good when it cut everything it should and nothing it should not.
  [[nodiscard]] bool Ok(double tolerance) const {
    return maxGouge <= tolerance && maxLeftover <= tolerance &&
           rapidsThroughStock == 0;
  }
};

[[nodiscard]] Verdict Compare(const HeightMap& machined,
                              const HeightMap& designed);

// The whole loop, for callers that just want the answer: post text in,
// verdict out.
[[nodiscard]] Verdict VerifyGcode(const std::string& gcode, const Setup& setup,
                                  const std::vector<Body>& bodies,
                                  double res = 0.25);

} // namespace cam
