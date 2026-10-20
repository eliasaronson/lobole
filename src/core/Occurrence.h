// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Configuration.h"
#include "core/ElementMap.h"
#include "core/Units.h"
#include "core/fem/FemStudy.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// One placed instance of a part within an assembly. Stores an *explicit*
// transform — its last solved placement — so an occurrence is always loadable
// on its own; mates re-solve over non-`fixed` occurrences on load
// (the literal-plus-`_expr` pattern applied to placement). See
// docs/assemblies/README.md#occurrences.
// What an occurrence places (A5). The field it qualifies is `partId`, whose
// SHAPE does not change — a slot id is a slot id — but whose resolution
// widens from "a part document" to "a part or an assembly document". Stored
// explicitly rather than inferred from whatever the id currently resolves to,
// so a DANGLING reference still knows what it was looking for and can say so.
enum class OccurrenceRef { Part, Assembly };

// Where one occurrence of a FLEXIBLE placement's sub-assembly stands, in the
// placement's own frame (v129,
// docs/assemblies/README.md#flexible-sub-assemblies). Keyed by that
// occurrence's id in the sub-assembly: only one level moves, so the id IS its
// whole path below the placement.
struct InnerPose {
  uint64_t occurrence = 0;
  glm::dvec3 position{0.0};
  glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
};

// A mate of a flexible placement's sub-assembly as THIS placement has it
// posed (v129): every value a mate can move, by the mate's id there.
struct InnerMateDof {
  uint64_t mate = 0;
  double rotation = 0.0;
  double offset = 0.0;
  double dx = 0.0;
  double dy = 0.0;
  glm::dquat ballRotation{1.0, 0.0, 0.0, 0.0};
};

// Where an attached occurrence is SEATED on its parent, by name: the planar
// face it bears on (`face`) and the cylinder or cone whose axis centres it
// (`axis`) — a screw's hole (its head's face and the bore, or the far face
// for a nut), a ring's groove (a flank and the groove's floor), a part on a
// shaft (a shoulder and the journal). Nothing in it is a fastener's: any
// standard part seated on two named faces rides this way. `innerPath`
// reaches a part inside a sub-assembly parent, as a Connector's does.
struct AttachSeat {
  ElementRef face;
  ElementRef axis;
  std::vector<uint64_t> innerPath;
  bool IsSet() const { return face.IsSet() && axis.IsSet(); }
};

struct Occurrence {
  uint64_t id = 0;     // stable, unique within the assembly, never reused
  std::string name;    // display label (cosmetic)
  uint64_t partId = 0; // stable id of the referenced part or assembly document
  OccurrenceRef refKind = OccurrenceRef::Part;
  std::optional<std::string> partVersion; // reserved: git-commit pin (F5)

  // Placement: translation + orientation (quaternion). Diffable, shear-free,
  // and how mate frames will think — not a raw mat4.
  glm::dvec3 position{0.0};
  glm::dquat orientation{1.0, 0.0, 0.0, 0.0}; // (w,x,y,z) identity

  bool visible = true;
  bool fixed = false; // grounded flag; an anchor for the placement pass

  // What this occurrence RIDES ON (docs/parts/README.md#attached-and-mated):
  // the id of another occurrence in the same assembly, with `attachOffset` /
  // `attachRotation` its placement in that occurrence's frame. 0 = free.
  //
  // Not a mate, deliberately. A mate is for kinematics and a fastener has
  // none — it is a zero-DOF leaf — so putting one in the mate graph costs up
  // to four rows per joint in the list you read the MECHANISM from, and mate
  // placement is frame alignment, which could relocate the fastener away from
  // where the fit computed it. An attachment is a composition, not a
  // constraint: no solver sees it.
  //
  // `position`/`orientation` keep their one meaning — the placement in the
  // ASSEMBLY — and are RECOMPUTED from the parent by ApplyAttachments. That
  // is the trade: one derived field pair instead of overloading what
  // `position` means for some occurrences, which is the kind of double duty
  // that has bitten this codebase before.
  uint64_t attachedTo = 0;
  glm::dvec3 attachOffset{0.0};
  glm::dquat attachRotation{1.0, 0.0, 0.0, 0.0};

  // The SEAT it rides on (v157; docs/parts/README.md#attached-and-mated).
  // Set, `attachOffset`/`attachRotation` are its placement in the seat's
  // frame — derived on every solve from these named faces of the parent
  // (assembly::SeatFrame) — so a hole that moves INSIDE its part, a plate
  // made thicker, takes the screw with it. Unset, they are in the parent's
  // own frame, which is what every file before v157 means.
  AttachSeat attachSeat;
  // Transient, never saved. `attachError`: the seat no longer resolves, so
  // the rider is left where it last was and SAYS so (like a mate's `error`).
  // `attachWarning`: a rider with no seat, riding at a fixed offset.
  // `attachSearched`: BindRidersToHoles looked for its hole this session.
  std::string attachError;
  std::string attachWarning;
  bool attachSearched = false;
  // Transient: the seat's frame as last resolved, and the part document
  // (InstanceId, Version) it was resolved in. Resolving a face computes its
  // centroid, which is most of a solve's attachment time, and a part that
  // did not regenerate cannot have moved its seat.
  uint64_t seatCacheDoc = 0;
  uint64_t seatCacheVersion = 0;
  glm::dmat4 seatCacheFrame{1.0};

