#pragma once

namespace ui {
struct AppState;

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
