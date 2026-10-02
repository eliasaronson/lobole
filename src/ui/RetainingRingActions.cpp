#include "ui/RetainingRingActions.h"

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/features/RetainingRingFeature.h"
#include "core/parts/RetainingRingTable.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // kStandardPartsFolder
#include "ui/DocumentSlots.h"

namespace ui {

uint64_t AddRetainingRing(AppState& state, const std::string& standard,
                          double size, const glm::dvec3& position,
                          const glm::dquat& orientation, std::string* error) {
  const auto fail = [&](const std::string& why) -> uint64_t {
    if (error)
      *error = why;
    return 0;
  };
  if (state.assembly == nullptr)
    return fail("a retaining ring is a bought part placed in an ASSEMBLY, "
                "not geometry in a part — on a part tab it would be machined "
                "by CAM and counted as the part's own mass. Switch to an "
                "assembly tab (set_active_part) and place it there; cut its "
                "groove in the part with a RetainingRingGroove feature.");
  // Refused BEFORE a tab exists for it: a refusal that arrives after the
  // part was created leaves the tab behind.
  parts::RetainingRingStandard which{};
  if (!parts::RetainingRingStandardFromString(standard, which))
    return fail("unknown retaining ring standard \"" + standard +
                "\" — accepted: " + parts::KnownRetainingRingStandards());
  std::string why;
  const parts::RetainingRingRow* row =
      parts::FindRetainingRing(which, size, &why);
  if (row == nullptr)
    return fail(why);

  const std::string name = parts::RetainingRingDesignation(which, *row);
  uint64_t partId = PartIdByName(state, name);
  if (partId == 0) {
    const int assemblyTab = state.activePart;
    AddPart(state, name);
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = kStandardPartsFolder;
    CreateFolder(state, kStandardPartsFolder);
    partId = slot.id;
    state.document->AddFeature(std::make_unique<RetainingRingFeature>(
        name, parts::RetainingRingStandardName(which), row->d1));
    if (assemblyTab >= 0 && assemblyTab < static_cast<int>(state.parts.size()))
      SetActivePart(state, static_cast<size_t>(assemblyTab));
  }
  if (state.assembly == nullptr)
    return fail("could not return to the assembly after creating " + name);

  Occurrence occ;
  occ.partId = partId;
  occ.name = name;
  occ.position = position;
  occ.orientation = orientation;
  return state.assembly->AddOccurrence(occ);
}

} // namespace ui