  // Which CONFIGURATION of the referenced document this placement uses
  // (A5, v56; a part's since v135); 0 = whatever that document's active
  // configuration is, which is the behaviour every existing file means.
  //
  // This is what makes a placed document reusable rather than merely tidy:
  // one Gangway placed "short" beside another placed "long", one chess piece
  // part placed as a pawn and as a rook. A part in a configuration is a copy
  // regenerated in it (ui::ConfiguredPartDocument), read through
  // ui::OccurrencePartDoc.
  uint64_t configurationId = 0;

  // The material this placement is made of, overriding every body's (a
  // material slug, v135); "" = the part's own. One piece part in maple and
  // in walnut is one part, not two copies that differ in a slug.
  std::string material;

  // FLEXIBLE (v129): a placed sub-assembly whose own mechanism moves in this
  // assembly — its occurrences, mates and relations join the parent's solve
  // and motion studies, instanced for this placement. A sub-assembly
  // document is shared by every placement of it (and handed out const), so
  // the poses live HERE, on the parent's occurrence: two placements never
  // fight over them, the parent's undo sees them, and the file re-solves the
  // same way. Rigid (false) is every file before v129.
  bool flexible = false;
  std::vector<InnerPose> innerPoses;       // the moving inner occurrences
  std::vector<InnerMateDof> innerMateDofs; // every inner mate with a DOF

  const InnerPose* FindInnerPose(uint64_t innerOccurrence) const {
    for (const InnerPose& p : innerPoses)
      if (p.occurrence == innerOccurrence)
        return &p;
    return nullptr;
  }
  const InnerMateDof* FindInnerMateDof(uint64_t innerMate) const {
    for (const InnerMateDof& d : innerMateDofs)
      if (d.mate == innerMate)
        return &d;
    return nullptr;
  }
};

// Where on its part a connector frame is anchored
// (docs/assemblies/solver.md#connector-frames). Either the part's coordinate
// origin (an identity frame) or a face named by a toponaming ElementRef —
// planar (frame = centroid + normal, CADShape::FacePlane) or cylindrical (frame
// = axis, CADShape::FaceAxis). The frame itself is *derived* at solve time,
// never stored, so a part edit moves the connector with the geometry.
struct ConnectorAnchor {
  bool origin = false; // true → the part origin; `face` is ignored
  ElementRef face;     // valid when !origin
  // Inference snap (A2.5 phase 4): when set, the frame ORIGIN is this edge's
  // characteristic point (a circular edge's center, else its midpoint) instead
  // of the face centroid — e.g. a mate snapped to a hole center. The face still
  // gives the Z normal. Additive; face-centroid when unset.
  ElementRef snapEdge;
  // Corner snap (phase 4c, v65): with BOTH edges set the origin is the vertex
  // where they meet. A corner has no stable name of its own — the ElementMap
  // names faces and edges, not vertices — but two named edges do, and so
  // does the point they share. When they share no vertex or more than one,
  // the anchor falls back to `snapEdge`'s own point rather than guessing.
  ElementRef snapEdge2;
  // A named Frame feature of the part (v86, agent-scale plan G0): the frame
  // AS DESIGNED — a bolt-circle centre, a shaft axis, an offset seat —
  // instead of the centroid of whatever face is nearest. When set, `origin`
  // and `face` are ignored.
  uint64_t frameId = 0;
};

// One side of a mate: a frame on some occurrence of this assembly.
struct Connector {
  uint64_t occurrenceId = 0;
  ConnectorAnchor anchor;

  // A5/v57: when `occurrenceId` names a SUB-ASSEMBLY, the chain of
  // occurrence ids from inside it down to the part whose face the anchor
  // means. Empty for the ordinary case.
  //
  // A placed assembly has no faces of its own, so before this the only way
  // to mate onto one was to open that assembly — which cannot express "bolt
  // this bracket to the flange of the pump inside the pump unit". The mate
  // still positions the TOP-LEVEL occurrence: the anchor only reads
  // geometry, and a rigid unit's face is at a known place in its frame. In a
  // FLEXIBLE unit (v129) a path that enters at a moving part mates that part
  // (ExpandFlexible re-points it).
  std::vector<uint64_t> innerPath;
};

// The DOF a mate leaves open between its two connector frames. The classical
// lower kinematic pairs, shipped incrementally — the type is the *normal
// form* a constraint stack would converge to.
enum class MateType {
  Fastened,    // nothing free
  Revolute,    // rotation about Z free (`rotation` is the DOF value); `offset`
               //   is a fixed axial slide along Z (the pose parameter that
               //   places B up/down the shaft — a hinge pin's seat depth), not
               //   a free DOF
  Slider,      // translation along Z free (`offset` is the DOF value)
  Cylindrical, // rotation about Z AND translation along Z free (both DOF)
  Planar,      // faces coincident; free to slide in-plane (`dx`,`dy`) + spin
               //   about Z (`rotation`); `offset` = the fixed plane gap
  Ball,        // origins coincident; free 3-axis rotation (`ballRotation`)
};

