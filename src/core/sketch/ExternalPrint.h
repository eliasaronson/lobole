// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a sketch's projection of an imported element was when it was picked
// (docs/sketching/README.md#projecting-an-imported-part). An Import names its
// faces and edges by their place in the file, and a changed file read again
// renumbers them: the print is what tells the element that was projected
// from whichever one now holds its index. The direct edits keep the same
// print of the faces they take (directedit::FaceFingerprint).

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct Body;
struct RegenContext;
class Feature;

namespace sketch {

struct ElementPrint {
  std::string element; // the name it was taken for: another name's is stale
  // Measure's vocabulary: "line"|"circle"|"arc"|"other" for an edge,
  // "plane"|"cylinder"|... for a face.
  std::string shape;
  double size = 0.0;        // an edge's length (mm), a face's area (mm²)
  glm::dvec3 centroid{0.0}; // its centre of mass (mm): a circle's centre
};

// Whether `name` is an imported file's own element, unchanged since: the
// seed an Import, positional, with no Derived prefix and no step after it
// that a feature made or moved it (an upstream edit moves such an element
// on purpose, by its parameters). Only these are printed.
bool IsFileOwn(const RegenContext& ctx, const std::string& name);
// The same against any feature list: what an assembly's connector asks of
// the part it names (docs/assemblies/README.md#a-mate-on-an-imported-face).
using FeatureLookup = std::function<const Feature*(uint64_t)>;
bool IsFileOwn(const FeatureLookup& find, const std::string& name);

// The print of the face (`face`) or edge at `index` of `body`, named `name`.
std::optional<ElementPrint> PrintAt(const Body& body, bool face, int index,
                                    const std::string& name);

// Whether `now` is the element `was` printed (its name aside).
bool SamePrint(const ElementPrint& was, const ElementPrint& now,
               double bodyDiagonal);

// Every face or edge of the bodies `featureId` created, named as the file's
// own geometry, that matches `was`. Each is (body, index, name).
struct Found {
  const Body* body = nullptr;
  int index = -1;
  std::string name;
};
std::vector<Found> FindPrinted(const RegenContext& ctx, uint64_t featureId,
                               bool face, const ElementPrint& was);
std::vector<Found> FindPrinted(const std::vector<Body>& bodies,
                               const FeatureLookup& find, uint64_t featureId,
                               bool face, const ElementPrint& was);
// The print's tolerance basis: the body's bounding diagonal, at least 1 mm.
double PrintDiagonal(const Body& body);

// "a circle 7.854 mm long at (5.05, -3.7, 18.7)".
std::string Describe(const ElementPrint& p, bool face);

} // namespace sketch
