// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SimGeometryPanel.h"

#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/fem/FemStudy.h"
#include "core/fem/SimGeometry.h"
#include "core/fem/SimStudy.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // PartDocById, OccurrencePartDoc
#include "ui/DocumentSlots.h"   // SimUndoGuard
#include "ui/ExprInput.h"
#include "ui/Icons.h"
#include "ui/PanelLayout.h" // LeftLabel
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h"
#include "ui/StudyHost.h"
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

namespace {

const ImVec4 kError(0.95f, 0.5f, 0.4f, 1.0f);
const ImVec4 kStale(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kMuted(0.6f, 0.6f, 0.6f, 1.0f);

// Wrapped at the tool window's width, as the study window's lines are: the
// window sizes itself to its content, and one long sentence must not
// widen it.
void Wrapped(const ImVec4& color, const std::string& text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

// The part whose features the simulation in front suppresses for a face on
// `occurrenceId` (0 on a part subject), and its key in SimGeometry: 0 for
// the subject part, the part's document id on an assembly.
struct PartOf {
  const Document* doc = nullptr;
  uint64_t key = 0;
};
PartOf PartForFace(AppState& state, const StudyHost& host,
                   uint64_t occurrenceId) {
  if (!host.OnAssembly())
    return {host.GeometryPart(), 0};
  const Occurrence* occ =
      host.Assembly() ? host.Assembly()->FindById(occurrenceId) : nullptr;
  if (occ == nullptr)
    return {};
  return {OccurrencePartDoc(state, *occ), occ->partId};
}

std::string FeatureLabel(const Document* part, uint64_t id) {
  const Feature* f = part ? part->FindFeatureById(id) : nullptr;
  return f ? "'" + f->GetName() + "'" : "feature " + std::to_string(id);
}

// The study the check and the refinement default read: the one in front.
const fem::Study* ActiveStudy(AppState& state, const StudyHost& host) {
  return host ? host.Find(state.simulation.activeStudyId) : nullptr;
}

// The geometry check of the study in front, as rows: each says what is
// small, how small against the element there, and the feature that made
// it; a click frames it, and a suppressible one has its feature's
// Suppress beside it.
void DrawGeometryCheck(AppState& state, const StudyHost& host) {
  const fem::Study* s = ActiveStudy(state, host);
  if (s == nullptr) {
    ImGui::TextColored(kMuted, "Add a study: the check reads its mesh size.");
    return;
  }
  const SimGeometryCheck& c = CachedGeometryCheck(state, s->id);
  if (!c.error.empty()) {
    Wrapped(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + c.error);
    return;
  }
  for (const fem::GeometryCheck::Body& b : c.check.bodies) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s: elements of %.3g mm", b.name.c_str(),
                  b.elementMm);
    ImGui::TextColored(kMuted, "%s", line);
  }
  if (c.check.findings.empty()) {
    ImGui::TextColored(kMuted, ICON_FA_CHECK
                       " Nothing smaller than the elements it is meshed at.");
    return;
  }
  // Grouped by the feature that made them: what a person suppresses.
  struct Group {
    uint64_t feature = 0;
    std::string name;
    bool suppressible = false;
    int edges = 0, faces = 0, gaps = 0;
    double smallest = 1e300;
    glm::dvec3 at{0.0};
    double element = 0.0;
  };
  std::vector<Group> groups;
  for (const fem::GeometryFinding& f : c.check.findings) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
      return g.feature == f.feature;
    });
    if (it == groups.end()) {
      groups.push_back({f.feature, f.featureName, f.suppressible});
      it = groups.end() - 1;
    }
    (f.kind == fem::GeometryFinding::Kind::Edge   ? it->edges
     : f.kind == fem::GeometryFinding::Kind::Face ? it->faces
                                                  : it->gaps)++;
    if (f.sizeMm < it->smallest) {
      it->smallest = f.sizeMm;
      it->at = f.at;
      it->element = f.elementMm;
    }
  }
  const PartOf part = PartForFace(state, host, 0);
  for (size_t i = 0; i < groups.size(); ++i) {
    const Group& g = groups[i];
    std::string what;
    const auto add = [&](int n, const char* one, const char* many) {
      if (n == 0)
        return;
      what += (what.empty() ? "" : ", ") + std::to_string(n) + " " +
              (n == 1 ? one : many);
    };
    add(g.edges, "edge", "edges");
    add(g.faces, "face", "faces");
    add(g.gaps, "gap", "gaps");
    char size[96];
    std::snprintf(size, sizeof(size), "  %.2g mm against %.3g", g.smallest,
                  g.element);
    const std::string who =
        g.feature == 0 ? std::string("No feature named") : g.name;
    ImGui::PushID(static_cast<int>(i));
    const std::string label =
        who + ": " + what + size + "###geom_finding_" + std::to_string(i);
    const float button = g.suppressible ? ImGui::GetFontSize() * 6.0f : 0.0f;
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None,
                          ImVec2(ImGui::GetFontSize() * 22.0f - button, 0)))
      FrameSimulationPlace(state, g.at, 4.0 * std::max(g.element, g.smallest));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Smaller than the elements it is meshed at: click to "
                        "frame the smallest.%s",
                        g.suppressible ? ""
                                       : "\nThe body's own first solid: "
                                         "refine the mesh instead.");
    if (g.suppressible && part.doc != nullptr) {
      ImGui::SameLine();
      if (ImGui::SmallButton(ICON_FA_EYE_SLASH " Suppress###geom_suppress")) {
        fem::SimGeometry next = host.sim->Geometry();
        if (!next.IsSuppressed(part.key, g.feature))
          next.suppressed.push_back({part.key, g.feature});
        state.simulation.placeRefusal = SetSimGeometry(state, next);
      }
    }
    ImGui::PopID();
  }
  if (!c.check.note.empty())
    Wrapped(kMuted, c.check.note);
}