// A mate: one connection between two connector frames, with one type. The
// solve places `b`'s occurrence so that b's world frame equals a's world
// frame composed with Flip?·Rz(rotation)·Tz(offset). Every mate carries the
// same three adjustment fields; the type decides which are fixed parameters
// and which is the live DOF — so a mate always stores its DOF pose and a
// file re-solves to identical placements (the A2 explicit-transform
// principle, one level up).
struct Mate {
  uint64_t id = 0;  // stable, shares the assembly's id counter with occurrences
  std::string name; // display label (cosmetic)
  MateType type = MateType::Fastened;
  Connector a;
  Connector b; // the side the solve places (relative to a)

  bool flipped = true;   // reverse b's Z against a's Z (two touching planar
                         // faces have opposing normals, hence default true)
  double rotation = 0.0; // twist about Z, radians: the revolute/cylindrical
                         // DOF, and on every other type — FASTENED included
                         // — the authored twist about the connector axis.
                         // flipped says which side, this says which way up.
  double offset = 0.0;   // translation along +Z of a's connector frame (a's
                         // outward face normal): slider/cylindrical DOF, the
                         // revolute axial seat, the planar plane gap. A
                         // POSITIVE offset moves b along a's +Z — see the
                         // MateAdjust composition (T is applied in a's frame).
  double dx = 0.0;       // in-plane X translation (planar DOF)
  double dy = 0.0;       // in-plane Y translation (planar DOF)
  glm::dquat ballRotation{1.0, 0.0, 0.0, 0.0}; // free rotation (ball DOF)

  // Transient solve status (mirrors Feature::m_error): stamped by the
  // placement pass, cleared on every solve, never serialized. Empty = ok.
  std::string error;
  // Transient too: when this mate closes a loop that did not close, ITS OWN
  // closure error in mm (0 otherwise) — the number its `error` quotes.
  double residualMm = 0.0;
};

// One occurrence's displacement within an exploded view (A4): a world-space
// offset, scaled by the transient explode factor at render time.
struct ExplodeStep {
  uint64_t occurrenceId = 0;
  glm::dvec3 offset{0.0}; // world space, at factor 1.0
  // v130: a moving part INSIDE a flexible placement, named as a mate is
  // ({path, mate}, Relation::mateAPath): `path` is [the placement] and
  // `occurrenceId` the sub-assembly's id for the part. Empty = an occurrence
  // of this document. Its offset adds to the placement's own step.
  std::vector<uint64_t> path;
};

// A named, persistent exploded view (A4). PURE VIEW STATE: applying it never
// touches occurrence transforms, mates, or the solve — the offset composes
// into the rendered instance transform only, so a saved exploded view cannot
// corrupt placements by construction. The active view + factor are transient
// (AppState), not stored.
struct ExplodedView {
  uint64_t id = 0;                // stable, shares the assembly's id counter
  std::string name;               // display label (cosmetic)
  std::vector<ExplodeStep> steps; // at most one per {path, occurrence}
};

// A relation couples two mates' degrees of freedom
// (docs/assemblies/relations.md): dofB = ratio * dofA + phase. The kind
// is a label and a unit convention on that one equation — a gear is
// rotation-to-rotation with ratio -zA/zB, a rack is the pinion's rotation to
// the rack's slide with the pitch radius as ratio (mm per radian), a screw
// is one cylindrical mate's own rotation to its own offset (pitch / 2pi),
// and linear is slide-to-slide. Enforced by propagation in a mate tree and
// as a residual row inside a loop.
// gear:  rotation -> rotation, ratio -zA/zB (meshing teeth reverse)
// chain: rotation -> rotation, ratio +zA/zB (a chain or a belt does NOT)
// rack:  rotation -> offset, ratio = pitch radius (mm per radian)
// screw: one cylindrical mate's rotation -> its own offset, pitch/2pi
// linear: offset -> offset
// cam:   rotation -> offset OR rotation through a LIFT TABLE, not a ratio
//        (v102: a follower may turn — a rocker, a pallet fork)
// freewheel: rotation -> rotation that drives ONE way and slips the other
//        (v104, docs/assemblies/relations.md#freewheel):
//        b = ratio * a + phase + lead
// click: ONE mate's DOF that may only travel one way — a pawl (v105)
// worm:  rotation -> rotation on crossed axes, ratio +-starts/teeth with the
//        sign from the worm's hand and where it sits (v146,
//        docs/assemblies/relations.md#worm)
// linear_n: sum of coefficient * dof over SEVERAL mates = constant (v171,
//        docs/assemblies/relations.md#relations-over-several-mates)
// epicyclic: Willis over {sun, ring, carrier}, stored as that linear_n
//        relation with the tooth counts it came from
// planet: a planet's own turn on its carrier, over {sun, carrier, planet}
// intermittent: rotation -> rotation OR offset that steps ONCE per period of
//        a, during a window, and stands still otherwise: a carry, a Geneva
//        (v174, docs/assemblies/relations.md#intermittent)
enum class RelationKind {
  Gear,
  Rack,
  Screw,
  Linear,
  Chain,
  Cam,
  Freewheel,
  Click,
  Worm,
  LinearN,
  Epicyclic,
  Planet,
  Intermittent
};
enum class MateDof { Rotation, Offset };

