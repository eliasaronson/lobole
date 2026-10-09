// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SheetMetalActions.h"
#include "ui/Widgets.h"

#include "core/Angles.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/SheetMetal.h"
#include "core/features/FlatPatternFeature.h"
#include "ui/AppState.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {
namespace {

// Rows, not a table: this panel is 218 px wide by design (PanelTitles.h) and
// ImGui table cells CLIP rather than elide, so a K column reading "0.4" when
// the value is "0.41 · table:steel R/t<2" looks like a number somebody chose.
void Row(const char* label, const std::string& value,
         const char* provenance = nullptr) {
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::TextUnformatted(value.c_str());
  if (provenance != nullptr && *provenance != '\0') {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", provenance);
  }
}

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f mm", v);
  return buf;
}

// A combo over a list of strings, returning the newly chosen one.
bool StringCombo(const char* label, const std::vector<std::string>& options,
                 const std::string& current, std::string& chosen) {
  bool changed = false;
  if (ImGui::BeginCombo(label, current.c_str())) {
    for (const std::string& option : options) {
      const bool selected = option == current;
      if (ImGui::Selectable(option.c_str(), selected)) {
        chosen = option;
        changed = option != current;
      }
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Apply a rules edit as ONE undoable step, dropping it if the result is not
// something a brake could work from. The panel never writes rules straight
// onto the document: an unstocked pairing has to be refused here exactly as
// the RPC refuses it, or the two ingresses disagree.
void Apply(AppState& state, sheet::SheetMetalRules rules) {
  std::string why;
  const auto t = sheet::LookupGauge(rules.material, rules.gauge, &why);
  if (!t) {
    // Changing the material can strand the gauge — 16ga exists in all three,
    // but "6ga" is a Brown & Sharpe row and steel has none. Fall to the
    // nearest thickness that material IS stocked in rather than refusing an
    // edit the user made in the only way the panel allows.
    const double want = rules.thickness;
    double best = -1.0;
    for (const sheet::Gauge& g : sheet::GaugeTable()) {
      if (g.material != rules.material)
        continue;
      if (best < 0.0 || std::abs(g.thickness - want) < std::abs(best - want)) {
        best = g.thickness;
        rules.gauge = g.designation;
      }
    }
    if (best < 0.0)
      return;
    rules.thickness = best;
  } else {
    rules.thickness = *t;
  }
  // A pinned radius can become illegal when the material hardens under it.
  // Dropping it back to "derived" is the honest repair: the number the user
  // chose is no longer available, and the material's own minimum is.
  if (!sheet::Validate(rules).empty())
    rules.bendRadius = 0.0;
  if (!sheet::Validate(rules).empty())
    return;
  state.commands->Execute(
      std::make_unique<SetSheetMetalRulesCommand>(std::move(rules)),
      *state.document);
}

// The configuration that shows the blank, or 0. Named rather than flagged:
// a flat pattern is switched on by a configuration like any other variant.
const Configuration* FlatConfig(Document& doc) {
  for (const Configuration& c : doc.GetConfigurations())
    if (!c.unsuppressedFeatures.empty())
      for (uint64_t id : c.unsuppressedFeatures)
        if (const Feature* f = doc.FindFeatureById(id))
          if (f->TypeName() == "FlatPattern")
            return &c;
  return nullptr;
}

uint64_t FlatPatternId(const Document& doc) {
  for (const auto& f : doc.GetFeatures())
    if (f->TypeName() == "FlatPattern")
      return f->GetId();
  return 0;
}

} // namespace

void DrawSheetMetalSection(AppState& state) {
  if (!SectionHeader("Sheet metal"))
    return;
  if (state.document == nullptr) {
    ImGui::TextDisabled("Sheet metal belongs to a part.");
    return;
  }
  Document& doc = *state.document;

  if (!doc.SheetMetal()) {
    ImGui::TextWrapped("Not a sheet part. Declaring the stock is what gives a "
                       "flange its thickness.");
    if (ImGui::Button("Make this sheet metal###sm_make")) {
      sheet::SheetMetalRules rules;
      rules.material = sheet::Materials().front();
      const std::vector<std::string> gauges = sheet::GaugesFor(rules.material);
      // The thickest stocked gauge would be an odd default and the thinnest
      // a fragile one; the middle of the range is the one a shop reaches for
      // without thinking, and it is derived from the table rather than typed.
      rules.gauge = gauges[gauges.size() / 2];
      rules.thickness =
          *sheet::LookupGauge(rules.material, rules.gauge, nullptr);
      Apply(state, std::move(rules));
    }
    return;
  }

  sheet::SheetMetalRules rules = *doc.SheetMetal();
  const double radius = rules.EffectiveBendRadius();

  std::string chosen;
  if (StringCombo("Material", sheet::Materials(), rules.material, chosen)) {
    rules.material = chosen;
    Apply(state, rules);
    return; // the document changed under us; next frame redraws from it
  }
  if (StringCombo("Gauge", sheet::GaugesFor(rules.material), rules.gauge,
                  chosen)) {
    rules.gauge = chosen;
    Apply(state, rules);
    return;
  }

  Row("Thickness", Mm(rules.thickness), rules.gauge.c_str());
  Row("Bend radius", Mm(radius),
      rules.bendRadius > 0.0 ? "set" : "table: the material's minimum");
  Row(
      "K-factor",
      [&] {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", rules.EffectiveKFactor(radius));
        return std::string(buf);
      }(),
      rules.KSource(radius).c_str());
  Row("Relief", sheet::ReliefStyleName(rules.reliefStyle),
      (Mm(rules.EffectiveReliefWidth()) + " x " +
       Mm(rules.EffectiveReliefDepth(radius)))
          .c_str());

  // The blank. A flat pattern is authored off and a configuration turns it
  // on, so the toggle here is that configuration — not a hidden mode.
  const uint64_t flatId = FlatPatternId(doc);
  const Configuration* cfg = FlatConfig(doc);
  bool flat = cfg != nullptr && doc.ActiveConfiguration() == cfg->id;
  ImGui::BeginDisabled(flatId == 0);
  if (ImGui::Checkbox("Show the blank###sm_flat", &flat)) {
    if (!flat) {
      doc.SetActiveConfiguration(0);
    } else {
      if (cfg == nullptr) {
        Configuration& made = doc.AddConfiguration("Flat");
        made.unsuppressedFeatures = {flatId};
        doc.SetActiveConfiguration(made.id);
      } else {
        doc.SetActiveConfiguration(cfg->id);
      }
    }
  }
  ImGui::EndDisabled();
  if (flatId == 0)
    ImGui::TextDisabled("Add a flat pattern to see the blank.");

  // The bends. One row each, and every K says where it came from: a bend
  // whose number nobody can trace is a bend nobody can argue with when the
  // part comes back the wrong length.
  const std::vector<Body>& bodies = doc.GetBodies();
  size_t total = 0;
  for (const Body& b : bodies)
    total += b.bends.size();
  if (total == 0) {
    ImGui::TextDisabled("No bends yet.");
    return;
  }
  ImGui::Spacing();
  ImGui::Text("Bends (%zu)", total);
  int n = 0;
  for (const Body& body : bodies) {
    for (const BendRegion& bend : body.bends) {
      ++n;
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%d  %.1f deg  R %.2f", n,
                    angles::ToDegrees(bend.angle), bend.radius);
      ImGui::TextUnformatted(buf);
      ImGui::SameLine();
      ImGui::TextDisabled("K %.3f %s", bend.kFactor, bend.kSource.c_str());
      if (bend.allowance > 0.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("· eats %s", Mm(bend.allowance).c_str());
      }
    }
  }
}

} // namespace ui
