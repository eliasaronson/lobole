// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/RefoldFeature.h"

#include "core/sheet/Unfold.h"

#include <algorithm>

RefoldFeature::RefoldFeature(const std::string& name, uint64_t unfoldId)
    : unfoldId(unfoldId) {
  m_name = name;
}

bool RefoldFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules (set_sheet_metal_rules)");
    return false;
  }
  if (ctx.FindById(unfoldId) == nullptr ||
      ctx.FindById(unfoldId)->TypeName() != "Unfold") {
    SetError("the Unfold this refolds (id " + std::to_string(unfoldId) +
             ") is not earlier in the history");
    return false;
  }
  // The body is whichever one carries that Unfold's creases: the Unfold
  // named it, and a Refold that named it again could only disagree.
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(), [&](const Body& b) {
        return std::any_of(b.bends.begin(), b.bends.end(),
                           [&](const BendRegion& r) {
                             return r.flat && r.unfoldedBy == unfoldId;
                           });
      });
  if (body == ctx.bodies.end()) {
    SetError("nothing is left flat from that Unfold - it is suppressed, "
             "failed, or was already refolded");
    return false;
  }
  std::string why;
  auto folded = sheet::Refold(*body, *ctx.sheetMetal, unfoldId, GetId(), &why);
  if (!folded) {
    SetError(why.empty() ? "the body could not be refolded" : why);
    return false;
  }
  body->shape = std::move(folded->shape);
  body->elementMap = std::move(folded->map);
  body->bends = std::move(folded->bends);
  return true;
}
