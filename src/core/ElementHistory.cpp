// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ElementHistory.h"
#include "cad/OccSignals.h"

#include "core/ElementOrigin.h"

#include "cad/NonDestructiveBoolean.h"
#include "common/Logger.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace topo {

namespace {

// One boolean operand: its shape and the names it carries (may be empty).
struct NamedInput {
  const TopoDS_Shape& shape;
  const ElementMap& map;
};

// Every face and edge of `shape`, in the index order ElementMap uses.
void MapSubshapes(const TopoDS_Shape& shape, TopTools_IndexedMapOfShape& faces,
                  TopTools_IndexedMapOfShape& edges) {
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
}

// Writes `name` for `sub` (a face or edge of the result) into `out`, keyed by
// the result's index maps. First writer wins: inputs are processed target
// first, tool second, index order within each, so collisions (e.g. both
// operands generating the same section edge) resolve deterministically.
struct Placer {
  const TopTools_IndexedMapOfShape& resFaces;
  const TopTools_IndexedMapOfShape& resEdges;
  ElementMap& out;

  void operator()(const TopoDS_Shape& sub, const std::string& name) const {
    if (sub.ShapeType() == TopAbs_FACE) {
      const int idx = resFaces.FindIndex(sub) - 1;
      if (idx >= 0) {
        out.names.try_emplace(idx, name);
      }
    } else if (sub.ShapeType() == TopAbs_EDGE) {
      const int idx = resEdges.FindIndex(sub) - 1;
      if (idx >= 0) {
        out.edgeNames.try_emplace(idx, name);
      }
    }
  }
};

// The results one input became, when it became several of one kind: each
// placed under `base` for now, and told apart by QualifyPieces once every
// history name is down.
struct PieceGroup {
  std::string base;
  TopAbs_ShapeEnum type;
  std::vector<int> members; // result indices, ascending
};

// Places every shape of `list` under `base`. A 1->N fan-out is recorded as a
// group per kind (faces, edges) for QualifyPieces.
void PlaceAll(const TopTools_ListOfShape& list, const std::string& base,
              const Placer& place, const TopTools_IndexedMapOfShape& resFaces,
              const TopTools_IndexedMapOfShape& resEdges,
              std::vector<PieceGroup>& groups) {
  if (list.Extent() == 1) {
    place(list.First(), base);
    return;
  }
  PieceGroup faces{base, TopAbs_FACE, {}}, edges{base, TopAbs_EDGE, {}};
  for (const TopoDS_Shape& s : list) {
    const bool face = s.ShapeType() == TopAbs_FACE;
    const int idx = (face ? resFaces : resEdges).FindIndex(s) - 1;
    if (idx < 0)
      continue;
    place(s, base);
    (face ? faces : edges).members.push_back(idx);
  }
  for (PieceGroup* g : {&faces, &edges}) {
    std::sort(g->members.begin(), g->members.end());
    g->members.erase(std::unique(g->members.begin(), g->members.end()),
                     g->members.end());
    if (g->members.size() > 1)
      groups.push_back(std::move(*g));
  }
}

// Names each group's pieces by the faces only they touch
// (docs/modeling/toponaming.md#split-pieces): a face piece's bounds are the
// named faces sharing an edge with it, an edge piece's the named faces at
// its end vertices that do not contain it, each entered by its PieceStem.
// A piece's qualifier is its bounds less those every piece of the group
// has; pieces nothing tells apart are numbered in result order.
void QualifyPieces(const std::vector<PieceGroup>& groups,
                   const TopoDS_Shape& result,
                   const TopTools_IndexedMapOfShape& resFaces,
                   const TopTools_IndexedMapOfShape& resEdges,
                   ElementMap& out) {
  if (groups.empty())
    return;
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces, vertexFaces;
  TopExp::MapShapesAndAncestors(result, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  TopExp::MapShapesAndAncestors(result, TopAbs_VERTEX, TopAbs_FACE,
                                vertexFaces);
  const auto faceStem = [&](const TopoDS_Shape& f) -> const std::string* {
    const int i = resFaces.FindIndex(f) - 1;
    return i >= 0 ? out.NameOf(i) : nullptr;
  };
  for (const PieceGroup& g : groups) {
    const bool faces = g.type == TopAbs_FACE;
    auto& names = faces ? out.names : out.edgeNames;
    std::vector<int> live;
    for (const int m : g.members)
      if (const auto it = names.find(m);
          it != names.end() && it->second == g.base)
        live.push_back(m);
    if (live.size() < 2)
      continue;
    const std::string own = PieceStem(g.base);
    std::vector<std::set<std::string>> bounds(live.size());
    for (size_t k = 0; k < live.size(); k++) {
      const auto add = [&](const TopoDS_Shape& f) {
        if (const std::string* n = faceStem(f)) {
          std::string stem = PieceStem(*n);
          if (stem != own)
            bounds[k].insert(std::move(stem));
        }
      };
      if (faces) {
        const TopoDS_Shape& face = resFaces(live[k] + 1);
        TopTools_IndexedMapOfShape boundary;
        TopExp::MapShapes(face, TopAbs_EDGE, boundary);
        for (int e = 1; e <= boundary.Extent(); e++)
          if (const int at = edgeFaces.FindIndex(boundary(e)); at > 0)
            for (const TopoDS_Shape& f : edgeFaces(at))
              if (!f.IsSame(face))
                add(f);
      } else {
        const TopoDS_Shape& edge = resEdges(live[k] + 1);
        const int at = edgeFaces.FindIndex(edge);
        const auto holds = [&](const TopoDS_Shape& f) {
          if (at <= 0)
            return false;
          for (const TopoDS_Shape& h : edgeFaces(at))
            if (h.IsSame(f))
              return true;
          return false;
        };
        TopTools_IndexedMapOfShape ends;
        TopExp::MapShapes(edge, TopAbs_VERTEX, ends);
        for (int v = 1; v <= ends.Extent(); v++)
          if (const int vi = vertexFaces.FindIndex(ends(v)); vi > 0)
            for (const TopoDS_Shape& f : vertexFaces(vi))
              if (!holds(f))
                add(f);
      }
    }
    std::set<std::string> shared = bounds.front();
    for (size_t k = 1; k < bounds.size(); k++) {
      std::set<std::string> both;
      std::set_intersection(shared.begin(), shared.end(), bounds[k].begin(),
                            bounds[k].end(), std::inserter(both, both.begin()));
      shared = std::move(both);
    }
    // Each piece's qualifier; "" for one nothing of its own tells apart.
    std::vector<std::string> quals(live.size());
    std::map<std::string, int> uses;
    for (size_t k = 0; k < live.size(); k++) {
      for (const std::string& b : bounds[k])
        if (!shared.count(b))
          quals[k] += (quals[k].empty() ? "" : "|") + b;
      uses[quals[k]]++;
    }
    std::map<std::string, int> next;
    for (size_t k = 0; k < live.size(); k++) {
      std::string name =
          g.base + (quals[k].empty() ? "" : "[" + quals[k] + "]");
      if (quals[k].empty() || uses[quals[k]] > 1)
        name += "." + std::to_string(next[quals[k]]++);
      names[live[k]] = std::move(name);
    }
  }
}

// No two faces, and no two edges, share a name: the first (lowest index)
// keeps it, every later one takes the next free ".<i>".
void MakeNamesUnique(std::map<int, std::string>& names) {
  std::set<std::string> seen;
  std::map<std::string, int> dup;
  for (const auto& [i, n] : names)
    if (!seen.insert(n).second)
      dup[n] = 1;
  if (dup.empty())
    return;
  std::set<std::string> first;
  for (auto& [i, n] : names) {
    const auto d = dup.find(n);
    if (d == dup.end() || first.insert(n).second)
      continue;
    std::string renamed;
    do
      renamed = n + "." + std::to_string(d->second++);
    while (seen.count(renamed));
    seen.insert(renamed);
    n = std::move(renamed);
  }
}

// The core rewrite: input MappedNames -> result ElementMap, driven by a
// normalized BRepTools_History plus the boolean's section edges (empty list
// for non-boolean ops), finished by the U/L fallback passes.
ElementMap PropagateNames(const std::vector<NamedInput>& inputs,
                          const BRepTools_History& hist,
                          const TopTools_ListOfShape& seamEdges,
                          const TopoDS_Shape& result, uint64_t featureTag) {
  ElementMap out;
  TopTools_IndexedMapOfShape resFaces, resEdges;
  MapSubshapes(result, resFaces, resEdges);
  const Placer place{resFaces, resEdges, out};

  const std::string tag = std::to_string(featureTag);
  const std::string mTag = ";M" + tag;
  const std::string gTag = ";G" + tag;
  std::vector<PieceGroup> groups;

  // History pass: survivor keeps its name; Modified appends M; Generated
  // appends G (booleans: a pierced face *generates* its section edges, a
  // fillet spine edge generates its blend face); removed-with-nothing-else
  // names simply die, so references to them fail — the intended signal.
  for (const NamedInput& input : inputs) {
    TopTools_IndexedMapOfShape faces, edges;
    MapSubshapes(input.shape, faces, edges);
    auto propagate = [&](const TopoDS_Shape& sub, const std::string& name) {
      const TopTools_ListOfShape& mods = hist.Modified(sub);
      if (mods.IsEmpty() && !hist.IsRemoved(sub)) {
        place(sub, name); // survivor: identity preserved for free
      } else if (!mods.IsEmpty()) {
        PlaceAll(mods, name + mTag, place, resFaces, resEdges, groups);
      }
      const TopTools_ListOfShape& gens = hist.Generated(sub);
      if (!gens.IsEmpty()) {
        PlaceAll(gens, name + gTag, place, resFaces, resEdges, groups);
      }
    };
    for (const auto& [faceIdx, name] : input.map.names) {
      if (faceIdx >= 0 && faceIdx + 1 <= faces.Extent()) {
        propagate(faces(faceIdx + 1), name);
      }
    }
    for (const auto& [edgeIdx, name] : input.map.edgeNames) {
      if (edgeIdx >= 0 && edgeIdx + 1 <= edges.Extent()) {
        propagate(edges(edgeIdx + 1), name);
      }
    }
  }

  // Boolean section edges history did not reach: "<tag>/seam<i>".
  int seam = 0;
  for (const TopoDS_Shape& e : seamEdges) {
    // A section edge handed back by the maker may itself have been modified.
    const TopTools_ListOfShape& mods = hist.Modified(e);
    if (mods.IsEmpty()) {
      place(e, tag + "/seam" + std::to_string(seam++));
    } else {
      for (const TopoDS_Shape& m : mods) {
        place(m, tag + "/seam" + std::to_string(seam++));
      }
    }
  }

  QualifyPieces(groups, result, resFaces, resEdges, out);
  return out;
}

// U pass then L pass over `shape`, writing into `map` (see header).
void FallbackPasses(const TopoDS_Shape& shape, ElementMap& map) {
  TopTools_IndexedMapOfShape resFaces, resEdges;
  MapSubshapes(shape, resFaces, resEdges);

  // U: a named face names its unnamed boundary edges. localIdx counts every
  // unique boundary edge (named or not, deduped — a closed surface's seam
  // edge appears twice in the wire), so the index is stable.
  for (const auto& [faceIdx, faceName] : map.names) {
    if (faceIdx + 1 > resFaces.Extent()) {
      continue;
    }
    TopTools_IndexedMapOfShape boundary;
    TopExp::MapShapes(resFaces(faceIdx + 1), TopAbs_EDGE, boundary);
    for (int k = 1; k <= boundary.Extent(); k++) {
      const int idx = resEdges.FindIndex(boundary(k)) - 1;
      if (idx >= 0) {
        map.edgeNames.try_emplace(idx, faceName + ";U" + std::to_string(k - 1));
      }
    }
  }

  // L: an unnamed face is named from its sorted boundary-edge names (catches
  // fillet corner patches bounded by the blend faces' U edges, seam faces,
  // revolve caps the history missed).
  for (int f = 1; f <= resFaces.Extent(); f++) {
    if (map.names.count(f - 1)) {
      continue;
    }
    TopTools_IndexedMapOfShape boundary;
    TopExp::MapShapes(resFaces(f), TopAbs_EDGE, boundary);
    std::vector<std::string> edgeNames;
    for (int k = 1; k <= boundary.Extent(); k++) {
      const int idx = resEdges.FindIndex(boundary(k)) - 1;
      if (const std::string* n = idx >= 0 ? map.EdgeNameOf(idx) : nullptr) {
        edgeNames.push_back(*n);
      }
    }
    if (edgeNames.empty()) {
      continue;
    }
    std::sort(edgeNames.begin(), edgeNames.end());
    std::string joined;
    for (const std::string& n : edgeNames) {
      joined += joined.empty() ? n : "," + n;
    }
    map.names[f - 1] = joined + ";L";
  }
}

// The passes after history: U/L fallback, then uniqueness.
void FinishNames(const TopoDS_Shape& shape, ElementMap& map) {
  FallbackPasses(shape, map);
  MakeNamesUnique(map.names);
  MakeNamesUnique(map.edgeNames);
}

// All faces and edges of `shape`, as the explicit argument list
// BRepTools_History's template constructor requires (it queries each argument
// directly — handing it the solid yields a silently empty history).
void AppendSubshapeArgs(const TopoDS_Shape& shape, TopTools_ListOfShape& args) {
  TopTools_IndexedMapOfShape faces, edges;
  MapSubshapes(shape, faces, edges);
  for (int i = 1; i <= faces.Extent(); i++) {
    args.Append(faces(i));
  }
  for (int i = 1; i <= edges.Extent(); i++) {
    args.Append(edges(i));
  }
}

} // namespace

TopTools_ListOfShape HistoryArguments(const TopoDS_Shape& shape) {
  TopTools_ListOfShape args;
  AppendSubshapeArgs(shape, args);
  return args;
}

CADShape BooleanNamed(BooleanOp op, const CADShape& target,
                      const ElementMap& targetMap, const CADShape& tool,
                      const ElementMap& toolMap, uint64_t featureTag,
                      ElementMap& out, std::string* error, double fuzzy,
                      const RefineResult& refine) {
  out = {};
  if (!target.IsValid() || !tool.IsValid()) {
    if (error)
      *error = "boolean input shape is invalid";
    return CADShape();
  }
  try {
    // On copies of both operands, topology only (the geometry is shared):
    // the target is the previous feature's cached body, and a boolean writes
    // the p-curves and tolerances it computes onto its arguments' edges, so
    // every later feature's result on that body depended on what had run on
    // it before -- a split of random design 33 lost 5.6 mm3 on a fresh body
    // and none after a cut had been tried on it. Not SetNonDestructive: that
    // reports the faces it copied as modified, and the names a saved document
    // cites ('4/g1') moved. A copy keeps the explorer order, and with it every
    // index-keyed name of the operands' maps. The two are copied as one
    // compound, so what they share they still share: two pieces of one body
    // (a Jog's halves) fused share their untouched edges, and copied apart
    // each of those came back modified.
    TopoDS_Shape targetIn, toolIn;
    {
      TopoDS_Compound both;
      BRep_Builder bb;
      bb.MakeCompound(both);
      bb.Add(both, target.GetShape());
      bb.Add(both, tool.GetShape());
      BRepBuilderAPI_Copy copy(both, false, false);
      targetIn = copy.ModifiedShape(target.GetShape());
      toolIn = copy.ModifiedShape(tool.GetShape());
    }
    std::unique_ptr<BRepAlgoAPI_BooleanOperation> mk;
    if (fuzzy > 0.0) {
      // The retry: built by hand so the fuzzy value is set before it runs.
      switch (op) {
      case BooleanOp::Union:
        mk = std::make_unique<BRepAlgoAPI_Fuse>();
        break;
      case BooleanOp::Subtract:
        mk = std::make_unique<BRepAlgoAPI_Cut>();
        break;
      case BooleanOp::Intersect:
        mk = std::make_unique<BRepAlgoAPI_Common>();
        break;
      }
      cad::BuildNonDestructive(*mk, targetIn, toolIn, fuzzy);
    } else {
      switch (op) {
      case BooleanOp::Union:
        mk = std::make_unique<BRepAlgoAPI_Fuse>(targetIn, toolIn);
        break;
      case BooleanOp::Subtract:
        mk = std::make_unique<BRepAlgoAPI_Cut>(targetIn, toolIn);
        break;
      case BooleanOp::Intersect:
        mk = std::make_unique<BRepAlgoAPI_Common>(targetIn, toolIn);
        break;
      }
    }
    if (!mk->IsDone()) {
      if (error)
        *error = "boolean operation did not complete";
      return CADShape();
    }
    TopoDS_Shape result = mk->Shape();

    // Normalize the maker's reports onto BRepTools_History (T1 prereq 5).
    TopTools_ListOfShape args;
    AppendSubshapeArgs(targetIn, args);
    AppendSubshapeArgs(toolIn, args);
    BRepTools_History hist(args, *mk);
    // The clean-up, composed onto the boolean's history: inputs -> result.
    if (refine) {
      BRepTools_History after;
      const TopoDS_Shape refined = refine(result, after);
      if (!refined.IsNull()) {
        hist.Merge(after);
        result = refined;
      }
    }

    out = PropagateNames({{targetIn, targetMap}, {toolIn, toolMap}}, hist,
                         mk->SectionEdges(), result, featureTag);
    FinishNames(result, out);
    return CADShape(result);
  } catch (const Standard_Failure& e) {
    LOG_ERROR("BooleanNamed: OCCT error: {}", e.GetMessageString());
    if (error)
      *error = std::string("OCCT error: ") + e.GetMessageString();
    out = {};
    return CADShape();
  }
}

ElementMap NamesThroughHistory(const CADShape& target,
                               const ElementMap& targetMap,
                               const CADShape& tool, const ElementMap& toolMap,
                               const BRepTools_History& history,
                               const CADShape& result, uint64_t featureTag) {
  ElementMap out = PropagateNames(
      {{target.GetShape(), targetMap}, {tool.GetShape(), toolMap}}, history,
      TopTools_ListOfShape(), result.GetShape(), featureTag);
  FinishNames(result.GetShape(), out);
  return out;
}

ElementMap NamesThroughHistory(const CADShape& input, const ElementMap& map,
                               const BRepTools_History& history,
                               const CADShape& result, uint64_t featureTag,
                               bool keepWholeFaces) {
  ElementMap out =
      PropagateNames({{input.GetShape(), map}}, history, TopTools_ListOfShape(),
                     result.GetShape(), featureTag);
  if (!keepWholeFaces) {
    FinishNames(result.GetShape(), out);
    return out;
  }
  // A clean-up rebuilds every face whose boundary edges it merged, and
  // reports each as Modified -- so the end face of two boxes unioned and
  // refined came back "1/face0;M4", though it is the same face on the same
  // surface. A result face that ONE input face became, whole, is that face:
  // it keeps the name verbatim. Faces it merged keep the ";M" naming.
  TopTools_IndexedMapOfShape inFaces, inEdges, resFaces, resEdges;
  MapSubshapes(input.GetShape(), inFaces, inEdges);
  MapSubshapes(result.GetShape(), resFaces, resEdges);
  std::map<int, std::vector<const std::string*>> onto;
  for (const auto& [idx, name] : map.names) {
    if (idx < 0 || idx + 1 > inFaces.Extent())
      continue;
    const TopoDS_Shape& f = inFaces(idx + 1);
    const TopTools_ListOfShape& mods = history.Modified(f);
    if (mods.IsEmpty()) {
      if (!history.IsRemoved(f))
        if (const int r = resFaces.FindIndex(f) - 1; r >= 0)
          onto[r].push_back(&name);
      continue;
    }
    int faces = 0, r = -1;
    for (const TopoDS_Shape& s : mods)
      if (s.ShapeType() == TopAbs_FACE) {
        ++faces;
        r = resFaces.FindIndex(s) - 1;
      }
    if (faces == 1 && r >= 0)
      onto[r].push_back(&name);
    else if (faces > 1)
      onto[-1].push_back(&name); // split: not one face's name any more
  }
  onto.erase(-1);
  for (const auto& [r, names] : onto)
    if (names.size() == 1)
      out.names[r] = *names.front();
  FinishNames(result.GetShape(), out);
  return out;
}

std::vector<std::string> FaceNamesIn(const TopoDS_Shape& part,
                                     const TopoDS_Shape& whole,
                                     const ElementMap& map) {
  TopTools_IndexedMapOfShape faces, mine;
  TopExp::MapShapes(whole, TopAbs_FACE, faces);
  TopExp::MapShapes(part, TopAbs_FACE, mine);
  std::vector<int> found;
  for (int i = 1; i <= mine.Extent(); i++)
    if (const int at = faces.FindIndex(mine(i)) - 1; at >= 0)
      found.push_back(at);
  std::sort(found.begin(), found.end());
  found.erase(std::unique(found.begin(), found.end()), found.end());
  std::vector<std::string> out;
  for (const int at : found)
    if (const std::string* n = map.NameOf(at))
      out.push_back(*n);
  return out;
}

namespace {

// Shared tail of both blend makers: build, verify, propagate, fall back.
template <typename Maker>
CADShape RunEdgeBlend(Maker& mk, const TopoDS_Shape& shape,
                      const ElementMap& bodyMap, uint64_t featureTag,
                      ElementMap& out, std::string* error) {
  // In an OCC signal scope (cad/OccSignals.h): the builder segfaults on a set
  // it cannot build, and only inside such a scope is that an exception.
  if (const auto why = occsig::RunCatchingOccSignals([&] { mk.Build(); })) {
    if (error)
      *error = "the blend builder failed inside OpenCASCADE (" + *why +
               "); a smaller radius, or fewer edges at once, usually builds";
    return CADShape();
  }
  if (!mk.IsDone()) {
    if (error) {
      *error = "the blend could not be built (radius too large for the "
               "geometry?)";
    }
    return CADShape();
  }
  const TopoDS_Shape result = mk.Shape();
  if (result.IsNull()) {
    return CADShape();
  }

  TopTools_ListOfShape args;
  AppendSubshapeArgs(shape, args);
  const BRepTools_History hist(args, mk);
  static const TopTools_ListOfShape kNoSeams;
  out = PropagateNames({{shape, bodyMap}}, hist, kNoSeams, result, featureTag);
  FinishNames(result, out);
  return CADShape(result);
}

// WHERE the blend could not be built, not just how many places. The count
// alone is honest and unactionable: asked which three contours of eight, a
// caller can only reason about the model and guess — and the answer was
// worth having both times it came up (four d5.5 fixing holes asked for an R8
// rim; three studs standing across the rims being rounded). OCC knows which
// contours failed and which edges are on them, so say so.
std::string FaultyContourPlaces(BRepFilletAPI_MakeFillet& mk) {
  std::string out;
  for (int i = 1; i <= mk.NbFaultyContours(); i++) {
    const int contour = mk.FaultyContour(i);
    if (contour < 1 || contour > mk.NbContours() || mk.NbEdges(contour) < 1) {
      continue;
    }
    // One point per contour: a contour is a chain, and the first edge's
    // middle is enough to find it in the model.
    BRepAdaptor_Curve curve(mk.Edge(contour, 1));
    const gp_Pnt at =
        curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s(%.3f, %.3f, %.3f)",
                  out.empty() ? "" : ", ", at.X(), at.Y(), at.Z());
    out += buf;
  }
  return out.empty() ? out : " — near " + out;
}

// Adds the edges to a blend maker (OCC's tangent-chain walk). Adding an
// edge walks the chain of edges tangent to it, which is where the builder
// went round a vertex beside a seam for ever (patched in our OCC,
// nix/patches/occt-fillet-reorder-faces-loop.patch: it throws) -- so in the
// signal scope too, not only the build. False with `error` set when the walk
// failed, or when an edge started no contour: its faces tangent, or a face
// on one side only, and on no other edge's tangent chain -- OCC left it out
// and built the rest, saying nothing.
template <typename Maker>
bool AddBlendEdges(Maker& mk, const TopTools_IndexedMapOfShape& edges,
                   const std::vector<int>& edgeIndices, double radiusOrDistance,
                   const ElementMap& bodyMap, std::string* error) {
  const auto why = occsig::RunCatchingOccSignals([&] {
    for (int idx : edgeIndices)
      mk.Add(radiusOrDistance, TopoDS::Edge(edges(idx + 1)));
  });
  if (!why) {
    for (int idx : edgeIndices) {
      if (mk.Contour(TopoDS::Edge(edges(idx + 1))) != 0)
        continue;
      if (error) {
        const std::string* n = bodyMap.EdgeNameOf(idx);
        *error = "edge '" + (n != nullptr ? *n : std::to_string(idx)) +
                 "' starts no blend (its two faces meet tangentially, "
                 "or it has a face on one side only): leave it out";
      }
      return false;
    }
    return true;
  }
  if (error) {
    if (why->find("do not lead to the next edge's faces") != std::string::npos)
      *error = "the blend builder cannot follow the tangent chain of "
               "these edges round one of its vertices (a seam meets it "
               "there); pick another edge, or one past a sharp corner";
    else
      *error = "the blend builder failed inside OpenCASCADE while "
               "collecting the edges (" +
               *why + ")";
  }
  return false;
}

} // namespace

