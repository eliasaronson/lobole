# Cam profiles

A disc cam — the mechanism, not machining (`src/core/cam/` is CAM) — cut from
the motion its follower is to make. `CamProfile` takes the motion as a law
of the cam's angle and the follower it is cut for, and builds the profile
that produces that motion: the exact envelope of the follower, fitted as a
B-spline and extruded across the hub. It is the inverse of the `contact`
relation ([../assemblies/relations.md](../assemblies/relations.md#contact)),
which drives a follower from a cam's shape; a cam cut by this feature and
driven through that relation reproduces the motion it was cut for, which the
follower check measures (`deviation_mm`).

Code: `src/core/CamProfileMath.{h,cpp}` (the laws, the follower's
kinematics, the envelope, undercut; pure, so the follower check evaluates
the designed motion without a regeneration),
`src/core/features/CamProfileFeature.{h,cpp}` (the placement, the defaults,
the solid). Tests: `tests/test_cam_profile.cpp`, the GUI test
`cam_profile/tool_cuts_the_default_lobe_on_a_picked_hub`.

## Placement

- **`face`**: the cylindrical OUTSIDE of the shaft or hub the cam goes on. It
  gives the axis (the cylinder's), the cam's zero (the cylinder's X
  direction), the base radius (its radius) and the width (its length along
  the axis). A bore is refused.
- **`frame`**: a Frame feature instead — Z the axis, X the zero. A frame has
  no radius and no length, so `base_radius_mm` and `width_mm` are then
  required.

In the cam's frame the follower stands on +X at angle 0. A sliding follower
moves along +X on the line y = `offset_mm`; a turning one's pivot is at
`pivot_mm` [x, y] and its contact point swings on `arm_mm` about it, placed
where the arm meets the base circle on the follower's side. `turn` (`ccw`,
the default, or `cw`) is the sense the cam is driven in about its axis;
seen from the cam, the follower then stands at −θ (or +θ).

## The motion

Exactly one of:

- **`lift`**: a table `[[angle_deg, lift], …]` in the cam relation's form,
  wrapping one turn (angles in [0, 360), rising), interpolated as the cam
  relation interpolates it.
- **`segments`**: `[{to_deg, rise_mm | rise_deg, law}]` from 0, each ending
  where the next begins, closing the turn at 360 and rising as much as they
  fall. The laws are `cycloidal`, `harmonic`, `poly345` (3-4-5), `dwell` (no
  rise) and `drop`: a step of no width, its `to_deg` where the segment
  before it ended and its rise negative — a snail cam's face.

The rise is in the follower's unit: mm for a sliding follower, degrees for a
turning one; the other unit is refused, never converted. The lowest lift of
the turn sits on the base circle.

## The follower it is cut for

`follower {kind: roller | flat | knife, motion: slide | turn, radius_mm,
offset_mm? (slide), pivot_mm and arm_mm (turn)}`. A knife's `radius_mm` is
its tip radius (0 = sharp); kinematically a rounded knife is a roller that
does not turn. A flat face is cut for a sliding follower only.

## How it is built

The follower's reference point (a roller's or tip's centre) is computed per
degree of θ from the law and the follower's kinematics, and turned into the
cam's frame. The profile is its envelope: the pitch curve offset inward by
the radius along its normal, or for a flat face the support-function form
p = h·n + h′·t with h = R_b + s. The points are fitted with
`GeomAPI_Interpolate` — one periodic B-spline when there is no drop, one per
span between drops otherwise — and the fit's deviation from the exact
envelope at every half degree is reported. A drop's step face joins one
span's end to the next one's start: a line for a sliding follower, an arc
about the pivot (as the cam stands at the drop) for a turning one. The wire
is faced and extruded the width along the axis, as a new body (`body: new`,
the default) or fused into the picked hub's body (`body: add`). The sketch's
255-pole limit does not apply.

## Undercut is refused

A roller (or a rounded knife) cusps where the pitch curve's convex radius of
curvature is not larger than its radius; a flat face cusps where
R_b + s + s″ ≤ 0. The feature fails, naming the θ ranges and the smallest
base radius that clears them (closed form for a flat face, searched for a
roller), and builds nothing.

## Defaults derived from the pick

Picked on a hub, with nothing else stated: the base radius is the hub's
radius, the width its length, the follower a radial roller a third of the
base radius, and the motion a cycloidal rise of half the base radius over
120°, a dwell of 60°, the fall over 120° and a dwell of 60°. All of it scales
with the hub ([../ui/tool-sessions.md](../ui/tool-sessions.md#defaults-and-refusals)),
and the report's `derived` names which inputs came from the pick. A turning
follower's motion is never derived (no rise in degrees follows from a hub).

## The report

`get_state` (and `add_feature`'s response) carry `report`: `base_radius_mm`,
`width_mm`, `max_radius_mm`, `rise_mm | rise_deg`, `fit_deviation_mm`,
`max_pressure_angle_deg` and its `max_pressure_at_deg` (between the contact
normal and the follower's direction of motion), `min_convex_radius_mm`,
`min_concave_radius_mm` (when there is a hollow), `flat_face_width_mm` (a
flat face: how far the contact travels on it, the range of s′), `drops`
[{at_deg, height_mm | height_deg}], the `follower` as built, and `derived`.

## The cam relation reads it

A cam relation takes `profile: {part, feature}` in place of `lift`
(`add_relation {type: cam, profile}`, the SDK's `Asm.cam(…, profile=…)`):
on every solve it reads the motion the feature was built with, as a
thread-backed screw reads its pitch, so a design animates its intended
motion before it has a follower shaped to the contact. The relation's b
must move the way the profile's follower does (a turning follower drives a
rotation); otherwise the relation's `error` says so.

## In the app

**Cam profile** on the toolbar (`part.cam_profile`) opens the feature's tool
window and waits for the hub's face — Accept stays disabled with "pick the
shaft or hub the cam goes on" until one is picked. Its rows edit the base
radius and width (blank = from the hub), the follower's kind and radius, the
sense of turn and new body / add to hub, and show the report's pressure
angle, rise and fit deviation.
