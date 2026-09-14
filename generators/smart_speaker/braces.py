"""The two internal braces, recovered from the export's own sections.

They used to be re-drawn to a rule and stated as such — "the exact mouldings
are a job for the CAD that drew it" — at 0.773 and 1.039 of the export by
volume. That was not a modelling limit. It was that the rule had been
measured off SCREENSHOTS, and three of its numbers were wrong:

    rail        8 mm, where the export has 16 (window edge at 89, not 97)
    spokes      24 mm square, where the export has 16
    ring outer  r60.5, which is its radius at the FACES — at mid-thickness
                it is 68.5, and the 8 mm between them is the rim fillet

`section {loops}` gives the real outline for the asking, and every number
below is read off it (docs/inspect/review-checklist.md; the RPC exists for
exactly this and its own description cites this brace as the cautionary
case). The blend joining each window to the ring solves exactly:
(h + R)^2 + 77.196^2 = (68.5 + R)^2 gives R = 11.000.

Both braces now carry the export's R8 blends, at half a wall thickness:

    ring brace     the rim where each of the four windows breaks out of a
                   face — 20 cylinders along the straights, 4 tori round the
                   ring, 8 general surfaces over the R11 blends, which is
                   exactly the face census of the export
    brace ladder   the four vertical corners of each slot, and then the slot
                   rims over them

The bore and the four fixings are NOT filleted, in either the export or here:
a plain cylinder meets a plain annulus. Asking for R8 round a d5.5 hole is
also impossible, and it is what "the fillet failed on 1 contour" was.

Two ordering rules the kernel enforces, both physical:

  - a fillet CONSUMES the edges it rounds, so a second pick must be taken
    after the first one runs, not gathered up front ("edge '4/g5;U3;M4' no
    longer exists" is the toponaming system doing its job);
  - the ladder's slot rims are rounded BEFORE its studs are stood on the
    panel. A stud lands across a rim and a fillet cannot run under one —
    afterwards, exactly three of the eight rim contours refuse, which is
    three studs.

What is still not reproduced: the export's spokes taper to a knife edge at
each face (its face section is TWO islands, the ring floating free of the
frame), because its rim fillets are exactly half the panel. Here they are
half a panel less `mitre_land`, so a 0.15 mm land is left — 3.3 cm3 over the
export, and the reason the ring brace reads 1.009 rather than 1.000. No
router cuts a knife edge either.
"""
import math

from designer_sdk import Plane

MDF = "mdf"

# Positions come back as float32, so an absolute tolerance has to suit the
# MAGNITUDE, not the feature. A panel face at -164.8 reads -164.800003, and
# picking edges with a 1e-6 window matched NOTHING — silently, because the
# pick list was then empty and the fillet simply never happened. A micron is
# still four orders finer than any edge this generator cares about.
NEAR = 1e-3

# The frame rail, the spokes and the ring's wall are all ONE WALL THICKNESS,
# read off the export's own mid-plane section rather than a screenshot (see
# the module docstring). They were 8 and 24 here, measured by eye, and that
# error was a third of the part: 274 cm3 against the export's 355.


def ring_brace(app, p):
    """The panel that closes the port bay and holds the tube on its axis.

    FOUR windows cut out of a solid panel, which is how the export is built —
    not one window with a ring and spokes added back inside it. That
    construction gave the same silhouette and a face full of coplanar
    imprint edges, and the rim fillets below cannot be built on those.
    """
    part = app.part("ring_brace")
    t = p["wall_thickness"]
    half_w = p["cabinet_internal_w"] / 2
    half_d = p["cabinet_internal_d"] / 2

    sk = part.sketch(Plane.xy(-t / 2), name="outline")
    sk.centered_rect((0, 0), half_w * 2, half_d * 2)
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    sk = part.sketch(Plane.xy(t / 2), name="windows")
    for sx in (-1, 1):
        for sy in (-1, 1):
            _window(sk, p, sx, sy)
    part.cut(sk.add(), distance=t, reversed=True)

    # The tube's seat. A fit, not a clearance — `port_tube_od`, so the tube
    # is held on axis by this hole rather than by whatever it is glued to.
    sk = part.sketch(Plane.xy(t / 2), name="bore")
    sk.circle((0, 0), d=p["port_tube_od"])
    part.cut(sk.add(), distance=t, reversed=True)

    # The fixings the upper port flare bolts down through. On the flare's
    # own lug circle, because that is what they are for — drawn to a rail
    # width instead they landed 0.5 mm off it by luck.
    sk = part.sketch(Plane.xy(t / 2), name="fixings")
    n = int(float(p["port_brace_bolt_count"]))
    for i in range(n):
        a = 2 * math.pi * i / n
        r = float(p["port_brace_bolt_circle"]) / 2
        sk.circle((r * math.cos(a), r * math.sin(a)), d=p["m4_t_nutt_dia"])
    part.cut(sk.add(), distance=t, reversed=True)

    _rim_fillets(part, p, t, half_w, half_d)
    return part