// What is suppressed now, each with its way back.
void DrawSuppressedList(AppState& state, const StudyHost& host) {
  const fem::SimGeometry& g = host.sim->Geometry();
  if (g.suppressed.empty()) {
    ImGui::TextColored(kMuted, "Nothing suppressed.");
    return;
  }
  for (size_t i = 0; i < g.suppressed.size(); ++i) {
    const fem::SuppressedFeature& f = g.suppressed[i];
    const Document* part =
        f.document == 0 ? host.GeometryPart() : PartDocById(state, f.document);
    ImGui::PushID(static_cast<int>(i));
    ImGui::TextUnformatted(
        (std::string(ICON_FA_EYE_SLASH "  ") + FeatureLabel(part, f.feature))
            .c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Put back###geom_unsuppress")) {
      fem::SimGeometry next = g;
      next.suppressed.erase(next.suppressed.begin() + static_cast<long>(i));
      state.simulation.placeRefusal = SetSimGeometry(state, next);
      ImGui::PopID();
      return; // the list moved under the loop
    }
    ImGui::PopID();
  }
}

// Every solid feature of a part subject, ticked when it is suppressed for
// the analysis: what a face that is hard to click (a logo's, a thread's)
// is suppressed by. Sketches and datums make no geometry to take out.
void DrawFeatureChecklist(AppState& state, const StudyHost& host) {
  const Document* part = host.GeometryPart();
  if (part == nullptr) {
    ImGui::TextColored(kMuted, "Click a face of the meshed part.");
    return;
  }
  const fem::SimGeometry& g = host.sim->Geometry();
  const float rowH = ImGui::GetFrameHeightWithSpacing();
  int n = 0;
  for (const auto& f : part->GetFeatures())
    n += !f->IsDatum() && f->TypeName() != "Sketch";
  const float height = std::min(static_cast<float>(n), 7.0f) * rowH + 4.0f;
  if (!ImGui::BeginChild("###sim_suppress_features",
                         ImVec2(ImGui::GetFontSize() * 22.0f, height),
                         ImGuiChildFlags_None)) {
    ImGui::EndChild();
    return;
  }
  for (const auto& f : part->GetFeatures()) {
    if (f->IsDatum() || f->TypeName() == "Sketch")
      continue;
    bool on = g.IsSuppressed(0, f->GetId());
    const std::string id =
        f->GetName() + "###sim_suppress_feature_" + std::to_string(f->GetId());
    if (ImGui::Checkbox(id.c_str(), &on)) {
      fem::SimGeometry next = g;
      if (on)
        next.suppressed.push_back({0, f->GetId()});
      else
        std::erase(next.suppressed, fem::SuppressedFeature{0, f->GetId()});
      state.simulation.placeRefusal = SetSimGeometry(state, next);
      break; // the geometry moved under the loop
    }
  }
  ImGui::EndChild();
}

