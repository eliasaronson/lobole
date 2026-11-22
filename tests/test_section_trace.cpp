// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Tracing a section (docs/sketching/README.md#tracing-a-section): a scanned
// slot's cut traced as real lines and arcs joined end to end, saved with
// where each came from; a re-scan that moves the section leaves the traced
// geometry where it was, and section_fit names the piece that is off the
// cut; through the pipeline, the traced profile extruded stands on the scan
// within its tolerance.

#include "TestIO.h"
#include "mesh_organic.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/ScanCompare.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SectionTrace.h"
#include "core/sketch/SketchSolver.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <random>

using Catch::Matchers::ContainsSubstring;

namespace {

constexpr double kPi = angles::kPi;

// A scanned slot standing on z = 0, `height` tall: centres `len` apart,
// ends of radius r, its centre at `at`. Rings of vertices every `spacing`
// round the outline and up the wall, each moved along its normal by
// Gaussian noise; the caps fanned from their centres. Closed.
mesh::TriMesh ScannedSlot(double len, double r, double height, double spacing,
                          double noise, uint32_t seed,
                          glm::dvec2 at = glm::dvec2(0.0)) {
  std::vector<glm::dvec2> ring, normal;
  const double half = len / 2;
  const auto line = [&](glm::dvec2 a, glm::dvec2 b, glm::dvec2 n) {
    const int k = std::max(1, int(std::round(glm::length(b - a) / spacing)));
    for (int i = 0; i < k; i++) {
      ring.push_back(a + (b - a) * (double(i) / k));
      normal.push_back(n);
    }
  };
  const auto arc = [&](glm::dvec2 c, double a0) {
    const int k = std::max(2, int(std::round(kPi * r / spacing)));
    for (int i = 0; i < k; i++) {
      const double t = a0 + kPi * double(i) / k;
      const glm::dvec2 n(std::cos(t), std::sin(t));
      ring.push_back(c + r * n);
      normal.push_back(n);
    }
  };
  line({-half, -r}, {half, -r}, {0, -1});
  arc({half, 0}, -kPi / 2);
  line({half, r}, {-half, r}, {0, 1});
  arc({-half, 0}, kPi / 2);
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, noise);
  mesh::TriMesh m;
  const int levels = std::max(1, int(std::round(height / spacing)));
  const uint32_t n = static_cast<uint32_t>(ring.size());
  for (int z = 0; z <= levels; z++)
    for (uint32_t i = 0; i < n; i++) {
      const glm::dvec2 p = ring[i] + normal[i] * gauss(rng) + at;
      m.positions.push_back({p.x, p.y, height * z / levels});
    }
  for (int z = 0; z < levels; z++)
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t a = z * n + i, b = z * n + (i + 1) % n;
      m.triangles.push_back({a, b, b + n});
      m.triangles.push_back({a, b + n, a + n});
    }
  const uint32_t bottom = static_cast<uint32_t>(m.positions.size());
  m.positions.push_back({at.x, at.y, 0.0});
  const uint32_t top = bottom + 1;
  m.positions.push_back({at.x, at.y, height});
  const uint32_t topRing = static_cast<uint32_t>(levels) * n;
  for (uint32_t i = 0; i < n; i++) {
    m.triangles.push_back({bottom, (i + 1) % n, i});
    m.triangles.push_back({top, topRing + i, topRing + (i + 1) % n});
  }
  return m;
}

uint64_t ImportScan(Document& doc, const mesh::TriMesh& m,
                    const std::string& tag) {
  const auto stl = fixtures::WriteStl(m, tag);
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  doc.AddFeature(std::move(imp));
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(doc.GetBodies()[0].IsMesh());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// A sketch across the scan at z = 10 with its pieces section.
SketchFeature& AddCut(Document& doc) {
  auto sk = std::make_unique<SketchFeature>(
      "Cut", SketchPlane(glm::dvec3(0, 0, 10), {0, 0, 1}, {1, 0, 0}));
  sk->AddSectionGeometry({}, 0.0, sketch::SectionFit::Pieces);
  doc.AddFeature(std::move(sk));
  auto& s =
      *dynamic_cast<SketchFeature*>(doc.GetFeature(doc.FeatureCount() - 1));
  INFO(s.GetError());
  REQUIRE(s.GetError().empty());
  return s;
}

void Solve(SketchFeature& sk) {
  sketch::Solver solver;
  solver.Load(sk);
  REQUIRE(solver.Solve().ok);
  solver.WriteBack(sk.entities);
}

} // namespace

