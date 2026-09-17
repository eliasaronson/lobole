#include "remote/RemoteSheetMetal.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/SheetMetal.h"
#include "core/io/ExportDxf.h"
#include "core/sheet/Check.h"
#include "ui/AppState.h"

#include <memory>
#include <optional>
#include <vector>

using nlohmann::json;

namespace remote {

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc SheetMetalMethods[] = {
    {"set_sheet_metal_rules",
     "Declare the stock this part is cut from: the sheet-metal rules every "
     "flange reads. `material` + `gauge` fix the thickness (a gauge is a "
     "stocked designation - '16ga' or '1.5mm' - and differs BY MATERIAL: "
     "16ga steel is 1.519 mm and 16ga aluminum 1.291 mm). Everything else is "
     "derived from those two unless given: bend radius = the material's "
     "minimum, K = the R/t band of the bend chart, relief = 1.5t wide by "
     "R+t deep. Omit them and a correction to the tables reaches this part; "
     "set them and they stay put. `clear: true` makes it not a sheet part "
     "again. Rebuilds every sheet feature. get_sheet_metal_rules with no "
     "rules set lists the materials and gauges carried.",
     "material, gauge, bend_radius_mm, k_factor, relief, relief_width_mm, "
     "relief_depth_mm, clear",
     NeedsPart},
    {"get_sheet_metal_rules",
     "The part's sheet-metal rules, each derived field RESOLVED to the number "
     "a flange will use and labelled with where it came from, plus the "
     "materials and gauges this build carries. `rules` is null when the part "
     "is not sheet metal.",
     "part", NeedsPart | Deferrable},
    {"check_sheet_metal",
     "Can a brake actually make this? Every finding names the element and the "
     "required-versus-actual number: a bend tighter than the material takes "
     "(re-checked here, because changing the gauge afterwards leaves every "
     "existing bend where it was), a wall too short to sit on the die, a bend "
     "line stopping inside a panel with nothing to relieve it, a hole near "
     "enough to a bend to draw into an oval. `not_checked` says what this "
     "build cannot answer, so a clean verdict is never mistaken for one.",
     "body, part", NeedsPart | Deferrable},
    {"export_dxf",
     "Write a sheet body's FLAT PATTERN to a DXF a shop can cut: R12 ASCII, "
     "millimetres ($INSUNITS 4), outline and holes on CUT, bend lines on "
     "BEND_UP / BEND_DOWN, dashed and red/blue the way nesting software "
     "expects — UP as seen from the side the first flange was picked on. "
     "Arcs stay arcs. The body must BE flat - activate the "
     "configuration that unsuppresses its FlatPattern first - because a "
     "folded part's silhouette is a picture of a part that cuts to nothing.",
     "path, body, part", NotInBatch},
};

std::span<const MethodDoc> SheetMetalMethodDocs() { return SheetMetalMethods; }

namespace {

// The rules as an agent reads them back: what was stored, and next to it the
// number a flange will actually use. A caller that sees only `bend_radius: 0`
// cannot tell "unset" from "zero", and the resolved column is what makes the
// derivation inspectable rather than a surprise at the brake.
json RulesJson(const sheet::SheetMetalRules& r) {
  const double radius = r.EffectiveBendRadius();
  return {{"material", r.material},
          {"gauge", r.gauge},
          {"thickness_mm", r.thickness},
          {"bend_radius_mm", radius},
          {"bend_radius_source", r.bendRadius > 0.0 ? "set" : "table:minimum"},
          {"min_bend_radius_mm", sheet::MinBendRadius(r.material, r.thickness)},
          {"k_factor", r.EffectiveKFactor(radius)},
          {"k_factor_source", r.KSource(radius)},
          {"relief", sheet::ReliefStyleName(r.reliefStyle)},
          {"relief_width_mm", r.EffectiveReliefWidth()},
          {"relief_depth_mm", r.EffectiveReliefDepth(radius)}};
}

// Every material and its stocked designations — the list IS the scope, so a
// picker built from it cannot offer a row set_sheet_metal_rules would refuse.
json StockJson() {
  json materials = json::object();
  for (const sheet::Gauge& g : sheet::GaugeTable()) {
    if (!materials.contains(g.material))
      materials[g.material] = json::array();
    materials[g.material].push_back(
        {{"gauge", g.designation}, {"thickness_mm", g.thickness}});
  }
  return materials;
}

// From the enum, not a literal list: a style added to one and not the other
// is a picker offering what the parser refuses, or the reverse.
json ReliefStylesJson() {
  json arr = json::array();
  for (const sheet::ReliefStyle s :
       {sheet::ReliefStyle::None, sheet::ReliefStyle::Rectangular,
        sheet::ReliefStyle::Obround, sheet::ReliefStyle::Tear})
    arr.push_back(sheet::ReliefStyleName(s));
  return arr;
}

} // namespace

bool HandleSheetMetalRpc(ui::AppState& state, Document& doc,
                         const std::string& m, const RpcParams& p, json& out) {
  if (m == "set_sheet_metal_rules") {
    if (p.value("clear", false)) {
      state.commands->Execute(
          std::make_unique<SetSheetMetalRulesCommand>(std::nullopt), doc);
      out = {{"rules", nullptr}};
      AttachRegen(out, &doc);
      return true;
    }
    // Start from what is already set, so changing only the relief style does
    // not silently clear the gauge.
    sheet::SheetMetalRules r;
    if (doc.SheetMetal())
      r = *doc.SheetMetal();
    if (p.contains("material"))
      r.material = p.at("material").get<std::string>();
    if (p.contains("gauge"))
      r.gauge = p.at("gauge").get<std::string>();
    if (r.material.empty() || r.gauge.empty()) {
      throw std::runtime_error(
          "set_sheet_metal_rules needs a material and a gauge - a flange "
          "cannot guess its own thickness. get_sheet_metal_rules lists what "
          "is stocked.");
    }
    // The gauge IS the thickness: resolving here (rather than storing a
    // number the caller supplies) is what stops a document claiming 16ga at
    // 1.2 mm.
    std::string why;
    const std::optional<double> t =
        sheet::LookupGauge(r.material, r.gauge, &why);
    if (!t)
      throw std::runtime_error(why);
    r.thickness = *t;
    if (p.contains("bend_radius_mm"))
      r.bendRadius = p.at("bend_radius_mm").get<double>();
    if (p.contains("k_factor"))
      r.kFactor = p.at("k_factor").get<double>();
    if (p.contains("relief")) {
      const std::string v = p.at("relief").get<std::string>();
      if (!sheet::ReliefStyleFromString(v, r.reliefStyle)) {
        throw std::runtime_error(
            "unknown relief style '" + v +
            "' (expected one of: " + sheet::ReliefStyleList() + ")");
      }
    }
    if (p.contains("relief_width_mm"))
      r.reliefWidth = p.at("relief_width_mm").get<double>();
    if (p.contains("relief_depth_mm"))
      r.reliefDepth = p.at("relief_depth_mm").get<double>();
    if (const std::string bad = sheet::Validate(r); !bad.empty())
      throw std::runtime_error(bad);

    state.commands->Execute(std::make_unique<SetSheetMetalRulesCommand>(r),
                            doc);
    out = {{"rules", RulesJson(r)}};
    AttachRegen(out, &doc);
    return true;
  }

  if (m == "check_sheet_metal" || m == "export_dxf") {
    const Document& src =
        p.contains("part") ? *ResolvePartParam(state, p.at("part")) : doc;
    const std::vector<Body>& bodies = src.GetBodies();
    if (bodies.empty())
      throw std::runtime_error("this part has no bodies");
    const size_t idx = p.value("body", size_t{0});
    if (idx >= bodies.size())
      throw std::runtime_error("no body " + std::to_string(idx) +
                               " - this "
                               "part has " +
                               std::to_string(bodies.size()));
    const Body& target = bodies[idx];
    if (!src.SheetMetal())
      throw std::runtime_error(
          "this part has no sheet-metal rules, so there is nothing to check "
          "it against (set_sheet_metal_rules)");

    if (m == "export_dxf") {
      dxf::FlatDrawing drawing{target.shape, target.bends};
      const std::string why =
          dxf::WriteFlatPattern(drawing, p.at("path").get<std::string>());
      if (!why.empty())
        throw std::runtime_error(why);
      out = {{"path", p.at("path")},
             {"bend_lines", static_cast<int>(target.bends.size())}};
      return true;
    }

    const sheet::CheckResult r = sheet::Check(target, *src.SheetMetal());
    json violations = json::array();
    for (const sheet::Violation& v : r.violations) {
      json j = {{"kind", v.kind},
                {"message", v.message},
                {"actual_mm", v.actual},
                {"required_mm", v.required}};
      if (v.bend != 0)
        j["bend"] = v.bend;
      if (!v.element.name.empty())
        j["element"] = {{"feature_id", v.element.featureId},
                        {"element", v.element.name}};
      violations.push_back(std::move(j));
    }
    out = {{"ok", r.violations.empty()},
           {"violations", std::move(violations)},
           {"bends_checked", r.bendsChecked},
           {"holes_checked", r.holesChecked},
           {"not_checked", r.notChecked}};
    return true;
  }

  if (m == "get_sheet_metal_rules") {
    // Read-only, so it answers about any part rather than only the active tab
    // (docs/agent-scale-followup-plan.md, W2).
    const Document& src =
        p.contains("part") ? *ResolvePartParam(state, p.at("part")) : doc;
    out = {{"rules",
            src.SheetMetal() ? RulesJson(*src.SheetMetal()) : json(nullptr)},
           {"stock", StockJson()},
           {"relief_styles", ReliefStylesJson()}};
    return true;
  }

  return false;
}

} // namespace remote
