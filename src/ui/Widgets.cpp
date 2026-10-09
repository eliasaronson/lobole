// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include <imgui.h>
#include <imgui_internal.h>

#include "ui/TextFit.h"

#include <algorithm>
#include <cstdio>

namespace ui {

bool DrawJobProgress(const JobProgress& job) {
  if (!job.status.empty())
    ImGui::TextUnformatted(job.status.c_str());
  if (job.fraction >= 0.0) {
    ImGui::ProgressBar(static_cast<float>(job.fraction), ImVec2(-FLT_MIN, 0),
                       job.overlay.empty() ? nullptr : job.overlay.c_str());
  } else {
    // Indeterminate. A bar parked at 0% because the work has no fraction to
    // report reads as stuck rather than as working, and every one of the
    // five hand-rolled versions of this had learned that separately.
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                       ImVec2(-FLT_MIN, 0),
                       job.overlay.empty() ? "working" : job.overlay.c_str());
  }
  if (!job.cancellable)
    return false;
  ImGui::BeginDisabled(job.cancelAsked);
  const std::string label = std::string(ICON_FA_STOP " ") +
                            (job.cancelAsked ? "Stopping" : "Cancel") +
                            job.cancelId;
  const bool pressed = ImGui::Button(label.c_str());
  ImGui::EndDisabled();
  if (!job.cancelTip.empty())
    ImGui::SetItemTooltip("%s", job.cancelTip.c_str());
  return pressed;
}

bool SectionHeader(const char* label, int count, bool defaultOpen) {
  return SectionHeader(label, count, defaultOpen, nullptr, nullptr);
}

bool SectionHeader(const char* label, int count, bool defaultOpen,
                   const char* addId, bool* addClicked, const char* addTip) {
  if (addClicked != nullptr)
    *addClicked = false;
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;
  ImGuiContext& g = *GImGui;
  const ImGuiStyle& style = g.Style;
  const ImGuiID id = window->GetID(label);
  // Where CollapsingHeader kept it: a header a person had open stays open.
  ImGuiStorage* storage = window->DC.StateStorage;
  bool open = storage->GetInt(id, defaultOpen ? 1 : 0) != 0;

  const char* labelEnd = ImGui::FindRenderedTextEnd(label);
  const float h = ImGui::GetFrameHeight();
  const ImVec2 pos = window->DC.CursorPos;
  const float width = ImGui::GetContentRegionAvail().x;
  const ImRect bb(pos, ImVec2(pos.x + width, pos.y + h));
  ImGui::ItemSize(bb, style.FramePadding.y);
  // The "+" is submitted after the header and over it; the header yields
  // the hover to it rather than toggling under the click.
  if (addId != nullptr)
    ImGui::SetNextItemAllowOverlap();
  if (!ImGui::ItemAdd(bb, id)) {
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label,
                                g.LastItemData.StatusFlags |
                                    ImGuiItemStatusFlags_Openable |
                                    (open ? ImGuiItemStatusFlags_Opened : 0));
    return open;
  }
  bool hovered = false, held = false;
  if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
    open = !open;
    storage->SetInt(id, open ? 1 : 0);
    ImGui::MarkItemEdited(id);
  }

  ImDrawList* dl = window->DrawList;
  if (hovered || held)
    dl->AddRectFilled(bb.Min, bb.Max,
                      ImGui::GetColorU32(held ? ImGuiCol_HeaderActive
                                              : ImGuiCol_HeaderHovered),
                      style.FrameRounding);
  // A hairline under the whole row: the section's edge, so a closed section
  // still reads as a heading and not as a stray label.
  dl->AddLine(ImVec2(bb.Min.x, bb.Max.y - 1.0f),
              ImVec2(bb.Max.x, bb.Max.y - 1.0f),
              ImGui::GetColorU32(ImGuiCol_Separator));
  const float fs = ImGui::GetFontSize();
  const ImVec2 textPos(bb.Min.x + style.FramePadding.x,
                       bb.Min.y + (h - fs) * 0.5f);
  // The chevron: what makes it read as something that opens.
  dl->AddText(textPos, ImGui::GetColorU32(ImGuiCol_TextDisabled),
              open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
  const ImVec2 labelPos(textPos.x + fs * 1.4f, textPos.y);
  dl->AddText(labelPos, ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd);
  if (count >= 0) {
    const float lw = ImGui::CalcTextSize(label, labelEnd).x;
    char n[24];
    std::snprintf(n, sizeof(n), "%d", count);
    dl->AddText(ImVec2(labelPos.x + lw + fs * 0.6f, labelPos.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), n);
  }
  IMGUI_TEST_ENGINE_ITEM_INFO(id, label,
                              g.LastItemData.StatusFlags |
                                  ImGuiItemStatusFlags_Openable |
                                  (open ? ImGuiItemStatusFlags_Opened : 0));
  if (addId != nullptr) {
    // Shown while the row is hovered, like a list row's eye: a create verb
    // that belongs to the section, without a button row under every header.
    const bool rowHovered =
        ImGui::IsMouseHoveringRect(bb.Min, bb.Max) && ImGui::IsWindowHovered();
    const ImGuiID aid = window->GetID(addId);
    const ImRect abb(ImVec2(bb.Max.x - h, bb.Min.y), bb.Max);
    if (ImGui::ItemAdd(abb, aid)) {
      bool aHovered = false, aHeld = false;
      if (ImGui::ButtonBehavior(abb, aid, &aHovered, &aHeld) &&
          addClicked != nullptr)
        *addClicked = true;
      if (aHovered || aHeld)
        dl->AddRectFilled(abb.Min, abb.Max,
                          ImGui::GetColorU32(aHeld ? ImGuiCol_ButtonActive
                                                   : ImGuiCol_ButtonHovered),
                          style.FrameRounding);
      if (rowHovered || aHovered) {
        const ImVec2 gs = ImGui::CalcTextSize(ICON_FA_PLUS);
        dl->AddText(
            ImVec2(abb.Min.x + (h - gs.x) * 0.5f, abb.Min.y + (h - fs) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text), ICON_FA_PLUS);
      }
      if (aHovered && addTip != nullptr)
        ImGui::SetTooltip("%s", addTip);
    }
    IMGUI_TEST_ENGINE_ITEM_INFO(aid, addId, g.LastItemData.StatusFlags);
  }
  return open;
}

