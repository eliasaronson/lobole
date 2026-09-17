#include "core/features/FlatPatternFeature.h"

#include "core/sheet/Unfold.h"

#include <algorithm>

FlatPatternFeature::FlatPatternFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool FlatPatternFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules, so there is no bend "
             "allowance to flatten with (set_sheet_metal_rules)");
    return false;
  }
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the sheet body to flatten no longer exists");
    return false;
  }

  std::string why;
  auto flat = sheet::Unfold(*body, *ctx.sheetMetal, fixedFace, GetId(), &why);
  if (!flat) {
    SetError(why.empty() ? "the body could not be flattened" : why);
    return false;
  }
  if (body->bends.empty()) {
    // Already flat. Worth saying rather than leaving the tree looking as if
    // something happened: a part with no bends has no blank distinct from
    // itself.
    SetWarning("this body has no bends, so its flat pattern is the body");
  }
  body->shape = std::move(flat->shape);
  body->elementMap = std::move(flat->map);
  body->bends = std::move(flat->creases);
  return true;
}
