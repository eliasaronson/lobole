#pragma once

// Leftover another setup removes
// (docs/cam/simulation.md#leftover-another-setup-removes).
//
// Each setup is verified from its own fresh stock, so a flip job's front side
// reports the half-depth outline the back finishes, and an indexed setup the
// material a turn or the other side removes. What the OTHER setups of the job
// leave is their final machined map -- the air above a height map, beyond a
// radius map -- and a leftover cell whose whole excess lies in that air is
// not left on the part: it is said as cut by that setup, not counted as
// leftover. The simulation and the safety checks still start from fresh
// stock, which errs toward a finding.

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamVerify.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cam {

// What one other setup leaves, in its own frame.
struct SetupStock {
  uint64_t id = 0;
  std::string name;
  glm::dmat4 modelToSetup{1.0};
  bool rotary = false;
  HeightMap height;            // planar: the machined top
  double zLo = 0.0, zHi = 0.0; // planar: the stock box's floor and top
  RadiusMap radius;            // rotary: the machined radius
  // Whether this setup leaves air at `model` (model space), to within `tol`:
  // inside its stock, and above (beyond) what it machined. A point outside
  // its stock is not claimed -- two setups whose boxes disagree leave the
  // doubtful strip as leftover.
  [[nodiscard]] bool Removes(const glm::dvec3& model, double tol) const;
};

struct OtherSetups {
  glm::dmat4 toModel{1.0}; // this setup's frame to model space
  std::vector<SetupStock> stocks;
  // "setup 'Back', because in it ..." for each setup that could not be
  // consulted, or "the job's other setups, because in this one ..." when none
  // could.
  std::vector<std::string> notChecked;
  // The id of the setup that removes the material from `from` to `to` (this
  // setup's frame): every point of it removed by one of them, the one
  // removing the most named. 0 when some point is removed by none.
  [[nodiscard]] uint64_t Claims(const glm::dvec3& from, const glm::dvec3& to,
                                double tol) const;
  [[nodiscard]] std::string NameOf(uint64_t id) const;
};

// Model space to `setup`'s frame, when every body of the job maps the same
// way: per-part fixture frames or placements that differ between parts have
// no single map, and a point of leftover could belong to any of them.
[[nodiscard]] bool ModelToSetup(const Setup& setup,
                                const std::vector<Body>& rawBodies,
                                glm::dmat4& out, std::string* why);

struct SetupProgram {
  const Setup* setup = nullptr;
  std::string gcode;
};

// The job's other setups' final stock, from their posted programs: each
// inspected as `VerifyGcode` would, its machined map kept.
[[nodiscard]] OtherSetups OtherSetupsOf(const Setup& self,
                                        const std::vector<SetupProgram>& others,
                                        const std::vector<Body>& rawBodies,
                                        double res);

} // namespace cam
