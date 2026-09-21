#include "remote/RemoteSheetMetal.h"

#include "core/Angles.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/SheetMetal.h"
#include "core/io/ExportDxf.h"
#include "core/sheet/Check.h"
#include "core/sheet/Unfold.h"
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
     "part", Deferrable,
     "rules{material, gauge, thickness_mm, bend_radius_mm, "
     "bend_radius_source, min_bend_radius_mm, k_factor, k_factor_source, "
     "relief, relief_width_mm, relief_depth_mm | the whole object is null "
     "when the part is not sheet metal}, stock{per-material gauges}, "
     "relief_styles"},
    {"check_sheet_metal",
     "Can a brake actually make this? Every finding names the element and the "
     "required-versus-actual number: a bend tighter than the material takes "
     "(re-checked here, because changing the gauge afterwards leaves every "
     "existing bend where it was), a wall too short to sit on the die, a bend "
     "line stopping inside a panel with nothing to relieve it, a hole near "
     "enough to a bend to draw into an oval. `not_checked` says what this "
     "build cannot answer, so a clean verdict is never mistaken for one.",
     "body, part", Deferrable,
     "ok, violations[]{kind,message,actual_mm,required_mm,bend?,element?}, "
     "bends_checked, holes_checked, not_checked"},
    {"list_bends",
     "Every bend of a sheet body, as DATA: its inside radius against the "
     "material's minimum (`below_minimum` is the DFM verdict, and a HEM is "
     "exempt because it is crushed flat rather than bent round a die), the "
     "angle, the K and where that K came from, the bend allowance once the "
     "part has been flattened, how far the bend runs along its axis, and the "
     "axis itself. `side` [x,y,z] adds `folds`: UP or DOWN as seen from that "
     "side, which is the only sense a bend has (sheet::FoldsToward) — omit it "
     "and no direction is claimed. The rules the part was built under come "
     "back beside them, so a bend audit is ONE call instead of three plus a "
     "drawing note's text.",
     "part, body, side", Deferrable,
     "bends[]{index,body,feature_id,angle_deg,radius_mm,min_radius_mm,crushed,"
     "below_minimum,width_mm,k_factor,k_source,flat,axis,axis_origin,allowance_"
     "mm?,folds?}, "
     "count, "
     "rules{material,gauge,thickness_mm,bend_radius_mm,k_factor,relief}"},
    {"export_dxf",
     "Write a sheet body's FLAT PATTERN to a DXF a shop can cut: R12 ASCII, "
     "millimetres ($INSUNITS 4), outline and holes on CUT, bend lines on "
     "BEND_UP / BEND_DOWN, dashed and red/blue the way nesting software "
     "expects — UP as seen from the side the first flange was picked on. "
     "Arcs stay arcs. The body must BE flat - activate the "
     "configuration that unsuppresses its FlatPattern first - because a "
     "folded part's silhouette is a picture of a part that cuts to nothing. "
     "`mark` is a string the machine ETCHES rather than cuts, written as a "
     "TEXT entity on a MARK layer and placed inside the blank: thirty flat "
     "blanks arrive at a welding table looking alike, and the one thing that "
     "tells them apart is the name the laser wrote while cutting them.",
     "body, mark, part, path", NotInBatch},
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

