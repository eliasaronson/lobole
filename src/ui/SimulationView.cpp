// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SimulationView.h"

#include "cad/CADMaterials.h"
#include "core/fem/FemMeshReport.h"
#include "core/fem/FemVtu.h"
#include "core/io/Serialization.h"
#include "core/io/SerializeSimulation.h" // StoredMeshToJson
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "ui/AppActionsInternal.h"
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationViewInternal.h"
#include "ui/ViewActions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace ui {

namespace {

using json = nlohmann::json;

std::string Fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
  char buf[200];
  std::snprintf(buf, sizeof buf, f, a, b, c);
  return buf;
}

uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}
uint64_t Bits(double v) {
  uint64_t b;
  static_assert(sizeof b == sizeof v);
  std::memcpy(&b, &v, sizeof b);
  return b;
}

// The material of a solved body: a part's body by index, an assembly's
// meshed occurrence's placed body by index.
// The study's own material, when it states one (fem::StudyMaterialRow).
CADMaterials::MaterialProperties MaterialOfBody(AppState& state,
                                                const StudyHost& host,
                                                const fem::Study& s,
                                                int bodyIndex) {
  std::string slug;
  if (const Document* part = host.GeometryPart()) {
    const auto& bodies = part->GetBodies();
    if (bodyIndex >= 0 && bodyIndex < static_cast<int>(bodies.size()))
      slug = bodies[bodyIndex].material;
  } else {
    for (const fem::Participant& pt : host.Roles(s))
      if (pt.role == fem::ParticipantRole::Meshed) {
        std::vector<Body> bodies;
        const std::vector<Body>& flat = SimSubjectBodies(state);
        for (size_t i = 0; i < flat.size(); i++)
          if (SimBodyOccurrence(state, i) == pt.occurrenceId)
            bodies.push_back(flat[i]);
        if (bodyIndex >= 0 && bodyIndex < static_cast<int>(bodies.size()))
          slug = bodies[bodyIndex].material;
      }
  }
  return fem::StudyMaterialRow(s, slug);
}

bool SolvedOnSeveral(const StudyHost& host, const fem::Study& s) {
  if (!host.OnAssembly())
    return false;
  int n = 0;
  for (const fem::Participant& pt : host.Roles(s))
    n += pt.role == fem::ParticipantRole::Meshed ||
         pt.role == fem::ParticipantRole::Bonded;
  return n > 1;
}

} // namespace

// --- The mesh ---------------------------------------------------------------

std::string MeshSettingsKey(const fem::Study& s) {
  const json j = DocumentIO::StudyToJson(s);
  std::string key;
  for (const char* k : {"mesh_size", "mesh_size_expr", "refinements",
                        "participants", "shape_from"})
    if (j.contains(k))
      key += std::string(k) + "=" + j.at(k).dump() + ";";
  return key;
}

const MeshReport* StudyMesh(const AppState& state, uint64_t studyId) {
  const uint64_t slot = StudyHostOverrideSlot() != 0 ? StudyHostOverrideSlot()
                                                     : ActiveSlotId(state);
  const auto it = state.meshReports.find({slot, studyId});
  return it == state.meshReports.end() || !it->second.error.empty() ||
                 it->second.bodies.empty() ||
                 it->second.bodies.front().mesh == nullptr
             ? nullptr
             : &it->second;
}

std::string StudyMeshKeyNow(AppState& state, const StudyHost& host,
                            const fem::Study& s) {
  return host.sim ? SimMeshKeyNow(state, *host.sim, s) : MeshSettingsKey(s);
}

bool StudyMeshStale(const AppState& constState, uint64_t studyId) {
  AppState& state = const_cast<AppState&>(constState);
  const MeshReport* mr = StudyMesh(state, studyId);
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  if (!mr || !s)
    return false;
  return StudyMeshKeyNow(state, host, *s) != mr->settingsKey;
}

void KeepStudyMesh(AppState& state, uint64_t slotId, uint64_t studyId,
                   MeshReport report) {
  report.epoch = ++state.meshReportEpoch;
  state.meshReports[{slotId, studyId}] = std::move(report);
}

MeshReport MakeMeshReport(std::vector<fem::StudyBodyMesh> meshes,
                          std::vector<fem::MeshBodyReport> reports) {
  MeshReport out;
  for (size_t i = 0; i < meshes.size(); ++i) {
    MeshReport::Body b;
    b.name = meshes[i].bodyName;
    b.bodyIndex = meshes[i].bodyIndex;
    b.nodes = meshes[i].mesh.stats.nodeCount;
    b.elements = meshes[i].mesh.stats.tetCount;
    b.worstQuality = meshes[i].mesh.stats.worstQuality;
    if (i < reports.size())
      b.report = std::move(reports[i]);
    b.mesh = std::make_shared<const fem::FemMesh>(std::move(meshes[i].mesh));
    out.bodies.push_back(std::move(b));
  }
  return out;
}

