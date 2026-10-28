// Half of a pipe clamp, with OpenSCAD's default resolution left on one hole.
// Known answers, checked by tests/test_mesh_files.cpp:
//   the saddle: r 12.5 inside ($fn 64), r 20 outside ($fn 64), about the x axis,
//     from x = -10 to 10, cut at z = 0 (only z >= 0 kept)
//   two ear holes r 2.25 ($fn 24), on z, at (0, +-27)
//   one default-resolution hole r 1.5 (no $fn: $fa 12, $fs 2 gives 5 sides),
//     on z, at (6, 27)
difference() {
  union() {
    intersection() {
      rotate([0, 90, 0]) translate([0, 0, -10]) cylinder(r = 20, h = 20, $fn = 64);
      translate([-10, -20, 0]) cube([20, 40, 20]);
    }
    translate([-10, -33, 0]) cube([20, 66, 4]);
  }
  rotate([0, 90, 0]) translate([0, 0, -11]) cylinder(r = 12.5, h = 22, $fn = 64);
  for (y = [-27, 27]) translate([0, y, -1]) cylinder(r = 2.25, h = 6, $fn = 24);
  translate([6, 27, -1]) cylinder(r = 1.5, h = 6);
}