// One member of a relation over several mates (v171): the relation is
// sum(coefficient * dof) = constant, the constant held in Relation::phase.
// An epicyclic's terms are {sun, ring, carrier} in that order and a
// planet's {sun, carrier, planet} — the member a walk solves for last is
// the last; `teeth` is the member's tooth count
// where the kind has one (0 for a carrier, and for every linear_n term).
struct RelationTerm {
  uint64_t mate = 0;
  std::vector<uint64_t> path; // as Relation::mateAPath reads
  MateDof dof = MateDof::Rotation;
  double coefficient = 1.0;
  int teeth = 0;
};

struct Relation {
  uint64_t id = 0;  // stable, shares the assembly's id counter
  std::string name; // display label (cosmetic)
  RelationKind kind = RelationKind::Gear;
  uint64_t mateA = 0;
  uint64_t mateB = 0; // may equal mateA (a screw)
  // v129: a mate is named by {path, mate}. The path is the occurrence ids
  // from this document down to the FLEXIBLE placement whose sub-assembly
  // holds the mate, and the id is that sub-assembly's; empty = a mate of
  // this document. An engine's chain drives the pump camshaft's own mate.
  std::vector<uint64_t> mateAPath;
  std::vector<uint64_t> mateBPath;
  MateDof dofA = MateDof::Rotation;
  MateDof dofB = MateDof::Rotation;
  double ratio = -1.0;
  double phase = 0.0; // in dofB's unit (radians or mm)
  // Cam only (v88): the lift profile, as (angle in DEGREES, lift in the
  // FOLLOWER's unit — mm for a slide, DEGREES for a turn, v102) rising by
  // angle and spanning one full turn. Stored as authored; the solve converts
  // a turn's degrees where it reads them (RelationValue, the loop row).
  // Interpolated by a periodic monotone cubic (Hermite, Fritsch-Carlson
  // limited tangents), so the follower's motion has a continuous slope and
  // never overshoots the table — the loop solver differentiates this
  // numerically, and a table read as a polyline would hand it a Jacobian
  // that jumps at every sample.
  //
  // A cam is the one relation that is NOT invertible: many angles give the
  // same lift, so propagation runs forward only and the reverse direction is
  // refused rather than picking a branch.
  std::vector<std::pair<double, double>> lift;
  // Freewheel only (v104): the direction b is DRIVEN in (+1 or -1), and
  // `lead`, how far b has run ahead of the input's drive line in b's unit.
  // A freewheel's pose depends on history, and `lead` is all of it: it only
  // ever grows in the `drives` direction, and propagation keeps
  // b = ratio * a + phase + lead exact by updating it — so propagating
  // again changes nothing, and the document (and its undo snapshots)
  // carry the history with the mates.
  int drives = 1;
  double lead = 0.0;
  // A freewheel's TEETH (v105): with a count, it re-engages only where teeth
  // meet, so the drive line stands at the notch at or behind `lead` and the
  // input travels up to a pitch before it drives again — a ratchet's dead
  // band. `ride` is what riding over them lifts: whenever the clutch stands
  // off its notch the named DOF is raised by the whole `rideAmount`, and
  // seated at a notch by nothing. It is applied after the walk, so it ADDS
  // to whatever drove that DOF
  // (generators/watch/README.md#keyless-works-and-winding).
  int teeth = 0;
  double rideAmount = 0.0;
  uint64_t rideMate = 0;
  MateDof rideDof = MateDof::Offset;
  double rideApplied = 0.0; // what the last pass added, to take back
  uint64_t threadPart = 0, threadFeature = 0; // authoritative screw thread
  // A gear's or a chain's tooth counts (v171), when its ratio was derived
  // from them: 0 = a bare ratio. `internal` is a gear meshing INSIDE a ring
  // (a planet in its ring), which turns the same way: ratio +teethA/teethB.
  int teethA = 0, teethB = 0;
  bool internal = false;
  // linear_n, epicyclic and planet (v171): the members, and mateA/mateB are
  // unused (0). `phase` is the relation's constant.
  std::vector<RelationTerm> terms;
  // Intermittent only (v174): b advances `step` (the FOLLOWER's unit,
  // DEGREES for a turn, mm for a slide, as a cam's lift) once per
  // `periodDeg` of a, during the window [startDeg, startDeg + durationDeg)
  // of each period, and stands still for the rest. `slots` > 0 is an
  // external Geneva's exact law over the window (step -360/slots, window
  // 180 - 360/slots, both derived); 0 is a uniform step, a gear-driven
  // carry. A pure function of a — the count is in a's turns, so nothing
  // here is history. `table` (v184), when not empty, is the law over the
  // window as (driver fraction, step fraction) points from (0, 0) to
  // (1, 1), both monotone, linear between them: a follower that keeps to
  // a driver's face, as an escape wheel to its pallet's impulse face.
  struct Intermittent {
    double step = 0.0;
    double periodDeg = 360.0;
    double startDeg = 0.0;
    double durationDeg = 0.0;
    int slots = 0;
    std::vector<std::pair<double, double>> table;
  } intermittent;
  bool HasTerms() const {
    return kind == RelationKind::LinearN || kind == RelationKind::Epicyclic ||
           kind == RelationKind::Planet;
  }
  std::string error; // set by the solve: a mate gone, a DOF missing
  // Transient too: a relation over several mates that the solve had to
  // complete by holding a member nothing drove ("ring (mate 7) held where it
  // stands"). Empty when every member it needed was driven.
  std::string held;
  // ...and which terms those were: a loop pass PINS them where propagation
  // held them, or its least squares would spread the turn over every member
  // and disagree with the tree.
  std::vector<size_t> heldTerms;
};