TEST_CASE("A scanned slot's section traces as real lines and arcs",
          "[sketch][pieces]") {
  Document doc;
  ImportScan(doc, ScannedSlot(40, 6, 20, 0.25, 0.03, 11), "trace-slot");
  SketchFeature& sk = AddCut(doc);
  const sketch::ExternalGeom& ext = sk.external[0];
  REQUIRE(ext.genIds.size() == 4); // the pieces section: the slot's own
  const sketch::TraceResult tr = sketch::TraceSection(sk, 0, 0.5);
  REQUIRE(tr.error.empty());
  REQUIRE(tr.ids.size() == 4);
  CHECK(tr.coincident == 4);
  Solve(sk);
  int lines = 0, arcs = 0;
  for (size_t k = 0; k < tr.ids.size(); k++) {
    const sketch::Entity& e = sk.entities[sk.IndexOfEntity(tr.ids[k])];
    CHECK_FALSE(sketch::IsConstruction(e));
    CHECK(sk.GeneratedBy(tr.ids[k]).empty());
    if (const auto* a = std::get_if<sketch::Arc>(&e)) {
      arcs++;
      CHECK(std::abs(a->radius - 6.0) <
            3.0 * tr.pieces[k].radiusSigmaMm + 0.02);
    } else {
      lines++;
    }
  }
  CHECK(lines == 2);
  CHECK(arcs == 2);
  // Saved with where each came from, and read back so.
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  REQUIRE(j.at("traced").size() == 4);
  CHECK(j.at("traced").at(0).at("section") == 0);
  CHECK(j.at("traced").at(0).at("chain") == 0);
  auto back = DocumentIO::FeatureFromJson(j);
  const auto* s = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(s != nullptr);
  REQUIRE(s->traced.size() == 4);
  CHECK(s->traced[0].entity == sk.traced[0].entity);
  CHECK(s->traced[0].from == sk.traced[0].from);
  CHECK(s->traced[0].to == sk.traced[0].to);
  // Refused: a record naming no entity, or no section.
  nlohmann::json bad = j;
  bad["traced"][0]["entity"] = 12345;
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    ContainsSubstring("does not have"));
  bad = j;
  bad["traced"][0]["section"] = 3;
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    ContainsSubstring("not a section"));
  bad = j;
  bad["traced"][0]["colour"] = 1;
  CHECK_THROWS(DocumentIO::FeatureFromJson(bad));
  // A deleted entity takes its record with it.
  sk.DeleteEntity(tr.ids[0]);
  CHECK(sk.traced.size() == 3);
  // Refused, changing nothing: not a section.
  const size_t had = sk.entities.size();
  CHECK_FALSE(sketch::TraceSection(sk, 7, 0.5).error.empty());
  CHECK(sk.entities.size() == had);
}

TEST_CASE("A re-scan moves the section, not the traced sketch",
          "[sketch][pieces]") {
  Document doc;
  ImportScan(doc, ScannedSlot(40, 6, 20, 0.25, 0.03, 12), "rescan-before");
  SketchFeature* sk = &AddCut(doc);
  const uint64_t id = sk->GetId();
  const sketch::TraceResult tr = sketch::TraceSection(*sk, 0, 0.5);
  REQUIRE(tr.ids.size() == 4);
  Solve(*sk);
  for (const sketch::TracedMiss& m : sketch::SectionFitReport(*sk)) {
    INFO("entity " << m.from.entity << " sigma " << m.miss.sigmaMm
                   << " stretch " << m.miss.stretchMm);
    CHECK(m.cut);
    CHECK(m.miss.explained); // on its own cut
    CHECK(m.miss.sigmaMm < 0.06);
  }
  std::vector<sketch::Entity> drawn;
  for (uint32_t t : tr.ids)
    drawn.push_back(sk->entities[sk->IndexOfEntity(t)]);
  // The re-scan: the slot 1 mm longer, its +x end moved out.
  const auto stl = fixtures::WriteStl(
      ScannedSlot(41, 6, 20, 0.25, 0.03, 13, {0.5, 0.0}), "rescan-after");
  auto* imp = dynamic_cast<ImportFeature*>(doc.GetFeature(0));
  REQUIRE(imp != nullptr);
  imp->path = stl.string();
  doc.Regenerate();
  sk = dynamic_cast<SketchFeature*>(doc.FindFeatureById(id));
  REQUIRE(sk != nullptr);
  REQUIRE(sk->GetError().empty());
  for (size_t k = 0; k < tr.ids.size(); k++) {
    const sketch::Entity& now = sk->entities[sk->IndexOfEntity(tr.ids[k])];
    for (uint8_t w = 0; w < 3; w++)
      CHECK(glm::length(sketch::EntityPointUV(now, w) -
                        sketch::EntityPointUV(drawn[k], w)) < 1e-9);
  }
  // section_fit: the +x end's arc is off the cut by the millimetre, and
  // says where; the -x end and the sides are on it.
  int off = 0;
  for (const sketch::TracedMiss& m : sketch::SectionFitReport(*sk)) {
    const sketch::Entity& e = sk->entities[sk->IndexOfEntity(m.from.entity)];
    const auto* a = std::get_if<sketch::Arc>(&e);
    const bool plusEnd = a != nullptr && a->center.x > 0.0;
    INFO("entity " << m.from.entity << (plusEnd ? " (+x arc)" : "") << " cut "
                   << m.cut << " mean " << m.miss.meanMm << " stretch "
                   << m.miss.stretchMm);
    if (plusEnd) {
      CHECK((!m.cut || !m.miss.explained));
      if (m.cut) {
        CHECK(m.miss.stretchMm > 0.5);
        CHECK(m.miss.stretchAt.x > 20.0);
      }
      off++;
    } else {
      CHECK(m.cut);
      CHECK(m.miss.explained);
    }
  }
  CHECK(off == 1);
}