std::string EdgeBlendRefusal(const CADShape& body, const ElementMap& bodyMap,
                             const std::vector<int>& edgeIndices,
                             EdgeBlendOp op, double radiusOrDistance) {
  if (!body.IsValid() || edgeIndices.empty())
    return {};
  try {
    const TopoDS_Shape shape =
        BRepBuilderAPI_Copy(body.GetShape(), false, false).Shape();
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int idx : edgeIndices)
      if (idx < 0 || idx + 1 > edges.Extent())
        return "edge index out of range";
    std::string error;
    if (op == EdgeBlendOp::Chamfer) {
      BRepFilletAPI_MakeChamfer mk(shape);
      AddBlendEdges(mk, edges, edgeIndices, radiusOrDistance, bodyMap, &error);
    } else {
      BRepFilletAPI_MakeFillet mk(shape);
      AddBlendEdges(mk, edges, edgeIndices, radiusOrDistance, bodyMap, &error);
    }
    return error;
  } catch (const Standard_Failure& e) {
    return std::string("the blend builder refused the edges: ") +
           e.GetMessageString();
  }
}

CADShape EdgeBlendNamed(const CADShape& body, const ElementMap& bodyMap,
                        const std::vector<int>& edgeIndices, EdgeBlendOp op,
                        double radiusOrDistance, uint64_t featureTag,
                        ElementMap& out, std::string* error) {
  out = {};
  if (!body.IsValid() || edgeIndices.empty()) {
    return CADShape();
  }
  // The blend builder segfaults on a set it cannot build rather than
  // reporting a faulty contour; RunEdgeBlend builds it in a signal scope.
  try {
    // On a copy, topology only: the body is the previous feature's cached
    // result, and the blend builder writes the p-curves and tolerances it
    // computes onto its input, so a later split of the same body built other
    // pieces after a fillet had been tried on it (random design 33), and a
    // build that faulted part-way left it half-written for undo to serve. A
    // copy keeps the explorer order, and with it every index-keyed name.
    const TopoDS_Shape shape =
        BRepBuilderAPI_Copy(body.GetShape(), false, false).Shape();
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int idx : edgeIndices) {
      if (idx < 0 || idx + 1 > edges.Extent()) {
        if (error) {
          *error = "edge index out of range";
        }
        return CADShape();
      }
    }
    const auto addAll = [&](auto& mk) {
      return AddBlendEdges(mk, edges, edgeIndices, radiusOrDistance, bodyMap,
                           error);
    };

    if (op == EdgeBlendOp::Chamfer) {
      BRepFilletAPI_MakeChamfer mk(shape);
      if (!addAll(mk))
        return CADShape();
      return RunEdgeBlend(mk, shape, bodyMap, featureTag, out, error);
    }
    BRepFilletAPI_MakeFillet mk(shape);
    if (!addAll(mk))
      return CADShape();
    CADShape result = RunEdgeBlend(mk, shape, bodyMap, featureTag, out, error);
    if (!result.IsValid() && error && mk.NbFaultyContours() > 0) {
      *error = "the fillet failed on " + std::to_string(mk.NbFaultyContours()) +
               " contour(s) (radius too large for the geometry?)" +
               FaultyContourPlaces(mk);
    }
    return result;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("EdgeBlendNamed: OCCT error: {}", e.GetMessageString());
    if (error) {
      *error = std::string("OCCT error: ") + e.GetMessageString();
    }
    out = {};
    return CADShape();
  }
}

