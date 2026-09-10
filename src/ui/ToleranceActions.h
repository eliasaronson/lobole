#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui {
struct AppState;

// One round joint of the active assembly, checked. Shared by the GUI and the
// `check_fits` RPC so a person and a script get the same verdict from the
// same walk — the arrangement CheckFasteners already uses.
struct FitCheck {
  uint64_t mateId = 0;
  std::string mateName;
  std::string mateType; // revolute | cylindrical | slider | fastened
  // pass | warn | fail | reported | unverified. Anything not fully
  // answerable is `unverified` WITH a reason and never a pass.
  std::string status;
  std::string reason;
  // Set only when the fit was computed (status != "unverified").
  bool computed = false;
  std::string holeOccurrence, shaftOccurrence;
  std::string holeClass, shaftClass;
  double size = 0.0;
  double holeMin = 0.0, holeMax = 0.0;
  double shaftMin = 0.0, shaftMax = 0.0;
  double minClearance = 0.0, maxClearance = 0.0;
  std::string kind; // clearance | transition | interference

  // One line a person can act on, for the panel and for a log.
  [[nodiscard]] std::string Sentence() const;
};

// Every fit the active assembly declares. Empty (and harmless) off an
// assembly tab.
[[nodiscard]] std::vector<FitCheck> CheckFits(AppState& state);

// The tolerance editor inside the Measure tool window: ONE picked face is a
// tolerance on its size, TWO are a tolerance on the distance between them —
// which is exactly the shape of a Tolerance's ref/ref2, so measuring and
// tolerancing are the same gesture. Draws nothing off a part tab, or when the
// picks are not two named elements of this part.
void DrawTolerancePick(AppState& state);

// Every tolerance the active part carries, as a section of the Variables
// panel — its neighbours there are the other authored numbers that sit
// outside the regen chain.
void DrawTolerancesSection(AppState& state);

} // namespace ui
