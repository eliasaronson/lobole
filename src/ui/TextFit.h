// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "imgui.h"

// Text that does not fit says so, and can still be read.
//
// ImGui clips; it does not elide. A label wider than its column is cut at the
// pixel, mid-word, with nothing to say it happened — `Gangway  [assembly]
// (fixed` and `1305294.05 m` are both real, both from the left dock at its
// default 218 px. A truncated NUMBER is the dangerous one: it still looks
// like a number. The BOM shipped a 385 kg dock reading "385".
//
// That bug was fixed once already, privately, in OccurrencePanel.cpp
// (MassText + CellTooltip), and again in AppActions.cpp (VolumeText), and
// the feature tree's own Bodies list — the same failure, one panel over —
// reached neither. This header is those helpers promoted, so the next panel
// gets them by default instead of by rediscovery.
//
// The elision is done by measuring and cutting the string, not with ImGui's
// RenderTextEllipsis: the result is an ordinary text/selectable item, so
// IsItemHovered, the test engine and the remote all see exactly what they
// saw before.
namespace ui {

// The leading part of `text` that fits in `maxWidth` px, with a trailing
// ellipsis when it had to cut. maxWidth <= 0 means the remaining content
// width. Sets `elided` when something was dropped.
std::string FitText(std::string_view text, float maxWidth = 0.0f,
                    bool* elided = nullptr);

// TextUnformatted that elides, and — only when it had to — names the whole
// string on hover.
void TextEllipsis(std::string_view text, float maxWidth = 0.0f);

// TextDisabled's dimmed equivalent.
void TextDisabledEllipsis(std::string_view text, float maxWidth = 0.0f);

// Dimmed PROSE, wrapped to the panel. The right answer for a sentence — a
// hint elided to "None. Use the mate tools in t…" is readable only on hover,
// and there is no reason to hide the end of a sentence when the panel has
// rows to spare. Elision is for NAMES and NUMBERS, which have no natural
// wrap point and must line up with their neighbours.
void TextDisabledWrapped(std::string_view text);

// Selectable whose VISIBLE label elides. A "###id" suffix in `label` is kept
// intact and re-appended, so the item's identity never changes with the
// elision — a ref that resolved at one panel width resolves at every other.
bool SelectableEllipsis(const std::string& label, bool selected = false,
                        ImGuiSelectableFlags flags = 0);

} // namespace ui