void NamePrimitiveElements(const CADShape& shape, const std::string& prefix,
                           ElementMap& out) {
  out = {};
  if (!shape.IsValid()) {
    return;
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape.GetShape(), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); i++) {
    out.names[i - 1] = prefix + "/face" + std::to_string(i - 1);
  }
  FallbackPasses(shape.GetShape(), out);
}

void NamesOfSubShape(const CADShape& whole, const ElementMap& wholeMap,
                     const CADShape& part, ElementMap& out) {
  out = ElementMap{};
  if (!whole.IsValid() || !part.IsValid()) {
    return;
  }
  TopTools_IndexedMapOfShape wholeFaces, wholeEdges, partFaces, partEdges;
  MapSubshapes(whole.GetShape(), wholeFaces, wholeEdges);
  MapSubshapes(part.GetShape(), partFaces, partEdges);

  for (int i = 1; i <= partFaces.Extent(); i++) {
    const int w = wholeFaces.FindIndex(partFaces(i));
    if (const std::string* n = w > 0 ? wholeMap.NameOf(w - 1) : nullptr) {
      out.names[i - 1] = *n;
    }
  }
  for (int i = 1; i <= partEdges.Extent(); i++) {
    const int w = wholeEdges.FindIndex(partEdges(i));
    if (const std::string* n = w > 0 ? wholeMap.EdgeNameOf(w - 1) : nullptr) {
      out.edgeNames[i - 1] = *n;
    }
  }
}