bool HandleSheetMetalRpc(ui::AppState& state, Document* doc,
                         const std::string& m, const RpcParams& p, json& out) {
  if (m == "set_sheet_metal_rules") {
    // The only one here that WRITES, and it writes to the tab you are on.
    Document& active = *doc;
    if (p.value("clear", false)) {
      state.commands->Execute(
          std::make_unique<SetSheetMetalRulesCommand>(std::nullopt), active);
      out = {{"rules", nullptr}};
      AttachRegen(out, &active);
      return true;
    }
    // Start from what is already set, so changing only the relief style does
    // not silently clear the gauge.
    sheet::SheetMetalRules r;
    if (active.SheetMetal())
      r = *active.SheetMetal();
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
                            active);
    out = {{"rules", RulesJson(r)}};
    AttachRegen(out, &active);
    return true;
  }

  if (m == "check_sheet_metal" || m == "export_dxf") {
    const Document& src = SubjectPart(state, doc, p);
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
      dxf::FlatDrawing drawing{target.shape, target.bends,
                               p.value("mark", std::string{})};
      const std::string why =
          dxf::WriteFlatPattern(drawing, p.at("path").get<std::string>());
      if (!why.empty())
        throw std::runtime_error(why);
      out = {{"path", p.at("path")},
             {"bend_lines", static_cast<int>(target.bends.size())},
             {"mark", drawing.mark}};
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

  if (m == "list_bends") {
    const Document& src = SubjectPart(state, doc, p);
    if (!src.SheetMetal())
      throw std::runtime_error(
          "this part has no sheet-metal rules, so it has no bends to list "
          "(set_sheet_metal_rules)");
    const std::vector<Body>& bodies = src.GetBodies();
    std::optional<size_t> only;
    if (p.contains("body")) {
      only = p.at("body").get<size_t>();
      if (*only >= bodies.size())
        throw std::runtime_error("no body " + std::to_string(*only) +
                                 " - this part has " +
                                 std::to_string(bodies.size()));
    }
    std::optional<glm::dvec3> side;
    if (p.contains("side")) {
      const auto& v = p.at("side");
      if (!v.is_array() || v.size() != 3)
        throw std::runtime_error("side is a direction [x, y, z]");
      side = glm::dvec3(v[0].get<double>(), v[1].get<double>(),
                        v[2].get<double>());
    }
    const sheet::SheetMetalRules& rules = *src.SheetMetal();
    const double minimum =
        sheet::MinBendRadius(rules.material, rules.thickness);
    json bends = json::array();
    int index = 0;
    for (size_t b = 0; b < bodies.size(); b++) {
      if (only && b != *only)
        continue;
      if (bodies[b].reference)
        continue;
      for (const BendRegion& bend : bodies[b].bends) {
        json j = {
            {"index", ++index},
            {"body", static_cast<int>(b)},
            {"feature_id", bend.featureId},
            {"angle_deg", angles::ToDegrees(bend.angle)},
            {"radius_mm", bend.radius},
            {"min_radius_mm", minimum},
            // A hem is crushed flat, not bent round a die: its radius
            // is the gap its return leaves and is MEANT to be under
            // what the material takes.
            {"crushed", bend.crushed},
            {"below_minimum", !bend.crushed && bend.radius < minimum - 1e-9},
            {"width_mm", bend.width},
            {"k_factor", bend.kFactor},
            {"k_source", bend.kSource},
            {"flat", bend.flat},
            {"axis", {bend.axisDir.x, bend.axisDir.y, bend.axisDir.z}},
            {"axis_origin",
             {bend.axisOrigin.x, bend.axisOrigin.y, bend.axisOrigin.z}}};
        // Only once the part has been laid flat: the allowance is what the
        // unfolder worked out, not something the fold recorded.
        if (bend.allowance > 0.0)
          j["allowance_mm"] = bend.allowance;
        if (side) {
          if (sheet::SquareTo(bend, *side))
            j["folds"] = sheet::FoldsToward(bend, *side) ? "up" : "down";
          else
            j["folds"] = nullptr; // not square to that side: no answer to give
        }
        bends.push_back(std::move(j));
      }
    }
    out = {{"bends", std::move(bends)},
           {"count", index},
           {"rules", RulesJson(rules)}};
    return true;
  }

  if (m == "get_sheet_metal_rules") {
    // Read-only, so it answers about any part rather than only the active tab
    // (docs/agent-scale-followup-plan.md, W2).
    const Document& src = SubjectPart(state, doc, p);
    out = {{"rules",
            src.SheetMetal() ? RulesJson(*src.SheetMetal()) : json(nullptr)},
           {"stock", StockJson()},
           {"relief_styles", ReliefStylesJson()}};
    return true;
  }

  return false;
}

} // namespace remote
