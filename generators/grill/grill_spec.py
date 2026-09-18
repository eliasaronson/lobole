"""Every dimension of the grill, in one table — and where every part SITS.

Derived from the cooking area outwards, the way the real thing is: the grate
decides the firebox, the firebox decides the cart (it RESTS on the cart's top
flanges, so the cart is exactly as wide as it is), and the gas train decides
how much deeper than the firebox the cart has to be.

World coordinates: Z up, the origin on the floor line of the cart's side
panels, centred between them. Every number here is either a place where two
surfaces TOUCH — a seat, a weld, a bolted face — or the size of a part that
has to reach between two of those. A hole appears ONCE, in the table both
parts of its joint read, because two parts that are bolted together are the
one place a generator can put the same number in twice and not be told.
"""
import math

# --- the thing that decides everything else -------------------------------
COOK_X = 620.0   # across the front
COOK_Y = 440.0   # front to back
BURNERS = 3

# --- stock ----------------------------------------------------------------
FIREBOX_STOCK = ("steel", "1.5mm")
HOOD_STOCK = ("steel", "1.2mm")
TENT_STOCK = ("stainless_steel", "1mm")
TRAY_STOCK = ("steel", "0.8mm")
PANEL_STOCK = ("stainless_steel", "1mm")
CART_STOCK = ("steel", "1mm")
SHELF_STOCK = ("steel", "1.2mm")
BRACKET_STOCK = ("steel", "1.5mm")       # ledges, rails, ears, hinge leaves
T_FB, T_HOOD, T_TENT, T_TRAY = 1.5, 1.2, 1.0, 0.8
T_PANEL, T_CART, T_SHELF, T_BRK = 1.0, 1.0, 1.2, 1.5

# --- firebox ---------------------------------------------------------------
GRATE_CLEAR = 12.0                       # firebox wall to cooking area
BOX_X = COOK_X + 2 * GRATE_CLEAR         # 644, outside of the walls
BOX_Y = COOK_Y + 2 * GRATE_CLEAR         # 464
BOX_H = 150.0                            # the wall, past its bend
BOX_RIM = 18.0                           # rim flange, out, all round
BURNER_PORT_D = 30.0
BURNER_AXIS_Z = 55.0                     # above the base's top face
DRAIN_D = 40.0
INNER_X = BOX_X - 2 * T_FB               # 641 between the side walls
INNER_Y = BOX_Y - 2 * T_FB               # 461 between front and back

# --- cart: as wide as the firebox it carries -------------------------------
CART_H = 650.0                           # floor line to the top flanges
CART_X = BOX_X + 2 * T_CART              # 646: the sides' inner faces ARE
                                         # the firebox walls' outer faces
FRONT_GAP = 80.0                         # panel to firebox: the gas train
CART_Y = BOX_Y + FRONT_GAP               # 544
CART_FLANGE = 20.0                       # the sides' flanges, turned in
SIDE_OUT = CART_X / 2.0                  # 323: the side panels' outer faces
SIDE_IN = SIDE_OUT - T_CART              # 322
FLANGE_BOLT_X = SIDE_IN - 10.0           # 312: down the middle of a flange
CART_FRONT = -CART_Y / 2.0               # -272: the front flanges' faces
CART_BACK = CART_Y / 2.0                 # +272

# --- where the firebox sits ------------------------------------------------
FB_Y = CART_BACK - BOX_Y / 2.0           # 40: its back wall on the cart's
FB_Z = CART_H                            # base ON the sides' top flanges
FRONT_WALL_OUT = FB_Y - BOX_Y / 2.0      # -192
FRONT_WALL_IN = FRONT_WALL_OUT + T_FB
BACK_WALL_OUT = FB_Y + BOX_Y / 2.0       # 272
BACK_WALL_IN = BACK_WALL_OUT - T_FB
SIDE_WALL_IN = INNER_X / 2.0             # 320.5
BASE_TOP = FB_Z + T_FB                   # 651.5
WALL_TOP = FB_Z + T_FB + BOX_H           # 801.5
RIM_TOP = WALL_TOP + T_FB                # 803: what the lid rests on
RIM_OUT = BOX_X / 2.0 + BOX_RIM          # 340 at the sides
RIM_BACK = BACK_WALL_OUT + BOX_RIM       # 290