void ApplyFallbackPasses(const CADShape& shape, ElementMap& map) {
  if (!shape.IsValid()) {
    return;
  }
  FallbackPasses(shape.GetShape(), map);
}

int EdgeIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& edge) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  return edges.FindIndex(edge) - 1;
}

glm::vec3 PolylineArcMidpoint(const std::vector<glm::vec3>& poly) {
  float total = 0.0f;
  for (size_t i = 0; i + 1 < poly.size(); i++)
    total += glm::length(poly[i + 1] - poly[i]);
  float half = total * 0.5f;
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    const float seg = glm::length(poly[i + 1] - poly[i]);
    if (half <= seg)
      return poly[i] +
             (seg > 0.0f ? half / seg : 0.0f) * (poly[i + 1] - poly[i]);
    half -= seg;
  }
  return poly.empty() ? glm::vec3(0.0f) : poly.back();
}

std::optional<ElementRef> ResolveEdgeOnBody(const Body& input,
                                            const std::string& pickedName,
                                            const glm::vec3& probe) {
  if (input.elementMap.ResolveEdge(pickedName).has_value()) {
    return ElementRef{input.createdBy, pickedName};
  }

  // Geometric fallback: an edge untouched by the downstream op is identical
  // on both body states, so `probe` lies exactly on its input polyline.
  const auto distToPoly = [](const glm::vec3& p,
                             const std::vector<glm::vec3>& poly) {
    float d = FLT_MAX;
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      const glm::vec3 a = poly[i];
      const glm::vec3 ab = poly[i + 1] - a;
      const float len2 = glm::dot(ab, ab);
      const float t = len2 > 0.0f
                          ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                          : 0.0f;
      d = std::min(d, glm::length(p - (a + ab * t)));
    }
    return d;
  };
  const auto polys = input.shape.GenerateEdgePolylines();
  int best = -1;
  float bestD = 1e-3f; // mm; identical geometry, so effectively exact
  for (size_t e = 0; e < polys.size(); e++) {
    if (polys[e].size() < 2) {
      continue;
    }
    const float d = distToPoly(probe, polys[e]);
    if (d < bestD) {
      bestD = d;
      best = static_cast<int>(e);
    }
  }
  if (best < 0) {
    return std::nullopt;
  }
  const std::string* name = input.elementMap.EdgeNameOf(best);
  if (name == nullptr) {
    return std::nullopt;
  }
  return ElementRef{input.createdBy, *name};
}