// Where an intermittent relation puts b (in b's unit as the solve holds it:
// radians for a turn, mm for a slide) for a at `aRad`, phase excluded
// (docs/assemblies/relations.md#intermittent). `IntermittentLaw` is the
// window's 0..1 progress at fraction t of it.
double IntermittentLaw(const Relation::Intermittent& i, double t);
// Why `table` is not an intermittent law (empty when it is): at least two
// points, from (0, 0) to (1, 1), the driver's fractions rising and the
// step's never falling.
std::string
IntermittentTableError(const std::vector<std::pair<double, double>>& table);
double IntermittentValue(const Relation& r, double aRad);

// The lift a cam profile gives at `angleRad`, and its slope (mm per radian).
// Periodic over one turn: the table wraps, so a crank turning past 360 sees
// the same profile again. An empty table is a zero lift.
double CamLift(const std::vector<std::pair<double, double>>& table,
               double angleRad, double* slopeOut = nullptr);

// A motion study (v87, docs/assemblies/README.md#motion-studies): ONE mate's
// degree of freedom driven from `from` to `to` in `samples` steps, the assembly
// solved at each, and every occurrence pair checked for a clash. The definition
// is persisted; the samples and the collision table are not — they are a
// product of running it, like a FEM result.
//
// "The animation is the deliverable": a crank turned through 720 degrees
// with sixteen pistons following is the thing a W16 exists to show, and it
// is worthless unless every one of those poses has been checked. So the
// check is part of the definition, not a separate verb someone has to know
// to run.
struct MotionStudy {
  uint64_t id = 0;         // stable, shares the assembly's id counter
  std::string name;        // display label (cosmetic)
  uint64_t driverMate = 0; // the mate whose DOF is driven
  // v129: the flexible placement the driver mate is inside; empty = a mate
  // of this document (Relation::mateAPath says how a path reads).
  std::vector<uint64_t> driverPath;
  MateDof driverDof = MateDof::Rotation;
  // Range in the DOF's own unit: RADIANS for a rotation, mm for an offset —
  // the same unit `set_mate` takes, so nothing here converts. The RPC takes
  // degrees for a rotation and converts once, at the boundary.
  double from = 0.0;
  double to = 0.0;
  int samples = 48; // poses, `to` inclusive; 720 degrees at 15 = 49
  bool checkClash = true;
  // "mesh" (the default: triangles, the speed a sweep needs) or "exact"
  // (the boolean, at a boolean's price, for a final gate).
  std::string clashMode = "mesh";
  // The mesh check's deflection, mm (v167): 0 derives it per body, 0.2% of
  // its bounding diagonal. A mechanism whose running gaps are finer than
  // that (print-in-place gears at 0.1 mm a side, where a 100 mm ring meshes
  // at 0.2 mm) says so here, or its triangles are coarser than the gap they
  // decide. Mesh mode only.
  double clashDeflectionMm = 0.0;
  // The configuration the study runs under (v128): an id, or 0 for the BASE
  // (no configuration); unset = whichever is active when it runs. A study
  // that measures "full load" has to SAY full load, or it measures whatever
  // the last person left switched on — and one that must run with nothing
  // held has to be able to say "base", which is why unset and 0 differ.
  std::optional<uint64_t> configuration;
  // v179: how many times a step may be halved where a free joint
  // coordinate jumps between samples (docs/assemblies/README.md#branch-jumps):
  // the default goes down to a 64th of the driver's step. 0 judges each
  // step as it is, refining nothing.
  static constexpr int kDefaultMaxRefine = 6;
  static constexpr int kMaxMaxRefine = 12;
  int maxRefine = kDefaultMaxRefine;
};

// Why `study`'s clash_deflection_mm cannot stand, or empty when it can: it
// must be a finite positive number of millimetres, and it means something
// only to the mesh check, so an exact study that carries one is refused
// rather than quietly ignoring it. One rule for the RPC and the loader.
std::string MotionStudyDeflectionProblem(const MotionStudy& study);

