// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The evidence a bound kind refuses on (fit::Refuses,
// docs/modeling/README.md#bounds-that-refuse): every edge of every body in a
// directory of BREP files, alone and with the edge its face's width ends at,
// filleted and chamfered past its least bound -- a thousandth, a tenth, a
// half, twice and four times -- and every build that gives a valid solid
// there counted. A bound
// kind refuses only while this finds none of its own.
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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
  }
  return "?";
}

// The factors past the least bound each edge set is built at.
constexpr double kFactors[] = {1.0 + fit::kRefuseMargin, 1.1, 1.5, 2.0, 4.0};
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
          const auto least = fit::BlendBound(body, set, op);
          if (!least)
            continue;
          const std::string key =
              std::string(op == EdgeBlendOp::Chamfer ? "chamfer " : "fillet ") +
              KindName(least->kind);
          Tally& t = tally[key];
          t.tried++;
          for (int level = 0; level < kLevels; level++) {
            const double size = least->size * kFactors[level];
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
                           ": " + key + " " + fit::Mm(least->size) +
                           " mm, built at " + fit::Mm(size) + " (" +
                           least->why + ")");
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
    const bool refuses = [&] {
      for (const auto k : {fit::BoundKind::Width, fit::BoundKind::Between,
                           fit::BoundKind::Curvature, fit::BoundKind::Rim})
        if (key.ends_with(KindName(k)))
          return fit::Refuses(k);
      return false;
    }();
    if (refuses)
      for (const int n : t.built)
        CHECK(n == 0);
  }
}
