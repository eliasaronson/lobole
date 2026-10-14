# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The one parameter table, and the frame everything is measured in.

The ported document carried 44 parameters SIX times — a part-scoped copy in
every wall — and they had already drifted: the W170S cut-out was corrected to
the datasheet's 148 mm in `front_wall` and left at 146.4 in the other five.
Here they are declared once, at project scope, so there is one of each.

Order matters. `declare()` walks the table top to bottom and a derived entry
may name any entry above it, which is how `alpair_cutout_dia` stays
`alpair_basket_dia + 1` in the document rather than arriving as the number 100.

## The frame

World Z is up and z = 0 is the TOP of the MDF carcass, so the cabinet hangs
down into negative z. That is the frame the original model's STEP used, and
the CAM job and the assembly transforms are both written in it; changing it
would buy nothing and invalidate both.

    z =     0   top of the top wall            <- the datum
    z =   -16   under the top wall             = wall_thickness
    z =  -136   top of the divider             = alpair_chamber_h below it
    z =  -152   under the divider
    z =  -388   W170S axis
    z =  -785   port-ring brace
    z = -1006   cabinet floor (inside)         = -(internal_h + wall)
    z = -1022   under the bottom wall
    z = -1122   the floor it stands on         = plinth_height lower

Heights are derived from the chamber they belong to, never written twice:
`CARCASS_H` is the interior plus two walls, and the W170S chamber is what is
left of it once the Alpair chamber and the port bay are taken out.
"""

from lobole.sdk import SdkError

# The table, in declaration order. (name, expression, comment) — the comment
# reaches the document, so the reason a number is what it is travels with it.
#
# Three parameters from the port are NOT here. `w170s_y = 700`,
# `w170_height = 650` and `alpair_y = 950` disagreed with the baffle sketch,
# which put the drivers at 634 and 946 above the cabinet floor and was what
# actually got built and machined. Two answers in a table is worse than one, so
# the sketch's answer is kept as the two anchors at the end of this list, and
# the parameters nothing read are gone.
TABLE = [
    # --- the box ----------------------------------------------------------
    ("cabinet_internal_h_total", "990.0",
     "87 cm W170S + 12 cm Alpair chamber"),
    ("cabinet_internal_w", "210.0", "chamber width"),
    ("cabinet_internal_d", "320.0", "chamber depth"),
    ("wall_thickness", "16.0", "MDF walls"),
    ("finish_wall_thickness", "3.6", "veneer over the MDF"),
    ("faner_width", "3.6", "veneer on the baffle, under the driver flanges"),
    ("mitre_land", "0.15",
     "the flat left at a mitred covering's edge. A 45 deg mitre on 3.6 mm "
     "ply wants a setback of exactly 3.6, which is a knife edge: the kernel "
     "refuses it (\"radius too large for the geometry\") and no router "
     "cuts one either"),
    ("alpair_chamber_h", "120.0", "the top chamber, under the top wall"),
    ("partition_wall_cutout", "4.0", "how deep a divider lets into a wall"),
    ("plinth_height", "100.0", "floor clearance for the port (= 1x port D)"),

    # --- derived: the carcass ---------------------------------------------
    ("carcass_h", "cabinet_internal_h_total + 2 * wall_thickness",
     "outside height of the MDF box, feet excluded"),
    ("carcass_half_h", "carcass_h / 2",
     "every wall is sketched about its own middle, so this is the v origin"),
    ("w170s_chamber_h",
     "cabinet_internal_h_total - alpair_chamber_h - wall_thickness",
     "what is left below the divider; it is not an independent number"),

    # --- the port ---------------------------------------------------------
    ("port_internal_dia", "100.0", "air column diameter"),
    ("port_tube_od", "105.0", "outer tube diameter"),
    ("port_flange_dia", "145.0", "mounting flange diameter"),
    # The two flares' contribution to the port's EFFECTIVE length. A plain
    # tube's two ends are worth 1.46r = 73 mm at this diameter, and using
    # that here reported 36.9 Hz for a port that tunes to 32 — enough to
    # look like a 4 Hz design error and send someone lengthening a port that
    # is already right.
    #
    # This is a Precision Port, and its flares are the reason it is one:
    # the design's planning notes chose it because "a straight 100 mm tube
    # would need 36 cm length, with pipe resonance at 476 Hz right in the
    # crossover transition band", where the flared 25 cm resonates at 686 Hz.
    # A 36 cm straight tube is Leff 433 mm, so at 250 mm physical these
    # flares are worth 183 mm. That equivalence reproduces the plan's own
    # 33 Hz at 50 L to 0.1 Hz, which is the check on it.
    #
    # Measure the real port if it matters to a hair: this is derived from a
    # design document, not from the part.
    ("port_end_correction", "183.0",
     "both flares' effective length; a plain tube would be 73 here"),
    # 221, not the 250 the plan drew. The plan sized the tube for its
    # budgeted 50 L; the cabinet came out 53.3 L net, and the tube is the
    # cheap thing to change — shortening it 29 mm lands fb back on the
    # 33 Hz target without moving the ring brace off the baffle's antinode.
    # CUT IT LONG AND TRIM: see BUILD.md. The flare's real effective length
    # is `port_end_correction`, which comes from a design document rather
    # than from the part, and the impedance saddle on the finished box is
    # the only measurement that settles it.
    ("port_length_total", "221.0", "physical tube length, before trimming"),
    # What the cabinet exists to get right, so `verify` checks it rather
    # than leaving it to whoever reads the printed number.
    ("port_target_fb", "33.0", "Visaton's alignment for a 50 L box"),
    ("port_flare_top_dia", "155.0", "upper flare, free in the bay"),
    ("port_flare_bottom_dia", "160.0", "lower flare, bolted to the floor"),
    ("port_flare_h", "39.9", "how tall one flare is"),
    ("port_spigot_od", "115.0",
     "measured off the bought lower flare: the spigot that passes through "
     "the cabinet floor"),
    ("port_flange_rim_od", "147.7",
     "the locating rim under that flange, which drops into the floor 10 mm "
     "below the seating face. It, not the spigot, is what sizes the hole — "
     "sized to the spigot instead, the rim sits 5,266 mm3 inside the floor"),
    ("port_floor_clearance", "1.0", "round the spigot, per side x2"),
    ("port_hole_radius", "11.0",
     "the radius rolled onto the port hole's UNDERSIDE. The original has "
     "one: the hole is d115 for only 5 mm of the 16, then flares to about "
     "d137. Left sharp, the flare's throat sits 2,788 mm3 inside the floor "
     "and the air turns a corner on the way out"),
    ("port_tube_insert", "13.6",
     "how far the tube plugs down inside the lower flare's throat; the "
     "export overlaps them by this much"),
    ("port_throat_protrusion", "7.49",
     "how far the lower flare's throat stands above the cabinet's inner "
     "floor. Measured off the original model's STEP assembly, which hangs "
     "that flare mouth-DOWN under the floor at z -1038.4 .. -998.5"),
    ("port_brace_bolt_circle", "130.0",
     "measured off the bought upper flare: four lugs at radius 65. The\n      ring brace is drilled to THIS, not to a rail width — it is the\n      thing the flare bolts to"),
    ("port_brace_bolt_count", "4", ""),
    ("outer_port_bolt_circle", "(128+(160-128)/2) * 1.0", ""),
    ("outer_port_bolt_count", "5", ""),
    ("outer_port_bolt_dia", "4.0", ""),

    # --- the W170S --------------------------------------------------------
    ("w170s_cutout_dia", "148.0",
     'Visaton W170S-4 datasheet: "Cut-out diameter 148 mm"'),
    ("w170s_flange_dia", "187.3", "outer flange"),
    ("driver_flange_clearance", "1.0",
     "round a driver flange, in the ply cut-out and the MDF rebate both. "
     "Cut to the flange diameter exactly, the W170S has 0.15 mm of air "
     "round it and cannot be dropped in"),
    # THE DATASHEET, not a measurement of the STEP. This said 175.0 —
    # "measured off the driver", overriding the 175.8 the ported table had —
    # and the ported table was right: Visaton's drawing (docs/drivers/
    # W 170 S_9024_9025.pdf) gives 187.3 flange, 175.8 bolt circle, 148
    # cut-out, 7 flange, 74 basket depth, and every one of those EXCEPT this
    # matches the table still. A bought part's dimensions come from the
    # people who made it.
    #
    # It is not a rounding argument either. The driver's own holes are d4.4
    # (`w170s_bolt_dia`) on M4, which is 0.2 mm of radial slack, and a
    # 0.4 mm error in the circle exceeds it: the bolts bind.
    ("w170s_bolt_circle", "175.8", "Visaton's drawing: five holes at r 87.9"),
    ("w170s_bolt_count", "5", ""),
    ("w170s_basket_depth", "74.0", "protrusion into the cabinet behind the baffle"),
    ("w170s_bolt_dia", "4.4", "TODO: threads"),
    ("w170s_flange_depth", "7.0", ""),

    # --- the Alpair -------------------------------------------------------
    ("alpair_basket_dia", "99.0", ""),
    ("alpair_cutout_dia", "alpair_basket_dia + 1",
     "basket (99) passes through; 0.5 mm clearance per side"),
    ("alpair_flange_dia", "122.0", ""),
    ("alpair_flange_depth", "4.0", ""),
    ("alpair_bolt_count", "5", ""),
    ("alpair_bolt_dia", "3.1", "TODO: threads"),
    ("alpair_bolt_circle", "112.0", ""),
    ("alpair_magnet_dia", "76.0", ""),
    ("alpair_total_depth", "62.1", ""),

    # --- where the drivers sit --------------------------------------------
    #
    # The two numbers the baffle used to hold as literals. Each is measured
    # from the thing that actually fixes it: the woofer from the floor,
    # because that is an ear-height decision and it must not move when the
    # cabinet grows; the Alpair from the top, because it lives in the top
    # chamber and is placed within it.
    ("w170s_center_from_floor", "634.0",
     "axis height above the cabinet floor (NOT above the ground: the plinth "
     "adds plinth_height)"),
    ("alpair_center_from_top", "76.0",
     "axis below the top of the carcass, inside alpair_chamber_h"),

    # --- joinery and hardware ---------------------------------------------
    ("plug_dia", "8.0", "dowel"),
    ("plug_length", "30.0", ""),
    ("plug_side_dist", "50.0", ""),
    ("m4_t_nutt_dia", "5.5", "Check this"),
    ("m3_t_nutt_dia", "4.2", "Check this"),
    ("t_nut_size", "14.8", "the square block a T-nut is set into"),
    ("t_nut_length", "25.0", ""),

    # --- feet and bracing --------------------------------------------------
    ("feet_flat_width", "wall_thickness", ""),
    ("feet_angle", "110", ""),
    ("window_brace_width", "25.0", "rail width in both braces"),
    ("brace_ladder_h", "345.6", "window brace + studs, under the divider"),
    # The studs are `window_brace_width` OUT from the wall they brace and one
    # `wall_thickness` across, and they TOUCH it — read off the export's own
    # section, which runs them to x = +/-105, the side wall's inner face.
    # They were 20 x 20 standing 2.5 mm clear here, and both errors hid
    # because 20 x 20 and 25 x 16 are both 400 mm2: the part's volume matched
    # the export and so did a section's area. Only the LOOPS showed it.
    # A stud that does not reach the wall braces nothing, and these are the
    # only members in the cabinet that shorten a panel's governing span.
    # NOT the brace's position — `port.ring_brace_mid` derives that from the
    # port's length, because the brace has to follow the tube it carries.
    # Kept because the ported document declared it and --check-drops reads
    # this table against that one.
    ("ring_brace_z", "-785.2", "vestigial: see port.ring_brace_mid"),
    # Read off the export's own mid-plane section, not a screenshot: the
    # blend that joins each window's straight edges to the ring is R11
    # exactly ((h+R)^2 + 77.196^2 = (68.5+R)^2 solves at 11.000).
    ("brace_blend_radius", "11.0",
     "window-to-ring blend on the port brace"),

    # --- the electronics, bolted to the bay floor ---------------------------
    #
    # The amp's case has four real mounting holes through the face it lies
    # on, and these three are MEASURED off it (mates.py refuses if the case
    # ever disagrees, the way the driver bolt circles are checked). The
    # ported table's `esp32_case_bolt_dist: 52` was a guess nothing read, and
    # it is not the pattern the case actually has.
    #
    # The supply has none — its only small holes are the case screws, whose
    # axes lie IN the face it stands on — so its two are ours, at a spacing
    # that clears the ends of a 142 mm brick.
    ("amp_bolt_x", "82.4", "the amp case's own mounting holes, across"),
    ("amp_bolt_y", "53.4", "and fore-and-aft"),
    ("amp_bolt_dia", "7.0", "M6 clearance, as the case is bored"),
    ("psu_bolt_dist", "100.0", "the supply's, across its 142 mm footprint"),
    ("case_bolt_dia", "4.5", "M4 clearance through the floor"),
    ("amp_y", "-110.0",
     "the amp sits FORE of the port, where the bay has room: the flare's "
     "bolt circle reaches r=72 and this is 113 out"),
    ("psu_y", "120.0", "and the supply aft of it, 130 out"),
]

# Names the port left behind that this generator does not declare, and why —
# `smart_speaker.py --check-drops` prints this against a document so a
# reappearance is noticed rather than absorbed.
DROPPED = {
    "w170s_y": "700 mm, unused; the baffle put the axis at 634 above the floor",
    "w170_height": "650 mm, unused; same disagreement as w170s_y",
    "alpair_y": "950 mm, unused; the baffle put the axis at 946",
}


def declare(part):
    """Register the table on `part`, at PROJECT scope. Returns {name: Param}.

    A Param is a float that carries its expression, so `P["wall_thickness"] /
    2` reaches the document as the formula and not as 8.0 — which is the whole
    reason the walls below can be written as ordinary arithmetic.
    """
    out = {}
    for name, expression, comment in TABLE:
        out[name] = part.param(name, expression, comment=comment,
                               scope="project")
    return out


def helmholtz_fb(p, displaced_litres=0.0):
    """Port tuning, in Hz, for the box and port the table describes.

    Reported by every run because it is the number the cabinet exists to get
    right, and because it is the one thing that changes when someone edits
    `port_length_total` to make a part fit.

    `displaced_litres` is everything standing INSIDE the woofer chamber —
    the braces, the port tube's outer envelope, the driver, the electronics.
    It matters and it is not small: 4.1 L of a 57.4 L chamber, which is 1.3
    Hz of tuning. This used to say the caller measured and passed it in, and
    no caller did, so every run printed the GROSS box's tuning as though it
    were the real one. `smart_speaker.py` measures it off the assembly now.
    Called without it you get the gross figure, which is optimistic — the
    box is always smaller than its walls suggest, never larger.
    """
    import math
    v_litres = ((float(p["cabinet_internal_w"])
                 * float(p["cabinet_internal_d"])
                 * float(p["w170s_chamber_h"])) / 1e6) - float(displaced_litres)
    if v_litres <= 0.0:
        raise SdkError(f"the woofer chamber measures {v_litres:.1f} L net — "
                       f"more is standing inside it than fits")
    r = float(p["port_internal_dia"]) / 2000.0
    length_m = float(p["port_length_total"]) / 1000.0
    # `port_end_correction` because this port is FLARED and the textbook
    # 0.85r + 0.61r is for a plain tube's two ends — see the parameter.
    effective = length_m + float(p["port_end_correction"]) / 1000.0
    area = math.pi * r * r
    return (343.0 / (2 * math.pi)) * math.sqrt(area / ((v_litres / 1000.0)
                                                       * effective))