std::string
UnresolvedReason(const ElementMap& map, const std::string& name,
                 ElementKind kind, const std::string& what,
                 const std::function<std::string(uint64_t)>& featureLabel) {
  const std::string gone = what + " '" + name + "' no longer exists";
  const auto& names = kind == ElementKind::Face ? map.names : map.edgeNames;
  const PieceName want = ParsePiece(name);
  // What it became: the other pieces of the face it was a piece of, or the
  // pieces of a face it was whole.
  std::vector<std::string> pieces;
  uint64_t by = 0;
  for (const auto& [index, n] : names) {
    (void)index;
    if (n == name)
      continue;
    if (want.piece) {
      const PieceName have = ParsePiece(n);
      if (have.piece && have.of == want.of) {
        pieces.push_back(n);
        if (by == 0)
          by = have.splitBy;
        continue;
      }
    }
    uint64_t at = 0;
    if (DescentOf(name, n, &at) == Descent::Split) {
      pieces.push_back(n);
      if (by == 0)
        by = at;
    }
  }
  if (pieces.empty())
    return gone;
  // The pieces that hold any of what bounded it, when some do: the ones it
  // could have meant.
  if (want.qualified) {
    std::vector<std::string> meant;
    for (const std::string& n : pieces) {
      const PieceName have = ParsePiece(n);
      for (const std::string& b : want.boundedBy)
        if (std::find(have.boundedBy.begin(), have.boundedBy.end(), b) !=
            have.boundedBy.end()) {
          meant.push_back(n);
          break;
        }
    }
    if (!meant.empty())
      pieces = std::move(meant);
  }
  const std::string label =
      featureLabel ? featureLabel(by) : "feature " + std::to_string(by);
  constexpr size_t kListed = 6;
  std::string list;
  const size_t shown = std::min(pieces.size(), kListed);
  for (size_t i = 0; i < shown; i++) {
    if (i > 0)
      list += i + 1 == shown && shown == pieces.size() ? " and " : ", ";
    list += "'" + pieces[i] + "'";
  }
  if (pieces.size() > shown)
    list += " and " + std::to_string(pieces.size() - shown) + " more";
  return what + " '" + name + "' was split by " + label + " into " + list +
         "; pick the one you meant";
}

