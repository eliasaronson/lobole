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
down into negative z. That is the frame the Onshape port used and the CAM job
and the assembly transforms are both written in it; changing it would buy
nothing and invalidate both.

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
    ("port_length_total", "250.0", "physical tube length with flares"),
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
    ("port_tube_insert", "13.6",
     "how far the tube plugs down inside the lower flare's throat; the "
     "export overlaps them by this much"),
    ("port_throat_protrusion", "7.49",
     "how far the lower flare's throat stands above the cabinet's inner "
     "floor. Measured off the Onshape assembly, which hangs that flare "
     "mouth-DOWN under the floor at z -1038.4 .. -998.5"),
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
    ("w170s_bolt_circle", "175.0",
     "measured off the driver: five d5 holes at radius 87.50. The port's "
     "table said 175.8, which is 0.4 mm out per bolt"),
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
    ("brace_stud", "20.0", "the three support studs are square, this side"),
    ("brace_stud_gap", "2.5", "how far a stud stands off the wall it braces"),
    ("ring_brace_z", "-785.2", "port-ring brace, from the top datum"),

    # --- the amplifier ------------------------------------------------------
    ("esp32_case_bolt_dist", "52.0", ""),
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


def helmholtz_fb(p):
    """Port tuning, in Hz, for the box and port the table describes.

    Reported by every run because it is the number the cabinet exists to get
    right, and because it is the one thing that changes when someone edits
    `port_length_total` to make a part fit.
    """
    import math
    # Net internal volume, in m^3: the gross box less the walls' share is
    # already what cabinet_internal_* describe, so this is that box minus the
    # divider and the braces, which the caller measures and passes in.
    v_litres = (float(p["cabinet_internal_w"]) * float(p["cabinet_internal_d"])
                * float(p["w170s_chamber_h"])) / 1e6
    r = float(p["port_internal_dia"]) / 2000.0
    length_m = float(p["port_length_total"]) / 1000.0
    # End correction: one flanged end, one free — 0.85r and 0.61r.
    effective = length_m + (0.85 + 0.61) * r
    area = math.pi * r * r
    return (343.0 / (2 * math.pi)) * math.sqrt(area / ((v_litres / 1000.0)
                                                       * effective))
