// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <TopoDS_Shape.hxx>
#include <glm/glm.hpp>
#include <gp_Pnt.hxx>

#include <array>
#include <vector>

// A body's volume, moments and area, face by face, each with an uncertainty
// (docs/inspect/README.md#the-face-by-face-integrator). Every volume and mass
// measure goes through it: CADShape::Volume and ComputeMassProps (cached by
// shape), and VolumeOf for a shape measured once. test_conventions.cpp keeps
// BRepGProp::VolumeProperties out of every other file.
namespace cad {

// What one face contributes, about a point `about` (Integrate uses the centre
// of the shape's box): its share of the volume by the divergence theorem
// (the position measured from `about` dotted with the face's normal, over
// three), of the first moments, of the inertia tensor about `about`
// (Ixx = int y^2 + z^2, Ixy = -int x y), and the face's area.
struct FaceFigure {
  double volume = 0.0;             // mm^3
  glm::dvec3 moment{0.0};          // mm^4
  std::array<double, 6> inertia{}; // xx yy zz xy xz yz, mm^5
  double area = 0.0;               // mm^2
};

struct IntegrateOptions {
  // Which figures to integrate: the volume group (volume, moments, inertia)
  // and the area. Each group converges on its own, so a figure is the same
  // bits whether it was integrated alone or with the other (CADShape::Volume
  // takes the volume alone, ComputeMassProps adds the area to it).
  bool volume = true;
  bool area = true;
  // The halvings a suspect face's second estimate may take after the first
  // (the pieces split at the knots). A test seam: zero leaves every suspect
  // face unconverged, its uncertainty the gap between the fixed rule and the
  // first estimate.
  int maxHalvings = 6;
  // Two successive estimates of a face agree when every figure moves by no
  // more than this fraction of the body's scale: the sum of the faces'
  // volumes' magnitudes (times the box's half diagonal for a moment, its
  // square for the inertia), and the sum of their areas. Rounding sits near
  // 2e-15 of it (docs/inspect/README.md#the-face-by-face-integrator).
  double convergence = 1e-13;
  // The fixed rule's figure stands when it is this close to the second
  // estimate, as a fraction of the same scale. Where the fixed rule is exact
  // the two sit within 6e-14; the closest a wrong one came was 4.5e-12.
  double agreement = 1e-12;
  // Keep each face's report in Integral::faces.
  bool keepFaces = false;
  // A test seam: give every face with a boundary a second estimate, as if
  // it were suspect, to measure how far the two rules sit apart where the
  // fixed one is exact.
  bool everyFaceSuspect = false;
};

// One face, as Integrate saw it.
struct FaceReport {
  int index = 0; // in TopExp_Explorer(shape, TopAbs_FACE) order
  bool suspect = false;
  FaceFigure fixed;  // BRepGProp's fixed rule on this face alone
  FaceFigure second; // the subdivided estimate (suspect faces only)
  FaceFigure used;   // what the body's figures sum
  FaceFigure uncertainty;
  int halvings = 0; // the second estimate's last level
  bool converged = true;
  bool volumeReplaced = false; // the second estimate replaced the fixed one
  bool areaReplaced = false;
  double seconds = 0.0; // CPU time of the second estimate
  // The volume's change at each level as a fraction of the body's volume
  // scale: the first from the fixed rule to the knot-split estimate, then
  // one per halving.
  std::vector<double> volumeChanges;
};

struct Integral {
  double volume = 0.0;          // mm^3
  glm::dvec3 centerOfMass{0.0}; // mm
  double inertia[3][3] = {};    // about centerOfMass, mm^5 at unit density
  double area = 0.0;            // mm^2

  double volumeUncertainty = 0.0;  // mm^3
  double centerUncertainty = 0.0;  // mm, along any axis
  double inertiaUncertainty = 0.0; // mm^5, of any component
  double areaUncertainty = 0.0;    // mm^2

  gp_Pnt about;     // the point the faces were integrated about
  FaceFigure sum;   // the faces' figures summed, about `about`
  FaceFigure scale; // the faces' magnitudes summed, about `about`
  int suspectFaces = 0;
  std::vector<int> unconverged;  // faces whose second estimate did not settle
  std::vector<FaceReport> faces; // with IntegrateOptions::keepFaces
};

// Faces are summed in explorer order, so the same shape gives the same bits
// on every run. A face whose p-curves are all lines and conics, on a surface
// with no knot line inside the face, keeps the fixed rule's figure; a body
// with no other face is measured exactly as BRepGProp measures each of its
// faces.
Integral Integrate(const TopoDS_Shape& shape,
                   const IntegrateOptions& options = {});

// The volume alone, integrated afresh: for a shape made to be measured once
// (a boolean's result, a piece of a split). A body's volume goes through
// CADShape::Volume, which caches it.
double VolumeOf(const TopoDS_Shape& shape);

// The principal moments of `m`'s inertia (about its centre) and their axes,
// unit vectors, in no particular order.
void PrincipalAxes(const Integral& m, double moments[3], glm::dvec3 axes[3]);

} // namespace cad