static std::string
UnresolvedReason(const std::vector<Body>& bodies, const ElementRef& ref,
                 ElementKind kind, const std::string& what,
                 const std::function<std::string(uint64_t)>& label) {
  std::string reason;
  for (const Body& b : bodies) {
    if (ref.featureId != 0 && b.createdBy != ref.featureId)
      continue; // 0: whichever body has it
    std::string r = UnresolvedReason(b.elementMap, ref.name, kind, what, label);
    if (r.find("' was split by ") != std::string::npos)
      return r;
    reason = std::move(r);
  }
  return reason.empty() ? UnresolvedReason(ElementMap{}, ref.name, kind, what)
                        : reason;
}

std::string UnresolvedReason(const std::vector<Body>& bodies,
                             const ElementRef& ref, ElementKind kind,
                             const std::string& what) {
  return UnresolvedReason(bodies, ref, kind, what, {});
}

std::string UnresolvedReason(const RegenContext& ctx, const ElementRef& ref,
                             ElementKind kind, const std::string& what) {
  return UnresolvedReason(
      ctx.bodies, ref, kind, what, [&](uint64_t id) -> std::string {
        for (const auto& f : ctx.features)
          if (f && f->GetId() == id)
            return f->GetName() + " (" + std::to_string(id) + ")";
        return "feature " + std::to_string(id);
      });
}

