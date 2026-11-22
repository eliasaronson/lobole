# Blend bounds at circular rims, and rounds that meet across a face

## The problem

[The size that fits](../modeling/README.md#bounds-that-refuse) refuses a
blend past a bound only for a disc's own rim. Everything else is built at
the size asked first, and far past a limit that failing build is most of
what the refusal costs: r30 on the plate of 100 bores
(`tests/fixtures/step/plate_100_holes.step`, the edge set of
`tests/test_blend_fit.cpp`'s plate case: every rim on the top and the
outline's four corner arcs) spends about 4.5 s of CPU in the builder's
failure at 30, against about 0.4 s for a round that fits.

The README says the plate's limit is "the bores' 1 mm radius, which no
bound measures". The kernel says otherwise:

- The bores are R3, and a convex rim of a BORE (material outside the
  cylinder) is not limited by R at all: a round's spine runs at R + r from
  the axis. One rim of the plate builds at r6 (twice R); a 1 mm bore through
  a 20 mm plate builds at r5.
- The limit is the 2 mm land between the outer row of bores and the
  plate's sides, shared by two rounds: the four picked corner arcs pull the
  whole outline in by tangency (the builder's propagation), so the side
  edges are rounded too, and the rounds meet at 1. All 104 edges build at
  0.999 and fail at 1.0. The rims alone (no arcs) build at 1.998 and fail
  at 2.0: neighbouring bores 4 mm apart, two rounds meeting at 2.
- The bounds miss it because the Between bound counts only PICKED far
  edges, and its width line is taken square to the near edge at five
  stations, which on a rim need not point at the nearest neighbour.

The axis limit is real where the round lies on the axis side of the
cylinder or cone: a boss's or shaft's end (the disc rim, already refused), a
blind bore's floor, a tapered hole's floor, a frustum's top. There the ball
centre runs at R − (something) from the axis, and the round cannot pass it.
The kernel agrees to the thousandth: a blind bore's floor of R1 and R5
builds at 0.999R and fails at 1.001R, fillet and chamfer, and a tapered
floor and a frustum's top fail exactly past the meridian formula below. Of
those, only the convex disc refuses today; the blind bore's floor is
measured (as Between and Curvature) but searched.

## The shape of the fix

Two bounds, both of the refusing kind and both a look at analytic geometry
(no build, no five-station probe), asked before every build as
`fit::RimBounds` is now.

**Axis** (`BoundKind::Rim`, generalised). A closed circular edge whose two
faces each have a straight meridian about the circle's axis -- a plane
square to it, a coaxial cylinder, a coaxial cone. In the meridian
half-plane the faces leave the edge point P = (R, 0) along unit directions
u1, u2 (into each face), at angle φ between them. A face leaning toward the
axis (uρ < 0) reaches it R / −uρ from P, and a blend's setback on it cannot
pass that: the blend's edge on it would be a circle of negative radius. A
chamfer's setback is its distance; a round's r / tan(φ/2). So the bound is
R tan(φ/2) / −uρ for a round, R / −uρ for a chamfer, the least over the
faces that lean in. A shaft's end gives R; a frustum leaning α inward, R
tan(45° + α/2), the disc rim's R tan(θ/2); a bore's top rim nothing. It is
NOT where the ball's centre crosses the axis (R sin φ / −(u1ρ + u2ρ)): on a
cylinder meeting a 120° drill point that is R, and the builder builds 1.5R
and fails at 2R, where the cone's setback reaches its tip. This replaces
the disc-only test, which it contains.

**Meet** (`BoundKind::Meet`, new). Two edges of the blend -- picked, or
taken in by the builder's tangent propagation (`topo::EdgeBlendContours`,
the builder's own contours, added and not built) -- on one planar face,
each a line or a circle (whole or arc) whose other face has a straight
cross-section (a plane beside a line; a coaxial cylinder or cone beside a
circle), so the round's setback on the shared face is r · tan((π − ψ)/2)
everywhere along it, ψ the angle between the two faces' into-directions
(a chamfer's is its distance). Where a segment square to both edges, at
interior points of both, lies in the face (its middle inside, crossing no
boundary), the two setbacks along it cannot exceed its length g: past g /
(k1 + k2) the two blends' contact lines cross, which needs the stripes
intersected, and the builder does not intersect separate stripes. Its
sentence: "face 'F' is 2 mm wide between edge 'A' and edge 'B', so the
rounds meet at 1 mm", with "(tangent to a picked edge, so blended with
it)" after an edge the propagation brought in.

`BlendBounds` offers both too, so the search under them is seeded by them
and a refusal under them builds one candidate where that fits.

## Files

- `src/core/BlendFit.{h,cpp}`: `BoundKind::Meet`, the axis formula in
  `RimBounds`, the meet bounds, `Refuses`.
- `src/core/ElementHistory.{h,cpp}`: `topo::EdgeBlendContours`.
- `src/core/features/FilletFeature.cpp`: the pre-build look asks both.
- `tests/test_blend_fit_rim.cpp`: the kernel sweep over bores, bosses,
  blind bores, tapered holes, frustums, countersinks and land widths, both
  directions (the largest that builds is at or under the bound by the
  search's margin, and nothing at or under it is refused), plus the plate's
  refusal and its CPU budget.
- `tests/test_blend_fit_sweep.cpp`: knows the new kind.
- `docs/modeling/README.md`: the bounds and the plate's numbers.

## How it is verified

- The sweep above, in `[fillet][fit]`.
- `[.fit_sweep]` over the action matrix's and the kernel cases' bodies:
  nothing builds past a Meet or the generalised Rim bound.
- The plate: r30 is refused naming the 2 mm land and answers 0.999 by one
  build; its CPU is asserted against the one-round case's (a ratio, on one
  core, as the plate case does now), and a planted slowdown (the pre-build
  look skipped) fails it.
- `test_blend_fit.cpp`, the kernel cases and the action matrix's baseline
  give the same answers.