// The symmetry plane: a datum of the part chosen here, or a flat face
// clicked; the side kept.
void DrawSymmetryControls(AppState& state, const StudyHost& host) {
  SimulationState& sim = state.simulation;
  const fem::SimGeometry& g = host.sim->Geometry();
  const Document* part = host.GeometryPart();
  std::string current = "None";
  if (g.symmetry.datum != 0)
    current = FeatureLabel(part, g.symmetry.datum);
  else if (g.symmetry.face.IsSet())
    current = "face " + g.symmetry.face.name;
  LeftLabel("Plane");
  if (ImGui::BeginCombo("###sim_symmetry_plane", current.c_str())) {
    if (ImGui::Selectable("None###symmetry_none", !g.symmetry.On())) {
      fem::SimGeometry next = g;
      next.symmetry = {};
      sim.placeRefusal = SetSimGeometry(state, next);
    }
    if (part != nullptr)
      for (const auto& f : part->GetFeatures())
        if (dynamic_cast<const DatumPlaneFeature*>(f.get()) != nullptr) {
          const std::string id =
              f->GetName() + "###symmetry_datum_" + std::to_string(f->GetId());
          if (ImGui::Selectable(id.c_str(), g.symmetry.datum == f->GetId())) {
            fem::SimGeometry next = g;
            next.symmetry = {};
            next.symmetry.datum = f->GetId();
            next.symmetry.flip = sim.pendingSymmetryFlip;
            sim.placeRefusal = SetSimGeometry(state, next);
          }
        }
    ImGui::EndCombo();
  }
  bool flip = g.symmetry.On() ? g.symmetry.flip : sim.pendingSymmetryFlip;
  if (ImGui::Checkbox("Keep the other side###sim_symmetry_flip", &flip)) {
    sim.pendingSymmetryFlip = flip;
    if (g.symmetry.On()) {
      fem::SimGeometry next = g;
      next.symmetry.flip = flip;
      sim.placeRefusal = SetSimGeometry(state, next);
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The half kept is the one the plane's normal points "
                      "to; ticked, the other.");
}

void DrawRefinements(AppState& state, const StudyHost& host) {
  const std::vector<fem::Refinement>& refs = host.sim->Mesh().refinements;
  if (refs.empty()) {
    ImGui::TextColored(kMuted, "No refinement: Refine on the toolbar.");
    return;
  }
  for (size_t i = 0; i < refs.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    double size = refs[i].size.value;
    std::string expr = refs[i].size.expr;
    LeftLabel(refs[i].face.name.c_str());
    if ((ExprInputDouble(state, "###geom_refine_size", &size, "%.3g", &expr) ||
         ImGui::IsItemDeactivatedAfterEdit()) &&
        size > 0.0 &&
        (size != refs[i].size.value || expr != refs[i].size.expr)) {
      SimUndoGuard guard(state);
      fem::SimMesh mesh = host.sim->Mesh();
      mesh.refinements[i].size = size;
      mesh.refinements[i].size.expr = expr;
      host.sim->SetMesh(std::move(mesh));
    }
    ImGui::PopID();
  }
}

} // namespace

std::string SetSimGeometry(AppState& state, fem::SimGeometry geometry) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return "no simulation in front";
  if (geometry == host.sim->Geometry())
    return "";
  const fem::SimContext ctx = SimContextFor(state, *host.sim);
  const std::string why = fem::SimGeometryRefusal(
      ctx, geometry, [&](uint64_t id) -> const Document* {
        return id == 0 ? ctx.part : PartDocById(state, id);
      });
  if (!why.empty())
    return why;
  SimUndoGuard guard(state);
  host.sim->SetGeometry(std::move(geometry));
  return "";
}

