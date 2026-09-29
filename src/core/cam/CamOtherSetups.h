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
// leftover. A setup cut AFTER others starts from what they left
// (`StartingStock`): the planar check, the rotary check and the playback
// alike.

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
  // Cut BEFORE the setup being checked (earlier in the job's order), so
  // what it removed is gone when this one starts.
  bool earlier = false;
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
  [[nodiscard]] bool HasEarlier() const {
    for (const SetupStock& s : stocks)
      if (s.earlier)
        return true;
    return false;
  }
};

// The stock a setup starts from, when setups earlier in the job have already
// cut: each column of `stock` (this setup's fresh stock, in its frame) lowered
// to the first point no earlier setup removed. An indexed setup after a
// turning one then starts from the turned part rather than the square bar it
// was: checked against the fresh bar, every entry into the chess king's cross
// was a rapid 12.5 mm "through material" and every pass a 24 mm "bite". A
// point counts as removed only when it is `margin` clear of what an earlier
// setup left, so the seed errs toward material -- toward a finding.
void SeedFromEarlier(HeightMap& stock, const OtherSetups& others, double zLo,
                     double margin);
// The same for a rotary stock: each ray of `stock` (this setup's blank, a
// radius map about its axis) shortened to the first point no earlier setup
// removed.
void SeedFromEarlier(RadiusMap& stock, const OtherSetups& others,
                     double margin);

// The clearance a seed keeps from what an earlier setup left: a cell of the
// grid, and never under 0.3 mm.
[[nodiscard]] double SeedMargin(double res);

// The stock a setup STARTS from: its fresh stock, seeded from what the job's
// earlier setups in `others` left -- a height map, or for a program whose A
// moves a radius map. THE start: the verifier, the rotary check and the
// playback all begin from it, so the picture and the verdict start from the
// same stock and an indexed setup after a turning one is played and checked
// against the turned part, not the square bar. `others` may be null: fresh.
[[nodiscard]] bool StartingStock(const Setup& setup,
                                 const std::vector<Body>& rawBodies, double res,
                                 const OtherSetups* others, HeightMap& out);
[[nodiscard]] RadiusMap StartingRotaryStock(const Setup& setup,
                                            const std::vector<Body>& rawBodies,
                                            double res,
                                            const OtherSetups* others,
                                            std::string* why = nullptr);

// Model space to `setup`'s frame, when every body of the job maps the same
// way: per-part fixture frames or placements that differ between parts have
// no single map, and a point of leftover could belong to any of them.
[[nodiscard]] bool ModelToSetup(const Setup& setup,
                                const std::vector<Body>& rawBodies,
                                glm::dmat4& out, std::string* why);

struct SetupProgram {
  const Setup* setup = nullptr;
  std::string gcode;
  bool earlier = false; // cut before the setup being checked
};

// The job's other setups' final stock, from their posted programs: each
// inspected as `VerifyGcode` would, its machined map kept.
[[nodiscard]] OtherSetups OtherSetupsOf(const Setup& self,
                                        const std::vector<SetupProgram>& others,
                                        const std::vector<Body>& rawBodies,
                                        double res);

} // namespace cam
