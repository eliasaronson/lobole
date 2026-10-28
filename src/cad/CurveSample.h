// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Adaptor3d_Curve.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace cad {

// One point of a sampled curve: its parameter and where it is.
struct CurveSample {
  double u = 0.0;
  gp_Pnt p;
};

// Points along `c`, first to last, whose chords stay within `deflection` of
// it. Empty when the curve cannot be sampled.
//
// The one way into GCPnts_QuasiUniformDeflection (tests/test_conventions.cpp
// fails on another). It judges a span by its end tangents, and before OCC 7.7
// it starts from the whole curve: a span whose two ends point the same way --
// a closed spline, a section that ends where it began -- came back as those
// two ends, a chord across the shape. Each chord is checked at its middle,
// and a sampling that strays by more than ten times `deflection` is done
// again with GCPnts_UniformDeflection, halving any chord whose middle is
// still more than `deflection` off. A sampling that holds is returned exactly
// as the quasi-uniform sampler gave it, so a curve it always sampled well
// (a CAM job's arcs, a DXF rim) comes out as it did before.
std::vector<CurveSample> SampleCurve(const Adaptor3d_Curve& c,
                                     double deflection);

} // namespace cad