bool SimulationGeometryPick(AppState& state, uint64_t occurrenceId,
                            const ElementRef& face) {
  using T = SimulationState::Tool;
  SimulationState& sim = state.simulation;
  if (sim.tool != T::Suppress && sim.tool != T::Symmetry &&
      sim.tool != T::Refine)
    return false;
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return true;
  sim.placeRefusal.clear();
  const PartOf part = PartForFace(state, host, occurrenceId);
  if (part.doc == nullptr) {
    sim.placeRefusal = "That face is on no part of this simulation";
    return true;
  }
  fem::SimGeometry g = host.sim->Geometry();

  if (sim.tool == T::Suppress) {
    // The feature that made the face: a hole's bore is the hole's, a
    // fillet's face the fillet's.
    const topo::ElementOrigin o = topo::DecodeElementName(face.name);
    const uint64_t madeBy = o.madeBy != 0 ? o.madeBy : o.seedTag;
    if (!o.ok || !o.derived.empty() || madeBy == 0 ||
        part.doc->FindFeatureById(madeBy) == nullptr) {
      sim.placeRefusal = "'" + face.name +
                         "' names no feature of the part that made it: "
                         "tick the feature in the list instead";
      return true;
    }
    if (g.IsSuppressed(part.key, madeBy)) {
      sim.placeRefusal = FeatureLabel(part.doc, madeBy) +
                         " is suppressed already; its Put back is below";
      return true;
    }
    g.suppressed.push_back({part.key, madeBy});
    sim.placeRefusal = SetSimGeometry(state, g);
    return true; // stays armed: the next hole
  }

  if (sim.tool == T::Symmetry) {
    if (host.OnAssembly()) {
      sim.placeRefusal = "A symmetry cut is a part simulation's";
      return true;
    }
    fem::ResolvedFace rf;
    std::string why;
    if (!fem::ResolveFace(*part.doc, face, "symmetry plane", rf, &why)) {
      sim.placeRefusal = why;
      return true;
    }
    const auto info = Measure::DescribeFaceAt(
        part.doc->GetBodies()[rf.bodyIndex], rf.faceIndex);
    if (!info || info->kind != "plane") {
      sim.placeRefusal = "'" + face.name +
                         "' is not flat: a symmetry plane is a flat face or "
                         "a datum plane (choose one above)";
      return true;
    }
    g.symmetry = {};
    g.symmetry.face = face;
    g.symmetry.flip = sim.pendingSymmetryFlip;
    sim.placeRefusal = SetSimGeometry(state, g);
    if (sim.placeRefusal.empty())
      sim.tool = T::None;
    return true;
  }

  // Refine: elements of the size typed on the face, in the simulation's
  // mesh.
  if (!(sim.pendingRefineMm > 0.0)) {
    sim.placeRefusal = "Type the element size first: above 0 mm";
    return true;
  }
  {
    fem::Study one;
    fem::Load at;
    at.face = face;
    one.loads.push_back(at);
    std::string why;
    const std::vector<uint64_t> ids = g.SuppressedIn(part.key);
    if (!ids.empty() && !fem::StudyForAnalysis(*part.doc, one, ids, &why)) {
      sim.placeRefusal = why;
      return true;
    }
  }
  fem::Refinement r;
  r.face = face;
  r.occurrenceId = host.OnAssembly() ? occurrenceId : 0;
  r.size = sim.pendingRefineMm;
  r.size.expr = sim.pendingRefineExpr;
  {
    SimUndoGuard guard(state);
    fem::SimMesh mesh = host.sim->Mesh();
    std::erase_if(mesh.refinements, [&](const fem::Refinement& x) {
      return x.face.featureId == face.featureId && x.face.name == face.name &&
             x.occurrenceId == r.occurrenceId;
    });
    mesh.refinements.push_back(r);
    host.sim->SetMesh(std::move(mesh));
  }
  sim.tool = T::None;
  return true;
}

void DrawSimGeometryTool(AppState& state) {
  using T = SimulationState::Tool;
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return;
  if (sim.tool == T::Suppress) {
    Wrapped(kMuted, "Click a face: the feature that made it leaves the "
                    "analysis (a hole, a logo, a small fillet). The part "
                    "keeps it.");
    ImGui::SeparatorText("Features");
    DrawFeatureChecklist(state, host);
    ImGui::SeparatorText("Smaller than its elements");
    DrawGeometryCheck(state, host);
  } else if (sim.tool == T::Symmetry) {
    Wrapped(kMuted, "Choose a datum plane, or click a flat face: the "
                    "analysis is cut on it and the cut held by a roller.");
    DrawSymmetryControls(state, host);
  } else if (sim.tool == T::Refine) {
    LeftLabel("Size (mm)");
    std::string expr;
    if (ExprInputDouble(state, "###sim_refine_size", &sim.pendingRefineMm,
                        "%.3g", &expr))
      sim.pendingRefineExpr = expr;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The element size on the face clicked. Until typed, "
                        "half the size the simulation meshes at.");
    Wrapped(kMuted, "Click a face: elements of this size there, in the "
                    "simulation's mesh.");
  }
  if (!sim.placeRefusal.empty())
    Wrapped(kError, sim.placeRefusal);
}

void DrawSimGeometryWindow(AppState& state) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return;
  ImGui::SeparatorText("Suppressed");
  DrawSuppressedList(state, host);
  if (!host.OnAssembly()) {
    ImGui::SeparatorText("Symmetry");
    DrawSymmetryControls(state, host);
  }
  ImGui::SeparatorText("Refinements");
  DrawRefinements(state, host);
  ImGui::SeparatorText("Smaller than its elements");
  DrawGeometryCheck(state, host);
  if (!sim.placeRefusal.empty())
    Wrapped(kError, sim.placeRefusal);
}

} // namespace ui
