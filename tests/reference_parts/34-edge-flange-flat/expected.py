# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The blank of 33-edge-flange: the panel's flat length L - (R + t) plus the
bend allowance BA = pi/2 (R + K t) plus the 15 wall, for L = 100, R = 3,
t = 1.5, K = 0.41 (pinned), by 60 wide and t thick: V = that x 60 x t, its z
span exactly t."""
import geom

R, T, K = 3.0, 1.5, 0.41


def expected():
    flat = 100 - (R + T) + geom.PI / 2 * (R + K * T) + 15
    return geom.box(0, 0, 0, flat, 60, T).expected()