namespace {

bool Resolves(const Body& b, const std::string& name, ElementKind kind) {
  return kind == ElementKind::Face ? b.elementMap.Resolve(name).has_value()
                                   : b.elementMap.ResolveEdge(name).has_value();
}

std::string BodyLabel(const Body& b) { return "'" + b.name + "'"; }

std::function<std::string(uint64_t)> ContextLabel(const RegenContext& ctx) {
  return [&ctx](uint64_t id) -> std::string {
    for (const auto& f : ctx.features)
      if (f && f->GetId() == id)
        return f->GetName() + " (" + std::to_string(id) + ")";
    return "feature " + std::to_string(id);
  };
}

} // namespace

const Body* FindOwningBody(const std::vector<Body>& bodies,
                           const ElementRef& ref, ElementKind kind,
                           const std::string& what, std::string* why,
                           const std::function<std::string(uint64_t)>& label) {
  const auto featureLabel = [&](uint64_t id) {
    return label ? label(id) : "feature " + std::to_string(id);
  };
  std::vector<const Body*> withId, resolving;
  for (const Body& b : bodies) {
    if (b.createdBy != ref.featureId)
      continue;
    withId.push_back(&b);
    if (Resolves(b, ref.name, kind))
      resolving.push_back(&b);
  }
  if (resolving.size() == 1)
    return resolving.front();
  if (resolving.size() > 1) {
    if (why)
      *why = what + " '" + ref.name + "' is on " +
             std::to_string(resolving.size()) + " bodies made by " +
             featureLabel(ref.featureId) + " (" + BodyLabel(*resolving[0]) +
             " and " + BodyLabel(*resolving[1]) +
             "), so the reference cannot say which: pick it on a body "
             "whose names are its own";
    return nullptr;
  }
  if (withId.empty()) {
    // Named by the feature that MADE the element rather than the one that
    // created its body (an element's made_by): say which id it is on.
    for (const Body& b : bodies)
      if (b.createdBy != 0 && Resolves(b, ref.name, kind)) {
        if (why)
          *why =
              what + " '" + ref.name + "' is on the body " + BodyLabel(b) +
              ", created by " + featureLabel(b.createdBy) +
              ", not on a body of " + featureLabel(ref.featureId) +
              ": reference it as {feature_id: " + std::to_string(b.createdBy) +
              "} (its body's created_by)";
        return nullptr;
      }
    if (why)
      *why = "the body of " + what + " '" + ref.name + "' (created by " +
             featureLabel(ref.featureId) +
             ") no longer exists: that feature made no body, or a later one "
             "consumed it";
    return nullptr;
  }
  if (why)
    *why = UnresolvedReason(bodies, ref, kind, what, label);
  return nullptr;
}