# --- the gas train, front to back ------------------------------------------
BURNER_D = 22.0
VENTURI_D = 28.0
VENTURI_L = 40.0
BURNER_Z = BASE_TOP + BURNER_AXIS_Z      # 706.5: port, tube and valve
BURNER_PITCH = COOK_X / BURNERS
PANEL_FACE = CART_FRONT                  # -272: the panel's BACK face, on
                                         # the sides' front flanges
PANEL_X = CART_X
PANEL_H = 170.0
PANEL_Z = 660.0                          # centre: 575 to 745
PANEL_STIFF_INSET = 34.0                 # clear of the sides' flanges
PANEL_TOP_REACH = 80.0                   # top stiffener, back to the firebox
PANEL_LIP = 15.0                         # and up its front wall, bolted
PANEL_BOTTOM_STIFF = 40.0
PANEL_TOP_Z = PANEL_Z + PANEL_H / 2.0    # 745
PANEL_BOTTOM_Z = PANEL_Z - PANEL_H / 2.0 # 575
STEM_BORE = 10.0                         # the valve stem, through the panel
VALVE_BODY = (34.0, 26.0, 26.0)          # x, y (deep), z
VALVE_Y = PANEL_FACE + VALVE_BODY[1] / 2.0   # its front face ON the panel
VALVE_SCREW_X = 12.0                     # each side of the stem
VALVE_STEM_D = 8.0
KNOB_OFF = 4.0                           # the knob stands off the face
KNOB_BORE_DEPTH = 10.0
VALVE_STEM_L = T_PANEL + KNOB_OFF + KNOB_BORE_DEPTH  # from its face
KNOB_D = 55.0
KNOB_H = 24.0
KNOB_SEAT_Y = PANEL_FACE - T_PANEL - KNOB_OFF
ORIFICE_D = 8.0
ORIFICE_L = 8.0
BURNER_MOUTH_Y = PANEL_FACE + VALVE_BODY[1] + ORIFICE_L   # on the orifice
BURNER_LEN = BACK_WALL_IN - BURNER_MOUTH_Y                # to the back wall
BURNER_TAP_DEPTH = 12.0
MANIFOLD_D = 20.0
MANIFOLD_WALL = 2.0
MANIFOLD_LEN = COOK_X
MANIFOLD_Z = BURNER_Z + VALVE_BODY[2] / 2.0 + MANIFOLD_D / 2.0  # ON the valves
VALVE_BOSS_D = 8.0                       # valve into manifold, line to line

# --- inside the firebox ----------------------------------------------------
LEDGE_LEG = 15.0                         # an L-angle, 15 x 15
LEDGE_LEN = INNER_X - 2.0                # clear of the side walls
GRATE_BAR_D = 8.0
GRATE_PITCH = 22.0
GRATE_X = COOK_X / 3.0
GRATE_Y = INNER_Y - 2.0                  # ends ON the grate ledges
GRATE_LEDGE_Z = RIM_TOP - 11.0           # 792: the grate top 3 below the rim
# Its ends land on the tent ledges' flat legs and clear their upstands: a
# tent that reaches the wall lands ON the upstand instead of beside it.
TENT_LEN = INNER_Y - 2 * (T_BRK + 1.0)
TENT_LEG = 90.0
TENT_DEG = 120.0                         # the V's included angle
TENT_LEDGE_Z = 724.0                     # over the burner, under the grate
# A resting part is SEATED, not interfering: a tenth of a millimetre of seat
# clearance, and the contact check is asked about touching to a tolerance
# that a shop would call touching.
TENT_SEAT_CLEAR = 0.1
SEAT_TOL = 0.15
RACK_Y = 150.0
RACK_BAR_D = 6.0
RACK_LEN = INNER_X - 8.0                 # ends ON the rack ledges
RACK_LEDGE_Z = RIM_TOP - 1.0             # 802: the rack clears the grate
RACK_LEDGE_LEN = RACK_Y + 20.0
RACK_CENTRE_Y = 170.0

# --- grease tray, under the firebox ----------------------------------------
TRAY_X = 500.0
TRAY_Y = 300.0
TRAY_H = 25.0
TRAY_HEM = 8.0
TRAY_RAIL_TOP = 15.0                     # welded under the firebox base
TRAY_RAIL_H = 32.0
TRAY_RAIL_LIP = 14.0                     # what the tray's floor rests on
TRAY_RAIL_LEN = 280.0
TRAY_RAIL_Y = FB_Y - 90.0                # centred under the drain
TRAY_CENTRE_Y = FB_Y - 110.0
DRAIN_Y = TRAY_CENTRE_Y
TRAY_SLIDE_X = TRAY_X / 2.0 + 4.0        # the rails' webs, clear of the tray
TRAY_FLOOR_Z = FB_Z - TRAY_RAIL_H + T_BRK