TEST_CASE("A traced scan's profile, extruded, stands on the scan",
          "[sketch][pieces][mesh_body]") {
  // Through the pipeline: the traced slot extruded the scan's height, and
  // compare_scan's answer
  // (docs/inspect/README.md#how-far-a-body-is-from-a-scan).
  Document doc;
  const uint64_t scan =
      ImportScan(doc, ScannedSlot(40, 6, 20, 0.25, 0.03, 14), "trace-pipeline");
  SketchFeature& sk = AddCut(doc);
  const uint64_t sketchId = sk.GetId();
  // The profile on a plane of its own at the scan's foot, so the section
  // (construction) and the traced loop are the only geometry.
  REQUIRE(sketch::TraceSection(sk, 0, 0.5).error.empty());
  Solve(sk);
  doc.Regenerate();
  auto ex = std::make_unique<ExtrudeFeature>("Body", sketchId, 20.0,
                                             ExtrudeMode::NewBody);
  ex->endMode = ExtrudeEnd::Symmetric;
  doc.AddFeature(std::move(ex));
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetBodies().size() == 2);
  const uint64_t body = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  scancompare::Input in;
  REQUIRE(scancompare::Gather(doc, {scan, body, 0.1}, in).empty());
  const scancompare::Outcome out = scancompare::Run(in, nullptr, nullptr);
  INFO(out.reply.dump());
  // The scan's own noise is 0.03: within three of it nearly everywhere but
  // the caps' fans, whose edges the wall's noise moves.
  CHECK(out.deviation.within > 0.97);
  CHECK(std::abs(out.deviation.meanMm) < 0.02);
}

TEST_CASE("A B-Rep slot's exact section traces as its own lines and arcs",
          "[sketch][pieces]") {
  // A B-Rep's cut is exact and sparse: a straight edge is its two ends.
  Document doc;
  auto slot =
      std::make_unique<SketchFeature>("Slot", SketchPlane(PlaneType::XY));
  slot->AddEntity(sketch::Line{{-20, -6}, {20, -6}});
  slot->AddEntity(sketch::Arc{{20, 0}, 6, -kPi / 2, kPi / 2});
  slot->AddEntity(sketch::Line{{20, 6}, {-20, 6}});
  slot->AddEntity(sketch::Arc{{-20, 0}, 6, kPi / 2, 3 * kPi / 2});
  doc.AddFeature(std::move(slot));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Body", doc.GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);
  SketchFeature& sk = AddCut(doc);
  const sketch::TraceResult tr = sketch::TraceSection(sk, 0, 0.5);
  REQUIRE(tr.error.empty());
  std::string kinds;
  for (uint32_t id : tr.ids) {
    const auto& e = sk.entities[sk.IndexOfEntity(id)];
    kinds += std::holds_alternative<sketch::Line>(e)  ? "L"
             : std::holds_alternative<sketch::Arc>(e) ? "A"
                                                      : "?";
    if (const auto* a = std::get_if<sketch::Arc>(&e))
      kinds += "(" + std::to_string(a->radius) + " " +
               std::to_string(a->endAngle - a->startAngle) + ")";
  }
  INFO("kinds " << kinds << ", cut points "
                << sk.external[0].cut[0].points.size());
  CHECK(tr.ids.size() == 4);
  CHECK(std::count(kinds.begin(), kinds.end(), 'L') == 2);
  CHECK(std::count(kinds.begin(), kinds.end(), 'A') == 2);
  for (const auto& p : tr.pieces)
    if (p.arc)
      CHECK(p.sigmaMm < 1e-3);
}