void SetListRowOpen(const std::string& disclosureId, bool open) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  window->DC.StateStorage->SetInt(window->GetID(disclosureId.c_str()),
                                  open ? 1 : 0);
}

ListRowResult ListRow(const ListRowSpec& row) {
  ListRowResult out;
  ImGuiContext& g = *GImGui; // IMGUI_TEST_ENGINE_ITEM_INFO names it
  (void)g;
  const float fs = ImGui::GetFontSize();
  const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
  const float rowLeft = ImGui::GetCursorScreenPos().x;
  const float rowRight = rowLeft + ImGui::GetContentRegionAvail().x;
  const float rowTop = ImGui::GetCursorScreenPos().y;

  // The disclosure first, when the row holds rows: a chevron like
  // SectionHeader's, its open state kept where that keeps its own.
  float lead = 0.0f;
  if (!row.disclosureId.empty()) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiStorage* storage = window->DC.StateStorage;
    const ImGuiID did = window->GetID(row.disclosureId.c_str());
    out.open = storage->GetInt(did, row.disclosureDefaultOpen ? 1 : 0) != 0;
    lead = fs * 1.3f;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + lead, pos.y + ImGui::GetFrameHeight()));
    ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
    if (ImGui::ItemAdd(bb, did)) {
      bool dHovered = false, dHeld = false;
      if (ImGui::ButtonBehavior(bb, did, &dHovered, &dHeld)) {
        out.open = !out.open;
        storage->SetInt(did, out.open ? 1 : 0);
        ImGui::MarkItemEdited(did);
      }
      const char* chevron =
          out.open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
      const ImVec2 cs = ImGui::CalcTextSize(chevron);
      window->DrawList->AddText(
          ImVec2(bb.Min.x + (lead - cs.x) * 0.5f,
                 bb.Min.y + (bb.GetHeight() - fs) * 0.5f),
          ImGui::GetColorU32(dHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
          chevron);
    }
    IMGUI_TEST_ENGINE_ITEM_INFO(
        did, row.disclosureId.c_str(),
        g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Openable |
            (out.open ? ImGuiItemStatusFlags_Opened : 0));
    ImGui::SameLine(0.0f, 0.0f);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(row.glyphColor.w > 0.0f ? row.glyphColor : dim, "%s",
                     row.glyph != nullptr ? row.glyph : "");
  // SameLine's offset is from the window's edge and ignores ImGui::Indent, so
  // an indented row (a flexible unit's parts under it) drew its label under
  // its own glyph. The indent is added back here.
  const ImGuiWindow* win = ImGui::GetCurrentWindowRead();
  const float indent =
      win->DC.Indent.x - (win->DecoOuterSizeX1 + win->WindowPadding.x -
                          win->Scroll.x); // 0 unindented
  ImGui::SameLine(lead + fs * 1.8f + indent);

  // The visible label elides into what the tail and the eye leave, keeping
  // its "###id" so the row's identity never changes with the panel's width
  // (TextFit.h). A name wider than the panel otherwise sized the row past the
  // window's edge, where it could not be hovered.
  const size_t hash = row.label.find("###");
  const std::string visible = row.label.substr(0, hash);
  const std::string id =
      hash == std::string::npos ? "###" + visible : row.label.substr(hash);
  // A tail glyph that is a BUTTON is drawn apart from the tail text, in a
  // square of its own at the right edge.
  const bool glyphButton = row.tailGlyph != nullptr && !row.tailGlyphId.empty();
  std::string tailText = row.tail;
  if (row.tailGlyph != nullptr && !glyphButton)
    tailText =
        std::string(row.tailGlyph) + (tailText.empty() ? "" : "  " + tailText);
  const float glyphW = glyphButton ? ImGui::GetFrameHeight() : 0.0f;
  const float reserve =
      (tailText.empty() ? 0.0f
                        : ImGui::CalcTextSize(tailText.c_str()).x + fs * 0.8f) +
      (row.eye != ListRowSpec::Eye::None ? fs * 1.6f : 0.0f) + glyphW;
  bool elided = false;
  const std::string shown =
      FitText(visible,
              std::max(fs * 3.0f, ImGui::GetContentRegionAvail().x - reserve),
              &elided) +
      id;
  const bool colored = row.dimmed || row.labelColor.w > 0.0f;
  if (colored)
    ImGui::PushStyleColor(ImGuiCol_Text,
                          row.labelColor.w > 0.0f ? row.labelColor : dim);
  if (ImGui::Selectable(shown.c_str(), row.selected,
                        ImGuiSelectableFlags_AllowDoubleClick |
                            ImGuiSelectableFlags_AllowOverlap)) {
    if (ImGui::IsMouseDoubleClicked(0))
      out.doubleClicked = true;
    else
      out.clicked = true;
  }
  if (colored)
    ImGui::PopStyleColor();
  // An elided name comes back AT ONCE, as TextFit's rows do: it is the
  // label, not commentary. The rest waits a moment, like any tooltip.
  if ((elided || !row.tooltip.empty()) &&
      ImGui::IsItemHovered(elided ? ImGuiHoveredFlags_None
                                  : ImGuiHoveredFlags_DelayShort)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(fs * 28.0f);
    if (elided) // the whole name, first, when the row could not show it
      ImGui::TextUnformatted(visible.c_str());
    if (!row.tooltip.empty())
      ImGui::TextUnformatted(row.tooltip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (row.menu && ImGui::BeginPopupContextItem(row.menuId.c_str())) {
    row.menu();
    ImGui::EndPopup();
  }
  const float rowBottom = ImGui::GetItemRectMax().y;
  out.hovered = ImGui::IsMouseHoveringRect(ImVec2(rowLeft, rowTop),
                                           ImVec2(rowRight, rowBottom), false);

  // The right edge: the tail, the tail glyph's button, then the eye.
  const float eyeW = row.eye != ListRowSpec::Eye::None ? fs * 1.6f : 0.0f;
  float right = rowRight - eyeW - glyphW;
  if (glyphButton) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID gid = window->GetID(row.tailGlyphId.c_str());
    const ImRect gbb(ImVec2(right, rowTop),
                     ImVec2(right + glyphW, rowTop + ImGui::GetFrameHeight()));
    if (ImGui::ItemAdd(gbb, gid)) {
      bool gHovered = false, gHeld = false;
      if (ImGui::ButtonBehavior(gbb, gid, &gHovered, &gHeld))
        out.tailClicked = true;
      if (gHovered || gHeld)
        window->DrawList->AddRectFilled(
            gbb.Min, gbb.Max,
            ImGui::GetColorU32(gHeld ? ImGuiCol_ButtonActive
                                     : ImGuiCol_ButtonHovered),
            ImGui::GetStyle().FrameRounding);
      const ImVec2 gs = ImGui::CalcTextSize(row.tailGlyph);
      window->DrawList->AddText(
          ImVec2(gbb.Min.x + (glyphW - gs.x) * 0.5f,
                 gbb.Min.y + (gbb.GetHeight() - fs) * 0.5f),
          ImGui::GetColorU32(row.tailGlyphColor.w > 0.0f ? row.tailGlyphColor
                                                         : dim),
          row.tailGlyph);
      if (gHovered && !row.tailGlyphTip.empty())
        ImGui::SetTooltip("%s", row.tailGlyphTip.c_str());
    }
    IMGUI_TEST_ENGINE_ITEM_INFO(gid, row.tailGlyphId.c_str(),
                                g.LastItemData.StatusFlags);
  }
  if (!tailText.empty()) {
    const std::string& tail = tailText;
    const float w = ImGui::CalcTextSize(tail.c_str()).x;
    ImGui::SameLine(0.0f, 0.0f);
    const float x = right - w - fs * 0.4f;
    if (x > ImGui::GetCursorScreenPos().x) {
      ImGui::SetCursorScreenPos(ImVec2(x, rowTop));
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%s", tail.c_str());
    } else {
      ImGui::NewLine();
    }
  }
  if (row.eye != ListRowSpec::Eye::None) {
    const bool hidden = row.eye == ListRowSpec::Eye::Hidden;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(right, rowTop));
    // Invisible until the row is hovered -- unless the row is hidden, when
    // the struck eye says so without asking. Always SUBMITTED, so a test (or
    // a keyboard) can reach it.
    const bool show = hidden || out.hovered;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        show ? (hidden ? dim : ImGui::GetStyleColorVec4(ImGuiCol_Text))
             : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fs * 0.2f, 0.0f));
    if (ImGui::SmallButton(((hidden ? std::string(ICON_FA_EYE_SLASH)
                                    : std::string(ICON_FA_EYE)) +
                            row.eyeId)
                               .c_str()))
      out.eyeClicked = true;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (show && ImGui::IsItemHovered())
      ImGui::SetTooltip(hidden ? "Show" : "Hide");
  }
  return out;
}

} // namespace ui
