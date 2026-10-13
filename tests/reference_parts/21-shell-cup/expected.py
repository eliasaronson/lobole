# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cup: V = pi R^2 h - pi (R - t)^2 (h - t) for R = 20, h = 30, t = 2; the
area is the outer side and bottom, the rim annulus pi (R^2 - (R - t)^2),
the inner floor and the inner wall 2 pi (R - t)(h - t); five faces."""
import geom


def expected():
    outer = geom.cylinder_z(0, 0, 0, 20, 30)
    cavity = geom.cylinder_z(0, 0, 2, 18, 28)
    area = outer.area + 2 * geom.PI * 18 * 28
    return outer.minus(cavity, area=area).expected(faces=5)
