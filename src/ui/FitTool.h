// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The Fit tool (docs/modeling/README.md#datums-on-a-scan): click a point on
// a body -- a scan's mesh body or any B-Rep -- and a plane, cylinder, cone
// or sphere is grown and fitted from it, as `fit_primitive` does. Accept
// keeps it as a datum that refits every regeneration: a DatumPlane
// from_fit for a plane, a DatumAxis from_fit for a cylinder's or cone's
// axis. A sphere is reported and refused as a datum, naming why.

#include "ui/AppState.h"

namespace ui {

// Opens and arms the point pick. Does nothing on a part without a body.
void OpenFitTool(AppState& state);
// Closes; nothing to put back (the tool previews nothing in the model).
void CancelFitTool(AppState& state);
// Adds the datum as one undo step and closes. False (left open) when there
// is no fit a datum can keep.
bool AcceptFitTool(AppState& state);
// A face pick landed while the tool's point was armed.
void FitPointPicked(AppState& state, const SelectionItem& item);
// The tool window's body.
void DrawFitToolWindow(AppState& state);

} // namespace ui
