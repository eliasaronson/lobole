#pragma once

#include "core/Occurrence.h"
#include "core/parts/BearingTable.h"
#include "ui/AssemblyActions.h" // RimPick

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

class Document;

// Bearings as generated standard parts: placing one where told, placing one
// on a picked seat, and checking the seat it is on
// (docs/parts/README.md#bearings).
namespace ui {

struct AppState;

// Place an ISO 15 bearing in the ACTIVE ASSEMBLY where told, the way
// AddFastener places a fastener: one generated part per designation, named
// "ISO 15 6200" and filed with the standard parts, so `bom` counts ten of one
// as one row. Refused on a part tab, and an unknown designation is refused
// naming the ones carried. Returns the occurrence id, or 0 with `error`.
uint64_t AddBearing(AppState& state, const std::string& designation,
                    const glm::vec3& position, const glm::quat& orientation,
                    std::string* error);

struct BearingRequest {
  // Which bearing, when the seat's diameter fits several. Either names one
  // ("6200") or narrows to a series ("62"); both empty and one candidate is
  // enough. Several candidates and neither is REFUSED naming them — a Ø10
  // shaft takes five series, and taking the first would put a 61800 where a
  // 6200 was meant.
  std::string designation;
  std::string series;
  bool dryRun = false;
};

struct BearingFit {
  bool ok = false;
  std::string why;     // the refusal, when !ok
  std::string summary; // one line: what goes where, against what
  std::string designation;
  // Every designation that fits the seat's diameter, as "6200 (10x30x9)".
  // Filled whenever the seat was read, so a picker can offer them.
  std::vector<std::string> candidates;
  // The bore goes on a journal (else: the outside goes in a bore).
  bool shaft = false;
  double seatDiameter = 0.0;
  bool shoulder = false;
  double overhang = 0.0; // mm past the end of the seat
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
  uint64_t occurrence = 0; // placed (not on a dry run)
  uint64_t mate = 0;       // the seat mate, 0 if none could be made
};

// Put a bearing on a picked SEAT: a cylindrical face (a shaft journal or a
// housing bore), or the circular rim where one meets its shoulder. The
// bearing's diameter is the seat's, its side sits against the shoulder, and a
// FASTENED mate holds the ring to the seat as placed — the bore to a journal,
// the outside to a housing bore — so `check_fits` reads the fit off it.
BearingFit FitBearing(AppState& state, const RimPick& seat,
                      const BearingRequest& request);

// A bearing's ring face, as `check_fits` needs it: the designation, whether
// it is the bore (a HOLE in fit terms) or the outside (a SHAFT), its nominal
// diameter and its ISO 492 normal-class band. nullopt when `part` is not a
// generated bearing or `face` is neither of those faces.
struct BearingRing {
  std::string designation;
  bool bore = false;
  double nominal = 0.0;
  parts::BearingBand band;
};
[[nodiscard]] std::optional<BearingRing> BearingRingOf(const Document* part,
                                                       const ElementRef& face);

// `check_seat` against a PLACED bearing: its d, D, B and chamfer read from
// the part, the seat it is mated to measured off the model — the journal or
// bore diameter, the shoulder's reach, the fillet into it, and whether the
// ring actually sits against it. With `catalogEntry` (an installed catalog,
// docs/parts/catalogs.md) the supplier's limits are checked against the same
// observations. Throws on a refusal (not a bearing, no such occurrence).
[[nodiscard]] nlohmann::json CheckSeat(AppState& state, uint64_t occurrence,
                                       const nlohmann::json* catalogEntry,
                                       const std::string& catalogDesignation);

} // namespace ui