// A CLEARANCE the designer declared, as an assembly saved before v160 holds
// it. It loads, and is moved into the project's requirements on open
// (docs/requirements/README.md#clearance-assertions): a declared clearance
// is a clearance requirement now, and an assembly writes none.
//
// The drone's `verify()` says "prop tip to arm is at least 2 mm" in a python
// script, and so does the engine's, and the clamp's. That is design intent
// living outside the design: nobody who opens the document can see it, the
// GUI cannot break it, and editing a plate thickness re-checks nothing. A
// number a script asserts once is not a constraint — it is a reading somebody
// took.
//
// DECLARED, NOT DETECTED. The assertion names its two subjects and its floor
// in the designer's own terms; nothing here infers what they meant from the
// geometry. vcad's snap-fit pack draws the same line and says why — automatic
// detection of the beam "is not attempted; the designer declares the beam and
// the arithmetic is enforced" — because inferred intent is wrong at exactly
// the edges that matter, which is where a checker starts disagreeing with the
// person using it.
struct ClearanceAssertion {
  uint64_t id = 0;  // stable, shares the assembly's id counter
  std::string name; // what this clearance IS ("prop tip to arm")
  // The two subjects, as TOP-LEVEL OCCURRENCE IDS — the same key a mate
  // stores, and for the same reason. A stored name would repoint silently
  // when an occurrence is renamed and break when one is deleted; an id does
  // neither. An id that no longer resolves is UNVERIFIABLE, which is the
  // honest answer and the one a name cannot give.
  uint64_t a = 0;
  uint64_t b = 0;
  double minMm = 0.0; // the floor that must hold
  // "mesh" (triangles, and the question is usually millimetres not microns)
  // or "exact". Mesh carries a tolerance, and verification SUBTRACTS it
  // rather than trusting the distance flat — the drone's gap() already does
  // this by hand, because each mesh can err its own way.
  std::string mode = "mesh";
  std::string why; // why this number, in the designer's words. Optional.

  // The last verification, so listing is cheap and staleness is visible
  // without re-running a sweep. `checkedVersion` is the assembly version the
  // answer was taken at: not equal to the current one means STALE, which is
  // neither a pass nor a failure — the W2 rule, one level up.
  std::string verdict;     // "" (never), "holds", "violated", "unverifiable"
  double measuredMm = 0.0; // what it was when last checked
  // What the answer was an answer ABOUT: a hash of the two subtrees' placed
  // geometry. Not equal to the current one means STALE, which is neither a
  // pass nor a failure.
  //
  // Content, deliberately, and not the assembly's version counter. Fifteen
  // mutators bump that counter and a post-load placement pass is one of them,
  // so every assertion would read stale on open and after every unrelated
  // edit — crying wolf, which costs what a false pass costs. A counter also
  // means nothing across files, and this codebase has already paid once for
  // treating a document version as an identity (ee247f9). A hash of the
  // placement travels, and is silent when nothing that matters moved.
  uint64_t checkedKey = 0;
  std::string note; // why it could not be checked, when it could not
};

// An assembly document: an ordered list of occurrences that *place* the bodies
// of other parts by explicit transform, plus the mates that constrain them
// (re-solved over non-`fixed` occurrences by a placement pass). A
// distinct document kind from a part's feature-list Document — an occurrence
// references another part, which Feature/RegenContext (same-document only)
// cannot reach, so an assembly regenerates by a placement pass, not by
// executing features.
namespace fem {
struct StudyRuntime;
struct StudyRuntimeCache;
} // namespace fem
namespace motion {
struct Result;
} // namespace motion
// The motion results an assembly carries, by study id (AssemblyDocument::
// MotionResult).
struct MotionResultCache;

// One mate value a pose step sets
// (docs/assemblies/README.md#assembly-sequences): a revolute's or cylindrical's
// angle, a slider's or cylindrical's travel.
struct SequencePose {
  uint64_t mateId = 0;
  MateDof dof = MateDof::Rotation;
  double value = 0.0; // degrees for a rotation, mm for an offset
};

// One step of an assembly sequence
// (docs/assemblies/README.md#assembly-sequences): the occurrences it places
// and the path they go home along -- one, or a GROUP that moves as one
// along one path -- or, with no occurrences, a POSE: mate values set, the
// assembly solved there, and every later step checked at it. Checked
// against every occurrence placed before it, not only its mate partner.
struct SequenceStep {
  std::vector<uint64_t> occurrences; // empty: a pose step
  std::vector<SequencePose> pose;    // a pose step's values
  // The way it moves going home, in the assembly's frame (unit). Unset: the
  // axis of the part's own cylinder that its mate seats, both ways tried
  // (a group's: its members' common one).
  std::optional<glm::dvec3> direction;
  // A point on the path's line, assembly frame. Unset: the cylinder's axis,
  // or the part's centre with an explicit direction.
  std::optional<glm::dvec3> origin;
  // The mate whose seat gives the default path; 0: the one mate joining it
  // to an occurrence placed before it.
  uint64_t mateId = 0;
  double distanceMm = -1.0; // from its seat to where it starts; < 0: clear
  double turnDeg = 0.0;     // turned about the path over it (a bayonet)
  bool thread = false;      // it goes on by its thread
  std::string note;         // what the step is, for a person
  bool IsPose() const { return occurrences.empty(); }
};

struct AssemblySequence {
  std::vector<SequenceStep> steps;
  // How much nearer faces come as made (check_insertion's allowance_mm).
  double allowanceMm = 0.0;
  bool Empty() const { return steps.empty(); }
};

class AssemblyDocument {
public:
  // The order its parts go together in, and how (v187; groups and poses
  // v188).
  const AssemblySequence& GetSequence() const { return m_sequence; }
  void SetSequence(AssemblySequence s) { m_sequence = std::move(s); }

  // The units this assembly is READ in — same rule and same storage
  // invariant as Document::Units (core/Units.h). The BOM's masses are the
  // reason an assembly needs its own: it is where they are shown.
  const units::System& Units() const { return m_units; }
  void SetUnits(const units::System& u) {
    m_units = u;
    MarkEdited();
  }