bool MeshStudyNow(AppState& state, uint64_t studyId, std::string* error) {
  const StudyHost host = StudyHostOf(state);
  fem::Study* s = host ? host.FindForRun(studyId) : nullptr;
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!s)
    return fail("no such study");
  return MeshSimStudyNow(state, host.simSlotId, studyId, error);
}

std::string MeshFaceName(const AppState& state, const MeshReport::Body& body,
                         int faceIndex) {
  if (const Document* doc =
          StudyHostOf(const_cast<AppState&>(state)).GeometryPart()) {
    const auto& bodies = doc->GetBodies();
    if (body.bodyIndex >= 0 && body.bodyIndex < static_cast<int>(bodies.size()))
      if (const std::string* name =
              bodies[body.bodyIndex].elementMap.NameOf(faceIndex))
        return *name;
  }
  return "face " + std::to_string(faceIndex + 1);
}

std::vector<MeshFinding> MeshFindings(const AppState& state,
                                      const MeshReport& report) {
  std::vector<MeshFinding> out;
  const bool several = report.bodies.size() > 1;
  for (const MeshReport::Body& b : report.bodies) {
    const fem::MeshBodyReport& r = b.report;
    const auto add = [&](MeshFinding::Level level, const char* code,
                         std::string text) -> MeshFinding& {
      MeshFinding f;
      f.level = level;
      f.code = code;
      f.text = (several ? b.name + ": " : std::string()) + std::move(text);
      f.body = b.name;
      out.push_back(std::move(f));
      return out.back();
    };
    const auto place = [](MeshFinding& f, const glm::dvec3& at, double r) {
      f.hasPlace = true;
      f.at = at;
      f.radiusMm = r;
    };
    // The volume check: Mesh() refuses past the slack, so this row is the
    // margin it passed with.
    if (r.shapeVolumeMm3 > 0.0)
      add(MeshFinding::Level::Info, "volume",
          Fmt("Volume %.6g mm\xC2\xB3 of the body's %.6g", r.volumeMm3,
              r.shapeVolumeMm3) +
              Fmt(" (%.2g apart, %.2g allowed)",
                  std::abs(r.volumeMm3 - r.shapeVolumeMm3), r.volumeSlackMm3));
    if (r.straightenedNodes > 0)
      add(MeshFinding::Level::Warning, "straightened",
          std::to_string(r.straightenedNodes) +
              " midside nodes straightened: their curved edges folded an "
              "element, which is now flat there (a finer size on that "
              "curve keeps it)");
    for (const auto& m : r.refinementMisses) {
      MeshFinding& f =
          add(MeshFinding::Level::Warning, "refinement",
              "Refinement on " + MeshFaceName(state, b, m.faceIndex) +
                  Fmt(" did not take: asked %.3g mm, ", m.askedMm) +
                  (m.gotMm > 0.0 ? Fmt("got %.3g mm", m.gotMm)
                                 : std::string("got none")) +
                  " (" + m.why + ")");
      if (m.gotMm > 0.0)
        place(f, m.at, std::max(4.0 * m.gotMm, 1e-3));
    }
    for (const auto& w : r.thinWalls) {
      MeshFinding& f =
          add(MeshFinding::Level::Info, "thin_wall",
              "One element through between " + MeshFaceName(state, b, w.faceA) +
                  " and " + MeshFaceName(state, b, w.faceB) +
                  Fmt(": %.3g mm, ", w.thicknessMm) + std::to_string(w.tets) +
                  " elements");
      place(f, w.at, std::max(3.0 * w.thicknessMm, 2.0 * r.sizeMean));
    }
    if (r.domains > 1) {
      for (const auto& i : r.interfaces) {
        MeshFinding& f =
            add(MeshFinding::Level::Info, "interface",
                "Members " + std::to_string(i.domainA + 1) + " and " +
                    std::to_string(i.domainB + 1) + " share " +
                    std::to_string(i.sharedFaces) + " element faces");
        place(f, i.at, 4.0 * r.sizeMean);
      }
      for (int d : r.looseDomains)
        add(MeshFinding::Level::Warning, "loose",
            "Member " + std::to_string(d + 1) +
                " shares no element face with another: it is bonded to "
                "nothing");
      if (r.unsharedNodes > 0) {
        MeshFinding& f =
            add(MeshFinding::Level::Warning, "unshared",
                std::to_string(r.unsharedNodes) +
                    " nodes coincide without being shared: a seam that "
                    "touches and carries nothing");
        place(f, r.unsharedAt, 4.0 * r.sizeMean);
      }
    }
    for (size_t k = 0; k < r.worst.size(); ++k) {
      const auto& e = r.worst[k];
      MeshFinding& f = add(e.quality < 0.1 ? MeshFinding::Level::Warning
                                           : MeshFinding::Level::Info,
                           "worst_element",
                           Fmt("Quality %.2f: shape %.2f, Jacobian %.2f",
                               e.quality, e.shape, e.jacobian) +
                               Fmt(", size %.2g mm", e.size));
      place(f, e.at, std::max(2.5 * e.size, 1e-3));
      f.tet = e.tet;
    }
  }
  return out;
}

