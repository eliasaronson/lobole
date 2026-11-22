// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The evidence a bound kind refuses on (fit::Refuses,
// docs/modeling/README.md#bounds-that-refuse): every edge of every body in a
// directory of BREP files, alone and with the edge its face's width ends at,
// filleted and chamfered past its least bound -- a thousandth, a
// hundredth, a tenth, a half, twice and four times -- and past its least
// refusing bound where that is not the least ("not least <kind>"), and every
// build that gives a valid solid there counted. A bound kind refuses only while
// this finds none of its own.
//
// Hidden: it builds a few failing blends an edge, minutes on the matrix's
// bodies. The bodies come from `tools/action_matrix.py export-breps DIR`
// (every design of the matrix's corpus) and `tools/kernel_cases.py
// export-breps DIR` (the fillet and chamfer cases' bodies before their
// blend):
//
//   LOBOLE_FIT_SWEEP_DIR=DIR [LOBOLE_FIT_SWEEP_SHARD=k/n] \
//     build/tests/designer_tests '[.fit_sweep]'

#include <catch2/catch_test_macros.hpp>

#include "core/BlendFit.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/MeasureBlend.h"
#include "core/features/FilletFeature.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

const char* KindName(fit::BoundKind k) {
  switch (k) {
  case fit::BoundKind::Width:
    return "width";
  case fit::BoundKind::Between:
    return "between";
  case fit::BoundKind::Curvature:
    return "curvature";
  case fit::BoundKind::Rim:
    return "rim";
  case fit::BoundKind::Meet:
    return "meet";
  }
  return "?";
}

// The factors past the least bound each edge set is built at.
constexpr double kFactors[] = {
    1.0 + fit::kRefuseMargin, 1.0 + fit::kMeetRefuseMargin, 1.1, 1.5, 2.0, 4.0};
constexpr int kLevels = static_cast<int>(std::size(kFactors));

struct Tally {
  int tried = 0;           // edge sets whose least bound is of this kind
  int built[kLevels] = {}; // of them, built at that factor past it
};

} // namespace