def _window(sk, p, sx, sy):
    """One quadrant's window, exactly as the export's mid-plane section has
    it: a rectangle whose inner corner is replaced by the ring's arc, joined
    to the two straight edges by tangent blends.

    Every number here is READ OFF that section (`section {loops}` on
    reference/ring_brace.step) rather than measured in a screenshot, which is
    what the old re-draw did — and it had the rail at 8 mm where the export
    has 16, the spokes at 24 where it has 16, and the ring at its face radius
    rather than its mid-thickness one. That was a third of the part.
    """
    h = float(p["wall_thickness"]) / 2.0          # spoke half-width
    rr = float(p["port_tube_od"]) / 2.0 + float(p["wall_thickness"])
    blend = float(p["brace_blend_radius"])
    ox = float(p["cabinet_internal_w"]) / 2.0 - float(p["wall_thickness"])
    oy = float(p["cabinet_internal_d"]) / 2.0 - float(p["wall_thickness"])

    # Where a blend of radius `blend`, tangent to the straight at h and to
    # the ring at rr, touches each of them. Its centre stands off both: at
    # h + blend from the axis and at rr + blend from the origin.
    span = math.sqrt((rr + blend) ** 2 - (h + blend) ** 2)
    k = rr / (rr + blend)
    on_ring = (span * k, (h + blend) * k)         # tangent point on the ring

    def at(u, v):
        return (sx * u, sy * v)

    def blend_arc(centre, a, b):
        """The tangent arc from a to b about `centre` — through its own
        midpoint, so the direction cannot come out inverted."""
        mx, my = centre
        d1 = (a[0] - mx, a[1] - my)
        d2 = (b[0] - mx, b[1] - my)
        bx, by = d1[0] + d2[0], d1[1] + d2[1]
        n = math.hypot(bx, by)
        mid = (mx + blend * bx / n, my + blend * by / n)
        sk.arc_through(a, mid, b)

    a = at(h, oy)
    b = at(ox, oy)
    c = at(ox, h)
    d = at(span, h)                  # blend starts on the y = h straight
    e = at(*on_ring)                 # ...and lands on the ring
    f = at(on_ring[1], on_ring[0])   # the mirrored pair, x <-> y
    g = at(h, span)

    sk.line(a, b)
    sk.line(b, c)
    sk.line(c, d)
    blend_arc(at(span, h + blend), d, e)
    # The ring itself, through its own 45 degree point.
    sk.arc_through(e, at(rr * math.cos(math.pi / 4), rr * math.sin(math.pi / 4)),
                   f)
    blend_arc(at(h + blend, span), f, g)
    sk.line(g, a)


def _rim_fillets(part, p, t, half_w, half_d):
    """Round the window rims at R = half a wall, as the export does.

    This is the taper that makes the export 60.5 across the ring at its
    faces and 68.5 at mid-thickness — a prism cannot have it, and it is why
    the export's two sections disagree by 12,000 mm2. The rim is about
    3,470 mm long over both faces and a fillet takes (1 - pi/4) R^2 off each
    millimetre of it, which is the 45 cm3 between an unfilleted brace and
    this one.

    The edges are picked by WHERE THEY LIE — in a face plane, and not on the
    outside of the panel — because there are thirty of them and no element
    name survives a change to the feature order above.
    """
    r = float(t) / 2.0 - float(p["mitre_land"])
    picks = []
    for body in part.bodies():
        for e in body.edges():
            m = e.midpoint
            if m is None or abs(abs(m[2]) - float(t) / 2.0) > NEAR:
                continue        # not in one of the two faces
            if (abs(abs(m[0]) - float(half_w)) < NEAR
                    or abs(abs(m[1]) - float(half_d)) < NEAR):
                continue        # the panel's own outline, which stays sharp
            # A closed CIRCLE in a face is a hole's rim — the bore and the
            # four fixings — and the export leaves every one of them sharp
            # (its bore is a plain cylinder meeting a plain annulus). Asking
            # for R8 round a 5.5 mm hole is also simply impossible, and it
            # is what "the fillet failed on 1 contour" was: the whole
            # feature refuses for the one contour that cannot be built.
            if e.kind == "circle":
                continue
            picks.append(e)
    # No `if picks:` guard. An empty pick is not "nothing to do", it is a
    # filter that stopped matching, and `part.fillet` says so.
    part.fillet(picks, r=r, name="rim")