  const std::vector<Occurrence>& GetOccurrences() const {
    return m_occurrences;
  }
  std::vector<Occurrence>& GetOccurrences() { return m_occurrences; }
  size_t OccurrenceCount() const { return m_occurrences.size(); }

  // Appends an occurrence, assigning a fresh stable id (the caller-supplied
  // `id` is ignored/overwritten). Returns the assigned id.
  uint64_t AddOccurrence(Occurrence occ);

  // Removes the occurrence with this id; returns false if absent.
  bool RemoveOccurrence(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  Occurrence* FindById(uint64_t id);
  const Occurrence* FindById(uint64_t id) const;

  const std::vector<Mate>& GetMates() const { return m_mates; }
  std::vector<Mate>& GetMates() { return m_mates; }
  size_t MateCount() const { return m_mates.size(); }

  // Appends a mate, assigning a fresh stable id from the SAME counter as
  // occurrences — ids are unique across the whole assembly, so an RPC/file/
  // error message naming "id 7" is unambiguous. Returns the assigned id.
  uint64_t AddMate(Mate mate);

  // Removes the mate with this id; returns false if absent.
  bool RemoveMate(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  Mate* FindMateById(uint64_t id);
  const Mate* FindMateById(uint64_t id) const;

  const std::vector<ExplodedView>& GetExplodedViews() const {
    return m_explodedViews;
  }
  std::vector<ExplodedView>& GetExplodedViews() { return m_explodedViews; }

  // Appends an exploded view, assigning a fresh stable id from the shared
  // counter. Returns the assigned id.
  uint64_t AddExplodedView(ExplodedView view);

  // Removes the exploded view with this id; returns false if absent.
  bool RemoveExplodedView(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  ExplodedView* FindExplodedViewById(uint64_t id);
  const ExplodedView* FindExplodedViewById(uint64_t id) const;

  // Relations (v83, docs/assemblies/relations.md): same id space and
  // the same add/remove/find discipline as mates and exploded views.
  const std::vector<Relation>& GetRelations() const { return m_relations; }
  std::vector<Relation>& GetRelations() { return m_relations; }
  size_t RelationCount() const { return m_relations.size(); }
  uint64_t AddRelation(Relation relation);
  bool RemoveRelation(uint64_t id);
  Relation* FindRelationById(uint64_t id);
  const Relation* FindRelationById(uint64_t id) const;

  // Motion studies (v87): the same id space and discipline.
  const std::vector<ClearanceAssertion>& GetAssertions() const {
    return m_assertions;
  }
  std::vector<ClearanceAssertion>& GetAssertions() { return m_assertions; }
  uint64_t AddAssertion(ClearanceAssertion a);
  bool RemoveAssertion(uint64_t id);
  ClearanceAssertion* FindAssertionById(uint64_t id);

  const std::vector<MotionStudy>& GetMotionStudies() const {
    return m_motionStudies;
  }
  std::vector<MotionStudy>& GetMotionStudies() { return m_motionStudies; }
  uint64_t AddMotionStudy(MotionStudy study);
  bool RemoveMotionStudy(uint64_t id);
  MotionStudy* FindMotionStudyById(uint64_t id);
  const MotionStudy* FindMotionStudyById(uint64_t id) const;

  // Directly set the next-id counter (used by the loader so restored ids are
  // never reissued). No-op if `next` is not greater than the current counter.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId)
      m_nextId = next;
  }

  // ---- FEM studies (docs/simulation/README.md#assembly-studies) ------------
  // A study is a question asked of the assembly and lives on it, the way a
  // part's studies live on the part: participants by occurrence, loads with
  // occurrence-qualified anchors. The same fem::Study record; the shape of
  // the accessors is Document's. Ids come from the shared counter.
  const std::vector<fem::Study>& GetStudies() const { return m_studies; }
  std::vector<fem::Study>& GetStudies() { return m_studies; }
  const fem::Study* FindStudy(uint64_t id) const;
  fem::Study* FindStudy(uint64_t id);
  fem::Study& AddStudy(const std::string& name = ""); // bumps the version
  bool RemoveStudy(uint64_t id);                      // bumps the version
  void RestoreStudy(fem::Study study);         // load/undo: keeps its ids
  uint64_t AllocateId() { return m_nextId++; } // a load's id
  // Derived mesh + results, saved beside each study (v153), SHARED with
  // every full-state copy of this document (the undo snapshots): results are
  // not document state, and a snapshot that duplicated a 2.5M-DOF mesh would
  // be the reason nobody could undo.
  const fem::StudyRuntime* StudyResults(uint64_t studyId) const;
  void StoreStudyResults(uint64_t studyId, fem::StudyRuntime rt);
  uint64_t StudyResultsEpoch() const;
  // Stale against the assembly AND every part the solve read: `partVersion`
  // answers a part id with its current version (the drawing's source rule).
  bool StudyResultsStale(
      uint64_t studyId,
      const std::function<uint64_t(uint64_t partId)>& partVersion) const;
  // Stamps a stored result as solved at this version and these part
  // versions: a reopened project's whose inputs hash as they did when it
  // was made. False when absent.
  bool MarkStudyResultsCurrent(
      uint64_t studyId,
      std::vector<std::pair<uint64_t, uint64_t>> partVersions);

  // The motion studies' results (docs/assemblies/README.md#motion-studies),
  // saved with the project beside each study and read back on open. Derived
  // like StudyResults and shared with the undo snapshots the same way; what
  // reads them in a session is the app's (ui::MotionResultFor), which
  // installs these and keeps them in step.
  std::shared_ptr<const motion::Result> MotionResult(uint64_t studyId) const;
  // Null erases.
  void StoreMotionResult(uint64_t studyId,
                         std::shared_ptr<const motion::Result> result);
  std::vector<uint64_t> MotionResultIds() const;

  // Configurations: named assembly variants. Reuses the Configuration type —
  // for an assembly, `suppressedFeatures` holds suppressed OCCURRENCE ids (a
  // suppressed occurrence is hidden from render / solve / BOM). Parameter
  // overrides are unused (an assembly has no parameters). An occurrence picks
  // one of the configurations of what it places, a part's or a
  // sub-assembly's (Occurrence::configurationId, 0 = that document's active
  // one); a part placed in one is a regenerated copy
  // (ui::ConfiguredPartDocument). The active configuration (0 = base) is
  // persisted.
  const std::vector<Configuration>& GetConfigurations() const {
    return m_configurations;
  }
  std::vector<Configuration>& GetConfigurations() { return m_configurations; }
  const Configuration* FindConfiguration(uint64_t id) const;
  Configuration* FindConfiguration(uint64_t id);
  Configuration& AddConfiguration(const std::string& name = "");
  bool RemoveConfiguration(uint64_t id);
  void RestoreConfiguration(Configuration cfg); // load/undo, keeps id

  uint64_t ActiveConfiguration() const { return m_activeConfiguration; }
  // Sets the active id (0 = base) and bumps the version so renderers resync;
  // returns false for an unknown non-zero id.
  bool SetActiveConfiguration(uint64_t id);
  void SetActiveConfigurationRaw(uint64_t id) { m_activeConfiguration = id; }
  const Configuration* ActiveConfig() const;

  // Whether an occurrence is hidden by the active configuration (its base
  // `visible` flag is separate; this is the config layer). Render/solve/BOM
  // skip an occurrence that is suppressed OR not visible.
  bool OccurrenceSuppressed(uint64_t occurrenceId) const;

  // The same question against a NAMED configuration (A5): 0 means the active
  // one, and an id that no longer resolves also falls back to it — a
  // configuration deleted out from under a placement should not silently
  // un-suppress half of it, but it must not break the model either.
  bool OccurrenceSuppressedIn(uint64_t configurationId,
                              uint64_t occurrenceId) const;

  // The configuration a placement asked for (v129): 0 = the active one, and
  // an id that no longer resolves falls back to it, as above. nullptr = the
  // base.
  const Configuration* ConfigurationFor(uint64_t configurationId) const;
  // MateSuppressed / RelationSuppressed against a NAMED configuration, the
  // way a flexible placement runs its sub-assembly (0 = active).
  bool MateSuppressedIn(uint64_t configurationId, uint64_t mateId) const;
  bool RelationSuppressedIn(uint64_t configurationId,
                            uint64_t relationId) const;

  // Whether the active configuration takes a mate out of the solve: it
  // suppresses the mate, or an occurrence the mate connects (v101). A mate
  // touching a hidden occurrence used to go on driving it.
  bool MateSuppressed(uint64_t mateId) const;
  // ...a relation: suppressed itself, or coupling a mate that is out.
  bool RelationSuppressed(uint64_t relationId) const;
  // The active configuration's hold on a mate's DOFs, or nullptr.
  const MateOverride* MateOverrideFor(uint64_t mateId) const;

  // Monotonic change counter — bumped on every add/remove/mutate — so the
  // renderer can skip resyncing an unchanged assembly (mirrors Document).
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

  // Whether the assembly has changed since it was saved — Document::SaveStamp
  // for an assembly: the version plus the saved-state edits that must not
  // look like a change to the model (the units), which MarkEdited counts. Dirty
  // tracking and autosave compare this, never Version().
  uint64_t SaveStamp() const { return m_version + m_edits; }
  void MarkEdited() { ++m_edits; }

private:
  uint64_t m_edits = 0;
  AssemblySequence m_sequence;
  std::vector<Occurrence> m_occurrences;
  units::System m_units;
  std::vector<Mate> m_mates;
  std::vector<ExplodedView> m_explodedViews;
  std::vector<Relation> m_relations;
  std::vector<MotionStudy> m_motionStudies;
  std::vector<ClearanceAssertion> m_assertions;
  std::vector<Configuration> m_configurations;
  std::vector<fem::Study> m_studies;
  std::shared_ptr<fem::StudyRuntimeCache> m_studyRuntime; // see StudyResults
  std::shared_ptr<MotionResultCache> m_motionResults;     // see MotionResult
  uint64_t m_activeConfiguration = 0;
  uint64_t m_nextId = 1; // stable ids grow from 1, never reused; one id
                         // space shared by occurrences, mates, and views
  uint64_t m_version = 1;
};