TEST_CASE("Nothing builds past a refusing bound", "[.fit_sweep]") {
  const char* dir = std::getenv("LOBOLE_FIT_SWEEP_DIR");
  if (dir == nullptr)
    SKIP("LOBOLE_FIT_SWEEP_DIR names no directory of BREP files");
  int shardK = 0, shardN = 1;
  if (const char* s = std::getenv("LOBOLE_FIT_SWEEP_SHARD"))
    REQUIRE(std::sscanf(s, "%d/%d", &shardK, &shardN) == 2);
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
    if (e.path().extension() == ".brep")
      files.push_back(e.path());
  std::sort(files.begin(), files.end());
  REQUIRE_FALSE(files.empty());

  std::map<std::string, Tally> tally; // "<op> <kind>"
  std::vector<std::string> past;
  for (size_t fi = 0; fi < files.size(); fi++) {
    if (static_cast<int>(fi % shardN) != shardK)
      continue;
    TopoDS_Shape whole;
    BRep_Builder builder;
    if (!BRepTools::Read(whole, files[fi].c_str(), builder))
      continue;
    int solidNo = 0;
    for (TopExp_Explorer sx(whole, TopAbs_SOLID); sx.More();
         sx.Next(), solidNo++) {
      Body body;
      body.shape = CADShape(sx.Current());
      body.createdBy = 1;
      topo::NamePrimitiveElements(body.shape, "1", body.elementMap);
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(sx.Current(), TopAbs_EDGE, edges);
      Measure::BlendProbe probe(body);
      // The edge sets: each edge alone, and with the edge each of its
      // faces' widths ends at.
      std::vector<std::vector<int>> sets;
      for (int e = 0; e < edges.Extent(); e++) {
        sets.push_back({e});
        if (const auto faces = probe.Faces(e))
          for (const int f : *faces)
            if (const auto w = probe.Width(e, f);
                w && w->endsAt >= 0 && w->endsAt > e)
              sets.push_back({e, w->endsAt});
      }
      for (const auto& set : sets)
        for (const EdgeBlendOp op :
             {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
          // The least bound, and the least that refuses where it is not
          // that one: a refusing bound is asked before the build whether
          // or not anything measures less.
          const auto least = fit::BlendBound(body, set, op);
          if (!least)
            continue;
          std::vector<std::pair<std::string, fit::Bound>> tested;
          const std::string opName =
              op == EdgeBlendOp::Chamfer ? "chamfer " : "fillet ";
          tested.emplace_back(opName + KindName(least->kind), *least);
          if (const auto refusing = fit::Least(fit::RefusingBounds(
                  body, set, op, std::numeric_limits<double>::infinity()));
              refusing && refusing->size > least->size * (1.0 + 1e-9))
            tested.emplace_back(
                opName + "not least " + KindName(refusing->kind), *refusing);
          for (const auto& [key, bound] : tested) {
            Tally& t = tally[key];
            t.tried++;
            for (int level = 0; level < kLevels; level++) {
              const double size = bound.size * kFactors[level];
              const CADShape built =
                  topo::EdgeBlendUnnamed(body.shape, set, op, size);
              if (!built.IsValid() || !Measure::InvalidReasons(built).empty())
                continue;
              t.built[level]++;
              std::string edgesText;
              for (const int e : set)
                edgesText += (edgesText.empty() ? "" : ",") + std::to_string(e);
              past.push_back(files[fi].filename().string() + " solid " +
                             std::to_string(solidNo) + " edges " + edgesText +
                             ": " + key + " " + fit::Mm(bound.size) +
                             " mm, built at " + fit::Mm(size) + " (" +
                             bound.why + ")");
            }
          }
        }
    }
  }
  for (const auto& [key, t] : tally) {
    std::printf("fit_sweep %-20s tried %6d  built at", key.c_str(), t.tried);
    for (int level = 0; level < kLevels; level++)
      std::printf("  x%g: %d", kFactors[level], t.built[level]);
    std::printf("\n");
  }
  for (const std::string& p : past)
    std::printf("fit_sweep past: %s\n", p.c_str());
  for (const auto& [key, t] : tally) {
    INFO(key);
    // A kind that refuses builds nothing past its refusal margin.
    for (const auto k :
         {fit::BoundKind::Width, fit::BoundKind::Between,
          fit::BoundKind::Curvature, fit::BoundKind::Rim, fit::BoundKind::Meet})
      if (key.ends_with(KindName(k)) && fit::Refuses(k))
        for (int level = 0; level < kLevels; level++)
          if (kFactors[level] >= 1.0 + fit::RefuseMargin(k))
            CHECK(t.built[level] == 0);
  }
}

// The evidence for fit::kReachMargin
// (docs/modeling/README.md#a-blend-moves-the-volume-it-can): every edge set
// of the same bodies, filleted and
// chamfered at a quarter, a half and nine tenths of its least bound, and
// every build the validity check passes held to the volume its blend can
// move. Prints the most each way, as a fraction of the reach, and every
// build the objection would refuse; one it refuses that is a blend is a
// margin too tight.
TEST_CASE("A blend that builds moves the volume within its reach",
          "[.fit_sweep]") {
  const char* dir = std::getenv("LOBOLE_FIT_SWEEP_DIR");
  if (dir == nullptr)
    SKIP("LOBOLE_FIT_SWEEP_DIR names no directory of BREP files");
  int shardK = 0, shardN = 1;
  if (const char* s = std::getenv("LOBOLE_FIT_SWEEP_SHARD"))
    REQUIRE(std::sscanf(s, "%d/%d", &shardK, &shardN) == 2);
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
    if (e.path().extension() == ".brep")
      files.push_back(e.path());
  std::sort(files.begin(), files.end());
  REQUIRE_FALSE(files.empty());

  int built = 0, held = 0;
  double most = 0.0; // |change| / reach on the side it moved
  std::string mostAt;
  std::vector<std::string> refused;
  for (size_t fi = 0; fi < files.size(); fi++) {
    if (static_cast<int>(fi % shardN) != shardK)
      continue;
    TopoDS_Shape whole;
    BRep_Builder builder;
    if (!BRepTools::Read(whole, files[fi].c_str(), builder))
      continue;
    int solidNo = 0;
    for (TopExp_Explorer sx(whole, TopAbs_SOLID); sx.More();
         sx.Next(), solidNo++) {
      Body body;
      body.shape = CADShape(sx.Current());
      body.createdBy = 1;
      topo::NamePrimitiveElements(body.shape, "1", body.elementMap);
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(sx.Current(), TopAbs_EDGE, edges);
      const double before = body.shape.Volume();
      for (int e = 0; e < edges.Extent(); e++)
        for (const EdgeBlendOp op :
             {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
          std::vector<int> on;
          const Body base = topo::BlendBody(body, {e}, op, on);
          const auto least = fit::BlendBound(base, on, op);
          if (!least)
            continue;
          std::vector<int> all = on;
          for (const topo::BlendContour& c :
               topo::EdgeBlendContours(base.shape, on, op))
            for (const int x : c.edges)
              if (std::find(all.begin(), all.end(), x) == all.end())
                all.push_back(x);
          const auto reach = fit::ReachOf(base, all);
          for (const double factor : {0.25, 0.5, 0.9}) {
            const double size = least->size * factor;
            const CADShape result =
                topo::EdgeBlendUnnamed(base.shape, on, op, size, false);
            if (!result.IsValid() || !Measure::InvalidReasons(result).empty())
              continue;
            built++;
            if (!reach)
              continue;
            held++;
            const fit::VolumeReach r = fit::ReachAt(*reach, op, size);
            const double change = result.Volume() - before;
            const double side = change < 0.0 ? r.remove : r.add;
            const double ratio =
                side > 0.0 ? std::abs(change) / side
                           : (std::abs(change) > 1e-9 * before ? HUGE_VAL : 0);
            const std::string at =
                files[fi].filename().string() + " solid " +
                std::to_string(solidNo) + " edge " + std::to_string(e) +
                (op == EdgeBlendOp::Chamfer ? " chamfer " : " fillet ") +
                fit::Mm(size) + ": change " + std::to_string(change) +
                " mm3, reach -" + std::to_string(r.remove) + " +" +
                std::to_string(r.add);
            if (ratio > most) {
              most = ratio;
              mostAt = at;
            }
            if (const std::string why =
                    fit::VolumeObjection(op, size, base.shape, result, r);
                !why.empty())
              refused.push_back(at);
          }
        }
    }
  }
  std::printf("reach_sweep: %d valid builds, %d held to a reach; the most "
              "%.4g of its reach (%s)\n",
              built, held, most, mostAt.c_str());
  // Each one refused is to be looked at: a blend it refuses is a reach too
  // tight. The four of the corpus are wrong bodies (README).
  for (const std::string& r : refused)
    std::printf("reach_sweep refused: %s\n", r.c_str());
  CHECK(held > 0);
}
