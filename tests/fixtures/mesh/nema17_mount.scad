// A NEMA 17 motor plate as a download is drawn (docs/interchange/README.md#recognising-a-faceted-mesh).
// Known answers, checked by tests/test_mesh_files.cpp:
//   plate 60 x 60 x 5, from z = 0 to 5, centred on the origin
//   pilot bore r 11 ($fn 96) through, on z
//   four screw holes r 1.7 ($fn 32) through, at (+-15.5, +-15.5)
//   four counterbores r 3 ($fn 32), from z = 3 to 5, on the same centres
//   one hex nut trap (a $fn 6 cylinder, corners on r 3.2), z 0 to 2.5, at (22, -22)
$fn = 32;
difference() {
  translate([-30, -30, 0]) cube([60, 60, 5]);
  translate([0, 0, -1]) cylinder(r = 11, h = 7, $fn = 96);
  for (x = [-15.5, 15.5], y = [-15.5, 15.5]) {
    translate([x, y, -1]) cylinder(r = 1.7, h = 7);
    translate([x, y, 3]) cylinder(r = 3, h = 3);
  }
  translate([22, -22, -1]) cylinder(r = 3.2, h = 3.5, $fn = 6);
}
