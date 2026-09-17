"""Every dimension of the grill, in one table.

Derived from the cooking area outwards, the way the real thing is: the grate
decides the firebox, the firebox decides the hood and the cart, and the cart
decides where the tank stands. Nothing below is a number typed twice.
"""

# --- the thing that decides everything else -------------------------------
COOK_X = 620.0   # across the front
COOK_Y = 440.0   # front to back
BURNERS = 3

# --- stock ----------------------------------------------------------------
# A grill is several gauges: the firebox takes the heat, the cart carries the
# load, the tents and tray are consumables.
FIREBOX_STOCK = ("steel", "1.5mm")
HOOD_STOCK = ("steel", "1.2mm")
TENT_STOCK = ("stainless_steel", "1mm")
TRAY_STOCK = ("steel", "0.8mm")
PANEL_STOCK = ("stainless_steel", "1mm")   # control panel, doors
CART_STOCK = ("steel", "1mm")
SHELF_STOCK = ("steel", "1.2mm")

# --- firebox --------------------------------------------------------------
GRATE_LEDGE = 12.0                       # grate rests this far inside the rim
BOX_X = COOK_X + 2 * GRATE_LEDGE         # 644
BOX_Y = COOK_Y + 2 * GRATE_LEDGE         # 464
BOX_H = 150.0                            # base to rim
BOX_RIM = 18.0                           # rim flange, out, all round
LID_GAP = 6.0                            # the lid sits on the rim, not in it
BURNER_PORT_D = 30.0                     # burner tube through the front wall
BURNER_AXIS_Z = 55.0                     # above the firebox floor: the port
                                         # and the tube share this number
DRAIN_D = 40.0                           # grease drain, base, centre rear

# --- hood -----------------------------------------------------------------
# The flat of the lid, sized so the lid covers the box: two facet runs plus
# the flat equal the firebox's depth.
HOOD_TOP_Y = BOX_Y - 2 * 120.0 * 0.7071067811865476
HOOD_FACET = 120.0                       # front and back facets, past the bend

HOOD_FACET_DEG = 45.0                    # each facet folds this far from flat
THERMO_D = 52.0                          # thermometer bezel bore, lid top
HANDLE_D = 20.0
HANDLE_SPAN = 400.0
HANDLE_RISE = 90.0

# --- inside the box -------------------------------------------------------
TENT_LEN = COOK_Y - 20.0                 # 420, clear of both walls
TENT_LEG = 90.0                          # each side of the inverted V
TENT_DEG = 120.0                         # its included angle
TENT_TAB = 20.0                          # the end tab it hangs by
TRAY_X = 500.0
TRAY_Y = 300.0
TRAY_H = 25.0                            # the headroom under the firebox,
                                         # over the bottle's regulator
TRAY_HEM = 8.0                           # the edge a person pulls on

# --- control panel --------------------------------------------------------
PANEL_X = 690.0
PANEL_H = 150.0
PANEL_TOP = 25.0                         # flange back, under the firebox
PANEL_BOTTOM = 20.0
VALVE_BORE = 22.0
VALVE_Z = 40.0                           # above the panel's centre: the bore
                                         # and the valve share this number
IGNITER_BORE = 16.0

# --- cart -----------------------------------------------------------------
CART_X = 690.0
# The back sits BETWEEN the sides, clear of their return flanges.
BACK_X = 690.0 - 2 * (20.0 + 1.0 + 1.0)
CART_Y = 500.0
CART_H = 650.0                           # floor to firebox underside
CART_FLANGE = 20.0
SHELF_X = 650.0
SHELF_Y = 420.0                          # clear of the back panel's flanges
SHELF_WALL = 30.0
DOOR_X = 330.0
DOOR_Y = 500.0
DOOR_HEM = 10.0
SIDE_SHELF_X = 420.0
SIDE_SHELF_Y = 300.0
SIDE_SHELF_DROP = 25.0

# --- hardware -------------------------------------------------------------
BURNER_D = 22.0
BURNER_LEN = COOK_Y - 30.0
BURNER_PITCH = COOK_X / BURNERS          # 206.7 between tube centres
MANIFOLD_D = 20.0
MANIFOLD_LEN = COOK_X
VALVE_BODY = (28.0, 26.0, 26.0)
VALVE_STEM_D = 8.0
KNOB_D = 55.0
KNOB_H = 24.0
GRATE_BAR_D = 8.0
GRATE_PITCH = 22.0
GRATE_X = COOK_X / 3.0                   # three sections
GRATE_Y = COOK_Y
RACK_Y = 150.0
RACK_BAR_D = 6.0
WHEEL_D = 150.0
WHEEL_W = 35.0
AXLE_D = 14.0
FOOT_D = 40.0
FOOT_H = 25.0                            # under the shelf, which is at 30
TANK_D = 310.0
TANK_H = 480.0
TANK_COLLAR_H = 90.0
THERMO_BEZEL_D = 60.0
THERMO_STEM_D = 10.0

# --- where things sit in the assembly -------------------------------------
# Z=0 is the floor. The cart carries the firebox; the firebox carries the
# hood; the hood hinges at the firebox's back rim.
FIREBOX_Z = CART_H
HINGE_Y = BOX_Y / 2.0                    # back rim, in firebox coordinates