Body* FindOwningBody(RegenContext& ctx, const ElementRef& ref, ElementKind kind,
                     const std::string& what, std::string* why) {
  return const_cast<Body*>(
      FindOwningBody(ctx.bodies, ref, kind, what, why, ContextLabel(ctx)));
}

const Body*
FindOwningBodyOfAll(const std::vector<Body>& bodies,
                    const std::vector<ElementRef>& refs, ElementKind kind,
                    const std::string& what, std::string* why,
                    const std::function<std::string(uint64_t)>& label) {
  const Body* first = nullptr;
  const ElementRef* firstRef = nullptr;
  for (const ElementRef& ref : refs) {
    const Body* b = FindOwningBody(bodies, ref, kind, what, why, label);
    if (b == nullptr)
      return nullptr;
    if (first == nullptr) {
      first = b;
      firstRef = &ref;
    } else if (b != first) {
      if (why)
        *why = "every " + what + " must be on one body: '" + firstRef->name +
               "' is on " + BodyLabel(*first) + " and '" + ref.name + "' on " +
               BodyLabel(*b);
      return nullptr;
    }
  }
  return first;
}

Body* FindOwningBodyOfAll(RegenContext& ctx,
                          const std::vector<ElementRef>& refs, ElementKind kind,
                          const std::string& what, std::string* why) {
  return const_cast<Body*>(FindOwningBodyOfAll(ctx.bodies, refs, kind, what,
                                               why, ContextLabel(ctx)));
}

} // namespace topo