# --- hood ------------------------------------------------------------------
HOOD_FACET = 120.0
HOOD_FACET_DEG = 45.0
HOOD_RUN = HOOD_FACET * math.cos(math.radians(HOOD_FACET_DEG))
HOOD_DROP = HOOD_FACET * math.sin(math.radians(HOOD_FACET_DEG))
# The lid lands halfway across the rim, front and back, and its end caps land
# on the side rims: it covers the box the way a lid does.
HOOD_LAND = BOX_Y / 2.0 + BOX_RIM / 2.0  # 241 from the firebox's centre
HOOD_TOP_Y = 2 * (HOOD_LAND - HOOD_RUN)
HOOD_X = BOX_X + 2 * BOX_RIM             # 680: out to the rims' edges
THERMO_BORE = 12.0
THERMO_STEM_D = 10.0
THERMO_BEZEL_D = 60.0
THERMO_NUT_AF = 17.0
HANDLE_D = 20.0
HANDLE_WALL = 1.5
HANDLE_SPAN = 400.0
HANDLE_RISE = 90.0
HANDLE_PLUG = 12.0                       # a welded plug, tapped, in each end
HANDLE_Y = -(HOOD_TOP_Y / 2.0 - 40.0)    # near the lid's front edge

# --- the lid's hinge: two ears and a pin, behind the rim --------------------
HINGE_X = 300.0                          # one each side
PIN_Y = 300.0                            # behind the rim's edge (290)
PIN_Z = 815.0
PIN_D = 6.0
# The ear on the firebox: welded up the back wall, stepping outboard of the
# rim before it rises past it (the rim is at z 801.5 to 803, out to y 290).
BOX_EAR = [(BACK_WALL_OUT, 745.0), (BACK_WALL_OUT, 800.0), (291.5, 800.0),
           (291.5, 828.0), (312.0, 822.0), (312.0, 800.0), (302.0, 762.0)]
# And the ear on the lid: its first two points lie ON the back facet's outer
# face, which is where it is welded — the generator puts them there from the
# face the app reports, so a change of facet angle moves the weld.
LID_EAR_WELD = (4.0, 24.0)               # up the facet from its bottom edge
LID_EAR = [(292.0, 805.0), (312.0, 806.0), (312.0, 822.0), (288.0, 832.0)]

# --- cart parts ------------------------------------------------------------
SHELF_X = CART_X - 2 * T_CART            # walls ON the sides' inner faces
SHELF_Y = CART_Y - 2 * (CART_FLANGE + T_CART) - 4.0
SHELF_WALL = 30.0
SHELF_Z = T_CART                         # ON the sides' bottom flanges
# Between the sides, and clear of the fillet at the root of their back
# flanges: an edge that laps into that fillet is a 14 mm3 clash 640 mm long.
BACK_X = CART_X - 2 * (T_CART + 2.5)
BACK_Y = CART_BACK - T_CART - T_CART     # ON the back flanges' inner faces
BACK_BOTTOM = 3.0                        # clear of the bottom flange
BACK_TOP = CART_H - 3.0                  # and of the top one
DOOR_GAP = 4.0
DOOR_X = (CART_X - DOOR_GAP) / 2.0 - T_CART
DOOR_BOTTOM = 40.0
DOOR_TOP = PANEL_BOTTOM_Z - 5.0
DOOR_Y = DOOR_TOP - DOOR_BOTTOM
DOOR_HEM = 10.0
DOOR_FACE = CART_FRONT                   # the door's back face, on the flange
SIDE_SHELF_X = 420.0
SIDE_SHELF_Y = 300.0
SIDE_SHELF_DROP = 25.0
SIDE_SHELF_Y_C = -40.0
SIDE_SHELF_Z = CART_H                    # its top, level with the cart's

