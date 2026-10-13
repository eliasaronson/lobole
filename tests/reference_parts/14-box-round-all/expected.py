# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Steiner's formula for a ball of radius r = 3 rolled round the inner box
a' b' c' = 34 x 24 x 14: V = a'b'c' + 2r(a'b' + b'c' + c'a')
+ pi r^2 (a' + b' + c') + 4/3 pi r^3, A = 2(a'b' + b'c' + c'a')
+ 2 pi r (a' + b' + c') + 4 pi r^2, centred at (0, 0, 10); 26 faces (6 flats,
12 cylinders, 8 sphere corners)."""
import geom


def expected():
    return geom.rounded_box(40, 30, 20, 3).moved(0, 0, 10).expected(faces=26)