def ladder(app, p):
    """The window brace under the divider, and its three support studs.

    Measured off the export rather than invented: a section runs 34,466 mm2
    through the panel and a flat 1,200 mm2 above it, which is three 20 x 20
    studs and nothing else. The panel's four slots at `window_brace_width`
    rails come to 34,344 mm2 of the same 210 x 320, so the rail width in the
    parameter table is the one the part was drawn with.

    The studs stand `brace_stud_gap` off the wall each braces — two on the
    side walls at mid depth, one on the back wall at mid width. That is what
    they are for: breaking the unsupported span of the big panels.

    The slots carry the export's two families of R8, both at half a wall:
    the four vertical corners of each slot, and the rim where each slot
    breaks out of a face. The rim pair meets in the middle of a 16 mm panel,
    so the slot wall has no straight height at all — which is why a section
    3.2 mm below the face reads the slot 1.6 mm wider than it is drawn, and
    why this generator once "measured" the rails at 23.38 and 21.76 off that
    section instead of the 25 they are.
    """
    part = app.part("brace_ladder")
    t = p["wall_thickness"]
    half_w = p["cabinet_internal_w"] / 2
    half_d = p["cabinet_internal_d"] / 2
    h = float(p["brace_ladder_h"])
    rail = p["window_brace_width"]
    stud = p["brace_stud"]
    gap = p["brace_stud_gap"]

    # The window brace, at the BOTTOM of the assembly.
    base = -h / 2
    sk = part.sketch(Plane.xy(base), name="window brace")
    sk.centered_rect((0, 0), half_w * 2, half_d * 2)
    part.extrude(sk.add(), distance=t, name="window brace")
    part.material(MDF)

    # Four windows: a rail round the outside and a rail each way through the
    # middle, so the slot is what is left of each quarter.
    sk = part.sketch(Plane.xy(base + float(t)), name="windows")
    sw = (half_w - rail * 1.5) / 2
    sd = (half_d - rail * 1.5) / 2
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx = sx * (rail / 2 + sw)
            cy = sy * (rail / 2 + sd)
            sk.centered_rect((cx, cy), sw * 2, sd * 2)
    part.cut(sk.add(), distance=t, reversed=True, name="windows")

    # The slots' R8, BEFORE the studs are stood on the panel. A stud lands
    # across a slot rim, and a fillet has no room to run under one: asked
    # afterwards, exactly three of the eight rim contours refuse, which is
    # three studs. It is also the order the part would be made in.
    _slot_blends(part, p, t, base, half_w, half_d)

    # Three studs, standing on it.
    rise = h - float(t)
    for at in ((-(half_w - stud / 2 - gap), 0.0),
               (half_w - stud / 2 - gap, 0.0),
               (0.0, half_d - stud / 2 - gap)):
        sk = part.sketch(Plane.xy(base + float(t)), name="stud")
        sk.centered_rect(at, stud, stud)
        part.extrude(sk.add(), distance=rise, mode="add", name="stud")
    return part


def _slot_blends(part, p, t, base, half_w, half_d):
    """R8 at the slots: their vertical corners, then their rims.

    In two features, not one. A corner and a rim meet at a point, and asking
    a kernel for both at once is asking it to blend a blend; separately, the
    corner fillets come first and the rim then runs over the rounded corner
    as a torus, which is exactly the 16 cylinders and 16 tori the export
    has.
    """
    r = float(t) / 2.0 - float(p["mitre_land"])
    top = float(base) + float(t)

    def pick(want):
        out = []
        for body in part.bodies():
            for e in body.edges():
                m = e.midpoint
                if m is None:
                    continue
                if (abs(m[0]) > float(half_w) - NEAR
                        or abs(m[1]) > float(half_d) - NEAR):
                    continue    # the panel's own outline stays sharp
                if want == "corner":
                    if (e.kind == "line"
                            and abs(m[2] - (top - float(t) / 2.0)) < NEAR):
                        out.append(e)
                elif abs(m[2] - top) < NEAR or abs(m[2] - float(base)) < NEAR:
                    out.append(e)
        return out

    part.fillet(pick("corner"), r=r, name="slot corners")
    # Picked AFTER that one ran, not before. A fillet consumes the edges it
    # rounds, so a rim list gathered up front refers to edges that no longer
    # exist by the time it is used — "edge '4/g5;U3;M4' no longer exists",
    # which is the toponaming system doing its job. It is also the right
    # order physically: the rim then runs over the rounded corners as the
    # tori the export has.
    part.fillet(pick("rim"), r=r, name="slot rims")


def placements(p):
    """The ladder hangs under the divider; the ring brace sits where the port
    passes through it."""
    divider_under = -(float(p["alpair_chamber_h"])
                      + 2 * float(p["wall_thickness"]))
    from port import RING_BRACE_MID
    return [
        ("brace_ladder", "brace ladder",
         (0, 0, divider_under - float(p["brace_ladder_h"]) / 2), None),
        ("ring_brace", "port ring brace",
         (0, 0, -float(p["carcass_h"]) + RING_BRACE_MID), None),
    ]
