#pragma once

#include "cad/CADKernel.h"

#include <BRepTools_History.hxx>
#include <TopoDS_Shape.hxx>
#include <glm/glm.hpp>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

// A worm and the wheel it drives (docs/modeling/worm-gears.md). The worm is a
// thread with a gear's axial profile, so it is HelicalSweep with a ZA
// (straight-sided axial) tooth space. The wheel is built the way it is cut: a
// hob — the worm, oversized — rolled through the mesh motion and subtracted
// at enough steps that the envelope converges.
class AssemblyDocument;
struct Mate;

namespace worm {

// What a worm pair is. Lengths in mm, the pressure angle in degrees.
struct PairSpec {
  double module = 0.0;       // the worm's axial module = the wheel's module
  int starts = 1;            // z1, threads on the worm
  int teeth = 0;             // z2, teeth on the wheel (0: the worm alone)
  double wormPitchDia = 0.0; // d1
  double pressureDeg = 20.0; // the ZA profile's axial pressure angle
  bool rightHanded = true;
  double clearance = 0.25; // c: root clearance as a fraction of the module
};

// Every size that follows from a PairSpec (KHK / DIN 3975, axial module).
struct Sizes {
  double axialPitch = 0.0; // px = pi m
  double lead = 0.0;       // pz = z1 px
  double leadAngle = 0.0;  // gamma, radians: tan = z1 m / d1
  double wormPitchRadius = 0.0, wormTipRadius = 0.0, wormRootRadius = 0.0;
  // The wheel (zero when teeth == 0).
  double wheelPitchRadius = 0.0;  // r2 = z2 m / 2
  double centreDistance = 0.0;    // a = r1 + r2
  double throatRadius = 0.0;      // ri = r1 - m, about the worm's axis
  double wheelThroatRadius = 0.0; // a - ri = r2 + m, at the mid-plane
  double wheelRootRadius = 0.0;   // r2 - (1 + c) m
  // The tip: r2 + 1.5 m, trimmed so the throat's arc stays within 45 degrees
  // of the mid-plane (the wheel prints axis-up; docs/modeling/worm-gears.md).
  double wheelTipRadius = 0.0;
  double faceWidth = 0.0; // b2 = 2 m (0.5 + sqrt(q + 1)), the usual face
};

// Why a spec cannot be built, or empty. `wheel` also checks the wheel's own
// numbers.
[[nodiscard]] std::string Check(const PairSpec& spec, bool wheel);
[[nodiscard]] Sizes Size(const PairSpec& spec);

// The throat's included arc, in degrees, on a wheel of face width `face`
// whose tip is at `tipRadius`.
[[nodiscard]] double ThroatArcDeg(const Sizes& sizes, double face,
                                  double tipRadius);

// Static self-locking (docs/modeling/worm-gears.md#sizing-a-pair): the pair
// locks when the lead angle is under the friction angle atan(mu).
// Efficiency is the worm driving, tan(gamma) / tan(gamma + rho).
struct Locking {
  double mu = 0.0;
  double frictionAngleDeg = 0.0;
  double efficiency = 0.0;
  bool locks = false;
};
[[nodiscard]] Locking SelfLocking(double leadAngleRad, double mu);

// The worm's tooth space as a closed axial profile for HelicalSweep, centred
// on y = 0: the ZA flanks from the root out to the tip, then straight out to
// `reach`. `backlash` (mm, axial) thins the tooth by half of it per flank.
// Empty and `why` when the root closes or the tooth comes to a point.
[[nodiscard]] std::vector<gp_Pnt2d> WormSpaceProfile(const PairSpec& spec,
                                                     double backlash,
                                                     double reach,
                                                     std::string* why);

// The worm's tooth space swept over [z0, z1] of `frame` (Direction the axis,
// XDirection where the phase is read): a TOOTH is centred at height `toothZ`
// on the XDirection side. Subtract it from the tip cylinder to get the worm.
[[nodiscard]] CADShape WormSpaceTool(const gp_Ax3& frame, double z0, double z1,
                                     bool squareStart, bool squareEnd,
                                     double toothZ, const PairSpec& spec,
                                     double backlash, double reach,
                                     std::string* why);

struct HobOptions {
  // Hob positions per wheel pitch. 0 (the default) follows the mesh motion
  // continuously — the limit the discrete hobbing loop converges to; a
  // positive count reproduces that loop at that many positions, scallops and
  // all (docs/modeling/worm-gears.md#how-the-wheel-is-cut).
  int stepsPerPitch = 0;
  int slices = 0;      // transverse sections across the face; 0 = derived
  int flankPoints = 0; // points per flank per section; 0 = derived
};

struct HobStats {
  int slices = 0;
  int flankPoints = 0;
  double seconds = 0.0;     // the whole wheel tool, wall time
  double spaceVolume = 0.0; // one tooth space, before the throat
  // The space's area in the mid-plane section, from root to tip, integrated
  // straight from the envelope (no loft between): the measure the hob's
  // convergence is read on.
  double midArea = 0.0;
  double tipRadius = 0.0; // where the tip ended up (Sizes' at most)
  // The thinnest crest (mm, as an arc): the tooth's width at the top of the
  // wheel's material, scanned across the face. `tipCrestZ` is where it is,
  // off the mid-plane along the axis; `tipCrestRadius` the radius it was
  // measured at (the tip, or the throat or blank where those are lower).
  double tipCrest = 0.0;
  double tipCrestZ = 0.0;
  double tipCrestRadius = 0.0;
};

// What a hobbed wheel loses, in two tools to subtract in turn from the
// blank: `rim`, the throat (a torus about the wheel's axis) and the tip
// trimmed to Sizes::wheelTipRadius; and `spaces`, z2 disjoint tooth spaces.
//
// A tooth space is the envelope of the hob — the worm with its tip raised by
// the root clearance and each flank thickened by `crowning` (mm) — rolled
// through the mesh: the wheel turning theta and the worm the turn that screws
// its thread r2*theta along its axis. It is found point by point: a point of
// the wheel is cut when some position of the hob contains it, and each
// transverse section's space is bounded by the two flank curves where that
// stops being true. The sections are lofted into one solid, which is turned
// z2 times.
//
// `frame`: origin on the wheel's axis at the face's mid-plane, Direction the
// axis, XDirection where tooth space 0 is centred (where the worm's tooth
// faces the wheel at zero turn). `halfFace` is half the face width,
// `blankRadius` the blank's.
struct WheelTools {
  CADShape rim;
  CADShape spaces; // z2 solids, space i turned i pitches about the axis
  // Where the spaces live, for cutting them by replication
  // (CutSpacesBySectors): a ring about the axis, `innerRadius` (under every
  // root) to `outerRadius` (past the blank), `halfHeight` either side of the
  // mid-plane (past every space's ends). `lowerSplit` and `upperSplit` are
  // angles from the frame's XDirection about its axis, below and above the
  // mid-plane, that run inside space 0 from root to tip wherever the wheel
  // has material: a sector bounded there and one pitch on holds one whole
  // tooth and cuts no tooth face.
  double innerRadius = 0.0, outerRadius = 0.0, halfHeight = 0.0;
  double lowerSplit = 0.0, upperSplit = 0.0;
};
[[nodiscard]] bool WheelCutTools(const gp_Ax3& frame, double halfFace,
                                 double blankRadius, const PairSpec& spec,
                                 double crowning, const HobOptions& options,
                                 WheelTools* out, HobStats* stats,
                                 std::string* why);

// The wheel's teeth cut from `body` (already throated) by replication
// (docs/modeling/worm-gears.md#how-the-teeth-are-cut): the ring of the body
// the spaces reach is cut once, a pitch-wide sector of it less spaces 0 and 1,
// and that sector is turned z2 times and glued back. The one boolean against
// a lofted space costs what forty did. Refused (false, `why`) when that ring
// is not a solid of revolution about the wheel's axis — something already
// cut into it would be copied round — or when a step fails; the caller then
// cuts the spaces the ordinary way.
//
// `history` maps every face and edge of `body` and of `tools.spaces` onto
// `result`, as one boolean's would. `removed` is what the cut took: z2 times
// the sector's loss, for the caller to check the result's volume against.
struct SectorCut {
  TopoDS_Shape result;
  Handle(BRepTools_History) history;
  double removed = 0.0;
};
[[nodiscard]] bool CutSpacesBySectors(const TopoDS_Shape& body,
                                      const gp_Ax3& frame, int teeth,
                                      const WheelTools& tools, SectorCut* out,
                                      std::string* why);

// The ratio of a worm relation (docs/assemblies/relations.md#worm): the
// wheel's mate turns this much per radian of the worm's. Its size is
// starts/teeth; its sign comes from the hand and the geometry. A right-hand
// worm turned +phi about its axis u screws its thread back along u and
// carries the wheel's tooth with it, so the wheel, on axis w, turns
// -phi*z1/z2 when the worm sits on the side c (from the wheel's axis to the
// worm's) where (u x w).c > 0, and the other way on the other side. A mate's
// DOF turns its B side about its A frame's Z, so a gear on the A side of its
// mate turns the other way about that axis; the carrier is the occurrence the
// two mates share, else the fixed one, else the A side. `wormFrame` and
// `wheelFrame` are the two mates' A connectors in world space. 0 and `why`
// when the axes are parallel or meet.
[[nodiscard]] double
RelationRatio(const AssemblyDocument& asmDoc, const Mate& wormMate,
              const Mate& wheelMate, bool samePath, bool topLevel,
              const glm::dmat4& wormFrame, const glm::dmat4& wheelFrame,
              int starts, int teeth, bool rightHanded, std::string* why);

} // namespace worm
