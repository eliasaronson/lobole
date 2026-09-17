#include "core/features/HemFeature.h"

#include "core/Angles.h"
#include "core/sheet/Flange.h"

std::string HemKindName(HemKind k) {
  return k == HemKind::Open ? "open" : "closed";
}

bool HemKindFromString(const std::string& name, HemKind& out) {
  if (name == "closed") {
    out = HemKind::Closed;
    return true;
  }
  if (name == "open") {
    out = HemKind::Open;
    return true;
  }
  return false;
}

const std::string& HemKindList() {
  static const std::string list = "closed, open";
  return list;
}

HemFeature::HemFeature(const std::string& name, std::vector<ElementRef> edges,
                       double length)
    : edges(std::move(edges)), length(length) {
  m_name = name;
}

bool HemFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules)");
    return false;
  }
  if (edges.empty()) {
    SetError("no edges picked to hem");
    return false;
  }
  const sheet::SheetMetalRules& rules = *ctx.sheetMetal;
  const double t = rules.thickness;
  if (length <= 1e-9) {
    SetError("a hem needs a return longer than zero");
    return false;
  }
  if (kind == HemKind::Open && gap < 0.0) {
    SetError("an open hem's gap cannot be negative");
    return false;
  }
  const double g = kind == HemKind::Closed ? t / 10.0 : gap > 0.0 ? gap : t;
  sheet::FlangeSpec spec;
  spec.radius = g / 2.0;
  spec.angle = angles::kPi;
  spec.wall = length;
  // At 180 degrees the fold's outside is R + t past its tangent line, so
  // setting the line back by that much keeps the outline where it was drawn.
  spec.setback = outside ? 0.0 : spec.radius + t;
  spec.crushed = true;
  // An inset hem stops its fold inside the panel exactly as an inset flange
  // does, and the metal tears from the end of the fold just the same — a
  // shop notches the corner. So it takes the document's relief, like any
  // other bend line that stops short.
  spec.relief = rules.reliefStyle;
  spec.reliefWidth = rules.EffectiveReliefWidth();
  spec.reliefDepth = rules.EffectiveReliefDepth(spec.radius);
  spec.offsetStart = offsetStart;
  spec.offsetEnd = offsetEnd;
  if (offsetStart < 0.0 || offsetEnd < 0.0) {
    SetError("a hem's inset cannot be negative");
    return false;
  }
  const std::string why =
      sheet::ApplyFlanges(ctx.bodies, edges, spec, rules, GetId(), t / 10.0);
  if (!why.empty()) {
    SetError(why);
    return false;
  }
  return true;
}