# --- the doors' hinges: a corner leaf each side, knuckles interleaved -------
HINGE_LEAF = (35.0, 20.0)                # plate: along the mount, tall
HINGE_KNUCKLE_D = 8.0
HINGE_PIN_D = 4.0
HINGE_TANG = HINGE_KNUCKLE_D / 2.0       # knuckle axis, off each mount face
HINGE_BOLT_X = (15.0, 27.0)              # along the leaf, from the knuckle
DOOR_HINGE_Z = (120.0, 480.0)            # the two knuckles, bottom of each

# --- running gear ----------------------------------------------------------
FOOT_D = 40.0
FOOT_H = 25.0
FLOOR_Z = -FOOT_H
FOOT_Y = CART_FRONT + 14.0               # forward of the shelf, so
                                         # the bolt's head clears it
WHEEL_D = 2 * (FOOT_H + 50.0)
WHEEL_W = 35.0
WHEEL_BORE = 12.2                        # running clearance on the shoulder
WHEEL_AXIS_Y = CART_BACK - 50.0
WHEEL_AXIS_Z = FLOOR_Z + WHEEL_D / 2.0
AXLE_BOLT_D = 12.0                       # the shoulder the wheel turns on
AXLE_THREAD = "M10"

# --- gas supply ------------------------------------------------------------
TANK_D = 310.0
TANK_H = 480.0
TANK_COLLAR_H = 80.0
TANK_Y = 0.0

# --- fasteners: clearance holes (ISO 273 normal) and tap drills ------------
CLEAR = {"M4": 4.5, "M5": 5.5, "M6": 6.6, "M8": 9.0, "M10": 11.0, "M12": 13.0}
TAP = {"M4": 3.3, "M5": 4.2, "M6": 5.0, "M8": 6.8, "M10": 8.5}
PITCH = {"M4": 0.7, "M5": 0.8, "M6": 1.0, "M8": 1.25, "M10": 1.5}


def burner_x(i):
    return (i - (BURNERS - 1) / 2.0) * BURNER_PITCH


def sides():
    return (-1.0, +1.0)


def hinge_bolt_z(hand, knuckle_z):
    """Where the cart's hinge leaf is bolted, for a knuckle starting there.

    The two leaves of a hinge interleave, and which half the CART's leaf
    takes is handed: the left door's cart leaf is the lower one, the right
    door's the upper. Its bolts are halfway up its own plate, so the hole in
    the side panel is 20 mm higher on the right — and the panel and the leaf
    have to agree, or the hinge bolts to nothing.
    """
    return knuckle_z + (30.0 if hand > 0 else 10.0)


# --- the bolted joints -----------------------------------------------------
# Each row is read THREE times: by the part the bolt's head sits on (which
# cuts the clearance hole), by the part behind it (which cuts the matching
# hole, or is tapped), and by the assembler (which calls fit_fastener on the
# rim). One table, so a hole cannot be moved in one part and not the other.

FIREBOX_BOLTS = [(sx * FLANGE_BOLT_X, y)            # base to the top flanges
                 for sx in sides() for y in (-110.0, 230.0)]
FIREBOX_BOLT = "M6"
BACK_BOLTS = [(sx * FLANGE_BOLT_X, z)               # back to the back flanges
              for sx in sides() for z in (150.0, 500.0)]
BACK_BOLT = "M5"
SHELF_BOLTS = [(sx * FLANGE_BOLT_X, y)              # shelf to the bottom ones
               for sx in sides() for y in (-150.0, 150.0)]
SHELF_BOLT = "M5"
PANEL_BOLTS = [(sx * FLANGE_BOLT_X, z)              # panel to the front ones
               for sx in sides() for z in (600.0, 640.0)]
PANEL_BOLT = "M5"
PANEL_LIP_BOLTS = [-150.0, 150.0]                   # panel lip to the firebox
PANEL_LIP_BOLT = "M5"
PANEL_LIP_Z = PANEL_TOP_Z + PANEL_LIP / 2.0
SIDE_SHELF_BOLTS = [-100.0, 100.0]                  # drop flange to the side
SIDE_SHELF_BOLT = "M5"
SIDE_SHELF_BOLT_Z = SIDE_SHELF_Z - 13.0
VALVE_BOLT = "M4"                                   # valve to the panel
BURNER_BOLT = "M5"                                  # back wall into the tube
HANDLE_BOLT = "M6"                                  # lid into the handle plug
FOOT_BOLT = "M6"                                    # bottom flange into a foot
HINGE_BOLT = "M4"                                   # hinge leaf to the side
