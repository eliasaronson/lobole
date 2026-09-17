#include "core/features/UnfoldFeature.h"

#include "core/sheet/Unfold.h"

#include <algorithm>

UnfoldFeature::UnfoldFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool UnfoldFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules, so there is no bend "
             "allowance to unfold with (set_sheet_metal_rules)");
    return false;
  }
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the sheet body to unfold no longer exists");
    return false;
  }
  if (body->bends.empty()) {
    SetError("this body has no bends to unfold");
    return false;
  }
  std::string why;
  auto flat = sheet::Unfold(*body, *ctx.sheetMetal, fixedFace, GetId(),
                            bendFeatures, &why);
  if (!flat) {
    SetError(why.empty() ? "the body could not be unfolded" : why);
    return false;
  }
  body->shape = std::move(flat->shape);
  body->elementMap = std::move(flat->map);
  body->bends = std::move(flat->bends);
  return true;
}