void FrameSimulationPlace(AppState& state, const glm::dvec3& at,
                          double radiusMm) {
  const glm::vec3 c(at);
  const glm::vec3 r(static_cast<float>(std::max(radiusMm, 1e-3)));
  FitCameraToWorldBox(state, c - r, c + r);
  state.simulation.placeMarked = true;
  state.simulation.placeAt = at;
}

// --- The mesh view ----------------------------------------------------------

namespace {

struct FaceKey {
  std::array<int, 3> n;
  bool operator==(const FaceKey&) const = default;
};
struct FaceKeyHash {
  size_t operator()(const FaceKey& k) const {
    return static_cast<size_t>(
        Mix(Mix(static_cast<uint64_t>(k.n[0]), static_cast<uint64_t>(k.n[1])),
            static_cast<uint64_t>(k.n[2])));
  }
};

// The four faces of a tet10 as tri6: corners, then the midside nodes of
// (0,1)(1,2)(2,0) of the face.
constexpr int kTetFace[4][6] = {{0, 1, 2, 4, 5, 6},
                                {0, 1, 3, 4, 8, 7},
                                {1, 2, 3, 5, 9, 8},
                                {0, 2, 3, 6, 9, 7}};
// tri6 -> 4 linear triangles through the midside nodes.
constexpr int kSub[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
// The barycentric coordinate of each tri6 node: zero on the element edge
// opposite, which is what the shader draws.
const glm::vec3 kBary[6] = {{1, 0, 0},       {0, 1, 0},       {0, 0, 1},
                            {0.5f, 0.5f, 0}, {0, 0.5f, 0.5f}, {0.5f, 0, 0.5f}};

void EmitTri6(std::vector<FieldVertex>& verts, std::vector<uint32_t>& indices,
              const std::array<FieldVertex, 6>& v) {
  const uint32_t base = static_cast<uint32_t>(verts.size());
  for (const FieldVertex& fv : v)
    verts.push_back(fv);
  for (const auto& s : kSub)
    for (int k : s)
      indices.push_back(base + static_cast<uint32_t>(k));
}

} // namespace

uint64_t MeshOverlayKey(const AppState& state, const MeshReport& report) {
  uint64_t key = Mix(0x4D455348ull, report.epoch);
  key = Mix(key, static_cast<uint64_t>(state.simulation.meshView));
  for (const glm::vec4& p : SectionPlanes(state))
    for (int k = 0; k < 4; ++k)
      key = Mix(key, Bits(p[k]));
  return key | 1ull;
}

void BuildMeshOverlay(AppState& state, const MeshReport& report,
                      std::vector<FieldVertex>& verts,
                      std::vector<uint32_t>& indices, FieldStyle& style,
                      ShownField& shown) {
  const SimulationState& sim = state.simulation;
  const bool byQuality = sim.meshView == SimulationState::MeshView::Quality;
  const bool bySize = sim.meshView == SimulationState::MeshView::Size;
  // A section through a mesh shows whole elements: those whose middle is on
  // the kept side, drawn uncut (a crinkle cut), so the interior tets and
  // their quality are what the cut face is made of.
  const ClipPlaneArray planes = SectionPlanes(state);
  std::vector<glm::dvec4> cuts;
  for (const glm::vec4& p : planes)
    if (glm::dot(glm::vec3(p), glm::vec3(p)) > 0.0f)
      cuts.push_back(glm::dvec4(p));

  double lo = 0.0, hi = 0.0;
  bool any = false;
  for (const MeshReport::Body& b : report.bodies) {
    if (!bySize)
      continue;
    lo = any ? std::min(lo, b.report.sizeMin) : b.report.sizeMin;
    hi = any ? std::max(hi, b.report.sizeMax) : b.report.sizeMax;
    any = true;
  }
  if (byQuality) {
    lo = 0.0;
    hi = 1.0;
  }
  shown = ShownField{};
  shown.mesh = true;
  shown.plain = !byQuality && !bySize;
  shown.title = byQuality ? "element quality" : bySize ? "element size" : "";
  shown.unit = bySize ? "mm" : "";
  shown.reversed = byQuality;
  shown.barLo = lo;
  shown.barHi = hi;
  shown.lo = lo;
  shown.hi = hi;
  if (byQuality) {
    double worst = 1.0;
    for (const MeshReport::Body& b : report.bodies)
      worst = std::min(worst, b.report.worstQuality);
    shown.lo = worst;
    shown.under = worst < 0.0;
  }

  for (const MeshReport::Body& b : report.bodies) {
    const fem::FemMesh& m = *b.mesh;
    const fem::MeshBodyReport& r = b.report;
    // Which tets are kept, then their faces met once: the skin of what is
    // kept.
    std::vector<char> kept(m.tets.size(), 1);
    if (!cuts.empty())
      for (size_t t = 0; t < m.tets.size(); ++t) {
        const auto& e = m.tets[t];
        const glm::dvec3 c = 0.25 * (m.nodes[e[0]] + m.nodes[e[1]] +
                                     m.nodes[e[2]] + m.nodes[e[3]]);
        for (const glm::dvec4& p : cuts)
          if (glm::dot(glm::dvec3(p), c) - p.w > 0.0)
            kept[t] = 0;
      }
    std::unordered_map<FaceKey, std::pair<int, int>, FaceKeyHash> skin;
    skin.reserve(m.boundary.size() * 2 + 64);
    for (size_t t = 0; t < m.tets.size(); ++t) {
      if (!kept[t])
        continue;
      const auto& e = m.tets[t];
      for (int f = 0; f < 4; ++f) {
        FaceKey key{{e[kTetFace[f][0]], e[kTetFace[f][1]], e[kTetFace[f][2]]}};
        std::sort(key.n.begin(), key.n.end());
        const auto it = skin.find(key);
        if (it == skin.end())
          skin.emplace(key, std::make_pair(static_cast<int>(t), f));
        else
          skin.erase(it);
      }
    }
    for (const auto& [key, tf] : skin) {
      const auto& e = m.tets[tf.first];
      double value = 0.0;
      if (byQuality && tf.first < static_cast<int>(r.quality.size()))
        value = r.quality[tf.first];
      else if (bySize && tf.first < static_cast<int>(r.size.size()))
        value = r.size[tf.first];
      const float scalar = hi > lo ? static_cast<float>(std::clamp(
                                         (value - lo) / (hi - lo), -1.0, 2.0))
                                   : 0.0f;
      std::array<FieldVertex, 6> v;
      for (int k = 0; k < 6; ++k) {
        v[k].position = glm::vec3(m.nodes[e[kTetFace[tf.second][k]]]);
        v[k].displacement = glm::vec3(0.0f);
        v[k].scalar = scalar;
        v[k].edge = kBary[k];
      }
      EmitTri6(verts, indices, v);
    }
  }
  style = FieldStyle{};
  style.edges = true;
  style.lit = true;
  style.plain = shown.plain;
  style.unclipped = !cuts.empty();
  style.reversed = shown.reversed;
}

// --- The field on show ------------------------------------------------------

std::optional<fem::ResultField> ResultFieldOf(SimulationState::Field f) {
  using F = SimulationState::Field;
  switch (f) {
  case F::VonMises:
    return fem::ResultField::VonMises;
  case F::Displacement:
    return fem::ResultField::Displacement;
  case F::Temperature:
    return fem::ResultField::Temperature;
  case F::HeatFlux:
    return fem::ResultField::HeatFlux;
  case F::PrincipalMax:
    return fem::ResultField::PrincipalMax;
  case F::PrincipalMid:
    return fem::ResultField::PrincipalMid;
  case F::PrincipalMin:
    return fem::ResultField::PrincipalMin;
  case F::AcrossLayers:
    return fem::ResultField::AcrossLayers;
  case F::InterlayerFactor:
    return fem::ResultField::InterlayerFactor;
  case F::FactorOfSafety:
    return fem::ResultField::FactorOfSafety;
  case F::PlasticStrain:
    return fem::ResultField::PlasticStrain;
  case F::ContactPressure:
    return fem::ResultField::ContactPressure;
  case F::None:
  case F::Shape:
  case F::Density:
    break;
  }
  return std::nullopt;
}

SimulationState::Field FieldOfResult(fem::ResultField f) {
  using F = SimulationState::Field;
  for (F c : {F::VonMises, F::Displacement, F::Temperature, F::HeatFlux,
              F::PrincipalMax, F::PrincipalMid, F::PrincipalMin,
              F::AcrossLayers, F::InterlayerFactor, F::FactorOfSafety,
              F::PlasticStrain, F::ContactPressure})
    if (ResultFieldOf(c) == f)
      return c;
  return F::None;
}

namespace {

fem::FieldInputs InputsFor(AppState& state, const StudyHost& host,
                           const fem::Study& s, const fem::StudyBodyResult& b,
                           fem::ResultField f) {
  fem::FieldInputs in;
  in.layerNormal = state.simulation.layerNormal;
  in.severalMaterials = SolvedOnSeveral(host, s);
  if (f == fem::ResultField::FactorOfSafety ||
      f == fem::ResultField::InterlayerFactor) {
    const auto& mp = MaterialOfBody(state, host, s, b.bodyIndex);
    in.yieldMPa = mp.yieldMPa;
    in.interlayerMPa = mp.interlayerMPa;
  }
  return in;
}

// What the study's kind rules out before the result is asked.
std::string KindRefusal(const StudyHost& host, const fem::Study& s,
                        SimulationState::Field field) {
  using F = SimulationState::Field;
  const bool topo = s.type == fem::AnalysisType::Topology;
  if (field == F::Shape || field == F::Density)
    return topo ? "" : "shape and density are a topology study's views";
  if (topo)
    return "a topology study shows its shape or its density";
  const bool modal = s.type == fem::AnalysisType::Modal ||
                     s.type == fem::AnalysisType::Buckling;
  if (modal && field != F::VonMises && field != F::Displacement)
    return std::string("a ") + fem::AnalysisTypeToString(s.type) +
           " study shows its mode shapes (displacement)";
  if (s.type == fem::AnalysisType::Thermal && field != F::Temperature &&
      field != F::HeatFlux)
    return "a thermal study shows temperature or heat_flux";
  return "";
}

} // namespace

std::string FieldRefusal(AppState& state, uint64_t studyId,
                         SimulationState::Field field) {
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  if (!s)
    return "no such study";
  if (field == SimulationState::Field::None)
    return "";
  if (std::string why = KindRefusal(host, *s, field); !why.empty())
    return why;
  const fem::StudyRuntime* rt = host.Results(studyId);
  if (!rt)
    return "the study has not been run";
  const auto rf = ResultFieldOf(field);
  const bool modal = s->type == fem::AnalysisType::Modal ||
                     s->type == fem::AnalysisType::Buckling;
  if (!rf || modal)
    return "";
  for (const fem::StudyBodyResult& b : rt->bodies) {
    std::string why;
    if (fem::NodalField(b.results, *rf, InputsFor(state, host, *s, b, *rf),
                        &why)
            .empty())
      return why.empty() ? "the result does not hold it" : why;
  }
  return "";
}

std::optional<double> YieldClampMPa(AppState& state, uint64_t studyId) {
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  const fem::StudyRuntime* rt = host ? host.Results(studyId) : nullptr;
  if (!s || !rt)
    return std::nullopt;
  std::optional<double> least;
  for (const fem::StudyBodyResult& b : rt->bodies) {
    const double y = MaterialOfBody(state, host, *s, b.bodyIndex).yieldMPa;
    if (y > 0.0 && (!least || y < *least))
      least = y;
  }
  return least;
}

uint64_t ResultOverlayKey(const AppState& state, const StudyHost& host,
                          const fem::StudyRuntime& rt, bool modal) {
  const SimulationState& sim = state.simulation;
  uint64_t key = Mix(0x4649454Cull, static_cast<uint64_t>(sim.field));
  key = Mix(key, sim.activeStudyId);
  key = Mix(key, host.ResultsEpoch());
  key = Mix(key, modal ? static_cast<uint64_t>(sim.activeMode) + 1 : 0);
  key =
      Mix(key,
          rt.caseNames.empty() ? 0 : static_cast<uint64_t>(sim.activeCase) + 1);
  key = Mix(key, (sim.clampLo ? 1u : 0u) | (sim.clampHi ? 2u : 0u));
  if (sim.clampLo)
    key = Mix(key, Bits(sim.rangeLo));
  if (sim.clampHi)
    key = Mix(key, Bits(sim.rangeHi));
  if (sim.field == SimulationState::Field::AcrossLayers ||
      sim.field == SimulationState::Field::InterlayerFactor)
    for (int k = 0; k < 3; ++k)
      key = Mix(key, Bits(sim.layerNormal[k]));
  // A half model's mirror image, about the plane as it is now.
  const bool mirror = sim.mirror && host.sim->Geometry().symmetry.On();
  key = Mix(key, mirror ? fem::GeometryHash(host.sim->Geometry()) : 0);
  return key | 1ull;
}

bool BuildResultOverlay(AppState& state, const StudyHost& host,
                        const fem::StudyRuntime& rt, const fem::Study& study,
                        std::vector<FieldVertex>& verts,
                        std::vector<uint32_t>& indices, FieldStyle& style,
                        ShownField& shown) {
  const SimulationState& sim = state.simulation;
  if (!KindRefusal(host, study, sim.field).empty())
    return false;
  const bool topo = study.type == fem::AnalysisType::Topology;
  const bool modal = study.type == fem::AnalysisType::Modal ||
                     study.type == fem::AnalysisType::Buckling;
  const bool density = topo && sim.field == SimulationState::Field::Density;
  const int caseIdx = rt.caseNames.empty() ? -1 : sim.activeCase;
  const auto rf = ResultFieldOf(sim.field);
  if (!density && !rf)
    return false;

  // --- Each body's value at each node, and what warps it.
  struct BodyField {
    const fem::StudyBodyResult* body = nullptr;
    std::vector<double> value;
    const std::vector<glm::dvec3>* disp = nullptr;
  };
  std::vector<BodyField> fields;
  for (const fem::StudyBodyResult& b : rt.bodies) {
    BodyField bf;
    bf.body = &b;
    const size_t n = b.mesh.nodes.size();
    if (density) {
      bf.value.assign(n, 0.0);
      std::vector<int> cnt(n, 0);
      for (size_t e = 0; e < b.mesh.tets.size() && e < b.density.size(); ++e)
        for (int i = 0; i < 10; ++i) {
          bf.value[b.mesh.tets[e][i]] += b.density[e];
          ++cnt[b.mesh.tets[e][i]];
        }
      for (size_t k = 0; k < n; ++k)
        if (cnt[k])
          bf.value[k] /= cnt[k];
    } else if (modal) {
      if (b.results.modes.empty())
        continue;
      const int mi = std::clamp(sim.activeMode, 0,
                                static_cast<int>(b.results.modes.size()) - 1);
      bf.disp = &b.results.modes[mi].displacement;
      if (bf.disp->size() != n)
        continue;
      bf.value.reserve(n);
      for (const glm::dvec3& u : *bf.disp)
        bf.value.push_back(glm::length(u));
    } else {
      const fem::FemResults& r = b.Case(caseIdx);
      bf.value = fem::NodalField(r, *rf, InputsFor(state, host, study, b, *rf),
                                 nullptr);
      if (bf.value.size() != n)
        return false; // the result does not hold the field: nothing to show
      // A thermal field is drawn on the undeformed part.
      if (*rf != fem::ResultField::Temperature &&
          *rf != fem::ResultField::HeatFlux && r.displacement.size() == n)
        bf.disp = &r.displacement;
    }
    fields.push_back(std::move(bf));
  }
  if (fields.empty())
    return false;

  // --- The field's extent, and where its ends are.
  shown = ShownField{};
  const fem::ResultFieldSpec* spec = rf ? &fem::SpecOf(*rf) : nullptr;
  shown.normalized = density || modal;
  shown.title = density ? "density" : modal ? "mode shape" : spec->title;
  shown.unit = shown.normalized ? "" : spec->unit;
  shown.reversed = !shown.normalized && spec->factor;
  bool anyUnbounded = false;
  for (const BodyField& bf : fields) {
    const fem::FieldRange range = fem::RangeOf(bf.value);
    if (range.loNode < 0)
      continue;
    for (double v : bf.value)
      anyUnbounded = anyUnbounded || v >= fem::kUnboundedFactor;
    const auto dispAt = [&](int node) {
      return bf.disp ? (*bf.disp)[node] : glm::dvec3(0.0);
    };
    if (!shown.hasExtremes || range.lo < shown.lo) {
      shown.lo = range.lo;
      shown.loAt = bf.body->mesh.nodes[range.loNode];
      shown.loDisp = dispAt(range.loNode);
      shown.loBody = bf.body->bodyName;
    }
    if (!shown.hasExtremes || range.hi > shown.hi) {
      shown.hi = range.hi;
      shown.hiAt = bf.body->mesh.nodes[range.hiNode];
      shown.hiDisp = dispAt(range.hiNode);
      shown.hiBody = bf.body->bodyName;
    }
    shown.hasExtremes = true;
  }
  if (!shown.hasExtremes)
    return false;
  // What the bar spans. A magnitude starts at zero and a signed field at
  // its least value; a normalized one is 0..1; a factor runs from its least
  // value to four times it, past which every node reads as safe.
  if (shown.normalized) {
    shown.barLo = 0.0;
    shown.barHi = 1.0;
  } else if (spec->factor) {
    shown.barLo = shown.lo;
    shown.barHi = std::min(shown.hi, 4.0 * shown.lo);
    if (!(shown.barHi > shown.barLo))
      shown.barHi = shown.hi;
  } else {
    shown.barLo = spec->isSigned ? shown.lo : 0.0;
    shown.barHi = shown.hi;
  }
  if (!shown.normalized) {
    if (sim.clampLo)
      shown.barLo = sim.rangeLo;
    if (sim.clampHi && sim.rangeHi > shown.barLo)
      shown.barHi = sim.rangeHi;
  }
  const double span = shown.barHi - shown.barLo;
  const double eps = 1e-9 * std::max(std::abs(shown.barHi), 1.0);
  shown.under = shown.lo < shown.barLo - eps;
  shown.over = shown.hi > shown.barHi + eps || anyUnbounded;
  // A factor's own top is not a limit anyone set: what is safer than it is
  // drawn in the top colour, and the tick says "and above". Only a range the
  // user set has an off-scale colour there.
  if (!shown.normalized && spec->factor && !sim.clampHi && shown.over) {
    shown.over = false;
    shown.topOpen = true;
  }
  const double tMax = shown.topOpen ? 1.0 : 2.0;

  // A half model drawn whole on request: each triangle again, reflected in
  // the symmetry plane, its displacement reflected with it and its winding
  // turned so it faces out.
  std::optional<fem::CutPlane> mirror;
  if (sim.mirror && host.sim->Geometry().symmetry.On()) {
    fem::CutPlane plane;
    if (fem::SimSymmetryPlane(SimContextFor(state, *host.sim), study.id, plane,
                              nullptr))
      mirror = plane;
  }
  const auto reflect = [&](const glm::dvec3& p, bool point) {
    const glm::dvec3 n = mirror->normal;
    const double d = glm::dot(point ? p - mirror->origin : p, n);
    return p - 2.0 * d * n;
  };

  // --- The triangles.
  for (const BodyField& bf : fields) {
    const fem::FemMesh& m = bf.body->mesh;
    for (const fem::FemMesh::BoundaryTri& tri : m.boundary) {
      std::array<FieldVertex, 6> v;
      for (int i = 0; i < 6; ++i) {
        const int n = tri.nodes[i];
        v[i].position = glm::vec3(m.nodes[n]);
        v[i].displacement =
            bf.disp ? glm::vec3((*bf.disp)[n]) : glm::vec3(0.0f);
        const double t = span > 0.0 ? (bf.value[n] - shown.barLo) / span : 0.0;
        v[i].scalar = static_cast<float>(std::clamp(t, -1.0, tMax));
        v[i].edge = glm::vec3(1.0f);
      }
      EmitTri6(verts, indices, v);
      if (mirror) {
        static constexpr int kTurned[6] = {0, 2, 1, 5, 4, 3};
        std::array<FieldVertex, 6> w;
        for (int i = 0; i < 6; ++i) {
          const int n = tri.nodes[kTurned[i]];
          w[i] = v[kTurned[i]];
          w[i].position = glm::vec3(reflect(m.nodes[n], true));
          w[i].displacement = bf.disp ? glm::vec3(reflect((*bf.disp)[n], false))
                                      : glm::vec3(0.0f);
        }
        EmitTri6(verts, indices, w);
      }
    }
  }
  style = FieldStyle{};
  style.reversed = shown.reversed;
  return true;
}

// --- The whole result -------------------------------------------------------

std::shared_ptr<const fem::StudyRuntime>
FullStudyResult(const AppState& constState, uint64_t studyId) {
  AppState& state = const_cast<AppState&>(constState);
  const StudyHost host = StudyHostOf(state);
  const fem::StudyRuntime* rt = host ? host.Results(studyId) : nullptr;
  if (!rt || rt->inputHash == 0)
    return nullptr;
  std::shared_ptr<const fem::StudyRuntime> full =
      state.studyResultsByInput.Find(rt->inputHash);
  if (!full || !fem::VtuRefusal(*full).empty())
    return nullptr;
  return full;
}

bool ExportStudyResultVtu(AppState& state, uint64_t studyId,
                          const std::string& path, int caseIndex,
                          VtuExport* out, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  if (!s)
    return fail("no such study");
  const fem::StudyRuntime* stored = host.Results(studyId);
  if (!stored)
    return fail("study has not been run");
  const std::shared_ptr<const fem::StudyRuntime> full =
      FullStudyResult(state, studyId);
  if (!full)
    return fail(fem::VtuRefusal(*stored).empty()
                    ? "the session holds no whole result for this study"
                    : fem::VtuRefusal(*stored));
  if (caseIndex >= static_cast<int>(full->caseNames.size()) &&
      !(caseIndex <= 0 && full->caseNames.empty()))
    return fail("case " + std::to_string(caseIndex) +
                " is not one of this study's load cases");
  std::string err;
  if (!DocumentIO::AtomicTargetOk(path, &err))
    return fail("path: " + err);
  fem::VtuSummary summary;
  const std::string text = fem::VtuText(
      *full, full->caseNames.empty() ? -1 : std::max(caseIndex, 0), &summary);
  if (!DocumentIO::WriteTextAtomic(path, text, &err))
    return fail("path: " + err);
  if (out) {
    out->points = summary.points;
    out->cells = summary.cells;
    out->bytes = summary.bytes;
    out->pointData = summary.pointData;
    out->cellData = summary.cellData;
  }
  return true;
}

// --- The check --------------------------------------------------------------

std::vector<fem::StudyProblem> CheckStudyNow(AppState& state, uint64_t studyId,
                                             bool thorough) {
  (void)thorough; // a simulation's check plans every time
  const StudyHost host = StudyHostOf(state);
  if (!host || host.Find(studyId) == nullptr)
    return {};
  return CheckSimStudyNow(state, host.simSlotId, studyId);
}

const StudyCheck& CachedStudyCheck(AppState& state, uint64_t studyId) {
  static const StudyCheck kNone;
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  if (!s)
    return kNone;
  uint64_t key = std::hash<std::string>{}(DocumentIO::StudyToJson(*s).dump());
  key = Mix(key, host.Version());
  key = Mix(key, studyId);
  // The subject as it is now: an edit to the part (or the assembly) the
  // check reads moves it.
  key = Mix(key, SimStampOf(state, host.sim->Subject().docId));
  if (const MeshReport* mr = StudyMesh(state, studyId))
    key = Mix(key, mr->epoch * 2 + (StudyMeshStale(state, studyId) ? 1 : 0));
  key |= 1ull;
  if (state.simulation.check && state.simulation.check->key == key)
    return *state.simulation.check;
  auto fresh = std::make_shared<StudyCheck>();
  fresh->key = key;
  fresh->studyId = studyId;
  fresh->problems = CheckStudyNow(state, studyId, false);
  state.simulation.check = fresh;
  return *state.simulation.check;
}

const SimGeometryCheck& CachedGeometryCheck(AppState& state, uint64_t studyId) {
  static const SimGeometryCheck kNone;
  const StudyHost host = StudyHostOf(state);
  const fem::Study* s = host ? host.Find(studyId) : nullptr;
  if (!s)
    return kNone;
  // What it reads: the simulation (its studies, mesh and geometry step)
  // and the subject as it is now.
  std::string key =
      std::to_string(host.simSlotId) + ":" + std::to_string(host.Version()) +
      ":" + std::to_string(studyId) + ":" + SimMeshKeyNow(state, *host.sim, *s);
  if (state.simulation.geometryCheck &&
      state.simulation.geometryCheck->key == key)
    return *state.simulation.geometryCheck;
  auto fresh = std::make_shared<SimGeometryCheck>();
  fresh->key = std::move(key);
  fresh->studyId = studyId;
  if (!fem::CheckSimGeometry(SimContextFor(state, *host.sim), studyId,
                             fresh->check, &fresh->error) &&
      fresh->error.empty())
    fresh->error = "the geometry check failed";
  state.simulation.geometryCheck = fresh;
  return *state.simulation.geometryCheck;
}

namespace {
// A value's bytes as a save writes it: the text, and its binary halves
// (what the directory form puts in the .bin beside it) at their size.
size_t SavedBytes(nlohmann::json j) {
  size_t binary = 0;
  std::function<void(nlohmann::json&)> walk = [&](nlohmann::json& v) {
    if (v.is_binary()) {
      binary += v.get_binary().size();
      v = nullptr;
    } else if (v.is_object() || v.is_array()) {
      for (auto& e : v)
        walk(e);
    }
  };
  walk(j);
  return binary + j.dump(2).size();
}
} // namespace

const SimStoredBytes& SimStoredBytesOf(AppState& state) {
  static SimStoredBytes none;
  static SimStoredBytes cached;
  static uint64_t cachedSlot = 0, cachedEpoch = ~uint64_t{0};
  static size_t cachedMeshes = 0;
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return none;
  const fem::SimDocument& sim = *host.sim;
  if (cachedSlot == host.simSlotId && cachedEpoch == sim.StudyResultsEpoch() &&
      cachedMeshes == sim.Meshes().size())
    return cached;
  cached = {};
  for (const uint64_t id : sim.ResultIds())
    if (const fem::StudyRuntime* rt = sim.StudyResults(id)) {
      nlohmann::json j;
      if (DocumentIO::StudyResultToJson(*rt, j)) {
        const size_t n = SavedBytes(std::move(j));
        cached.results[id] = n;
        cached.total += n;
      }
    }
  for (const uint64_t key : sim.KeptMeshKeys())
    if (const fem::StoredMesh* m = sim.FindMesh(key)) {
      const size_t n = SavedBytes(DocumentIO::StoredMeshToJson(key, *m));
      cached.meshes[key] = n;
      cached.total += n;
    }
  cachedSlot = host.simSlotId;
  cachedEpoch = sim.StudyResultsEpoch();
  cachedMeshes = sim.Meshes().size();
  return cached;
}

std::string ByteSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1000 * 1000)
    std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1e6);
  else if (bytes >= 1000)
    std::snprintf(buf, sizeof(buf), "%.0f kB", bytes / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%zu B", bytes);
  return buf;
}

} // namespace ui
