// A knob: a chamfered skirt (a cone), a domed top (a sphere) and a bore.
// Known answers, checked by tests/test_mesh_files.cpp:
//   skirt: a cone from r 15 at z = 0 to r 12 at z = 6 ($fn 48), on z
//   body: a cylinder r 12 from z = 6 to 14 ($fn 48), on z
//   dome: a sphere r 12 about (0, 0, 14) ($fn 48), its top half
//   bore: r 3 ($fn 36), from z = -1 to 10, on z
difference() {
  union() {
    cylinder(r1 = 15, r2 = 12, h = 6, $fn = 48);
    translate([0, 0, 6]) cylinder(r = 12, h = 8, $fn = 48);
    translate([0, 0, 14]) intersection() {
      sphere(r = 12, $fn = 48);
      translate([-13, -13, 0]) cube([26, 26, 13]);
    }
  }
  translate([0, 0, -1]) cylinder(r = 3, h = 11, $fn = 36);
}
