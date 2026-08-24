#include "ui/TextFit.h"

namespace ui {

namespace {

// U+2026, the character the UI font is loaded for (Theme.cpp kUiRanges) —
// three periods would be a different width and a worse glyph.
constexpr const char* kEllipsis = "\xE2\x80\xA6";

float TextWidth(std::string_view s) {
  return ImGui::CalcTextSize(s.data(), s.data() + s.size()).x;
}

// A tooltip carrying the whole string, for the item just submitted.
void FullTextTooltip(std::string_view text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
}

} // namespace

std::string FitText(std::string_view text, float maxWidth, bool* elided) {
  if (elided != nullptr)
    *elided = false;
  if (maxWidth <= 0.0f)
    maxWidth = ImGui::GetContentRegionAvail().x;
  if (maxWidth <= 0.0f || text.empty() || TextWidth(text) <= maxWidth)
    return std::string(text);

  const float room = maxWidth - TextWidth(kEllipsis);
  if (room <= 0.0f) {
    if (elided != nullptr)
      *elided = true;
    return kEllipsis; // narrower than the ellipsis itself
  }
  // Binary search on the cut point, then walk back off any UTF-8
  // continuation byte so a multi-byte glyph is never split in half.
  size_t lo = 0, hi = text.size();
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    if (TextWidth(text.substr(0, mid)) <= room)
      lo = mid;
    else
      hi = mid - 1;
  }
  while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80)
    lo--;
  if (elided != nullptr)
    *elided = true;
  return std::string(text.substr(0, lo)) + kEllipsis;
}

void TextEllipsis(std::string_view text, float maxWidth) {
  bool elided = false;
  const std::string shown = FitText(text, maxWidth, &elided);
  ImGui::TextUnformatted(shown.c_str());
  if (elided)
    FullTextTooltip(text);
}

void TextDisabledEllipsis(std::string_view text, float maxWidth) {
  bool elided = false;
  const std::string shown = FitText(text, maxWidth, &elided);
  ImGui::TextDisabled("%s", shown.c_str());
  if (elided)
    FullTextTooltip(text);
}

void TextDisabledWrapped(std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%.*s", static_cast<int>(text.size()), text.data());
  ImGui::PopStyleColor();
}

bool SelectableEllipsis(const std::string& label, bool selected,
                        ImGuiSelectableFlags flags) {
  // Split off a "###id" so the identity survives the elision. Without this a
  // row that fits and the same row that does not would be two different
  // items, and every ref to it would break at some panel width.
  const size_t hash = label.find("###");
  const std::string_view visible(
      label.data(), hash == std::string::npos ? label.size() : hash);
  const std::string_view id = hash == std::string::npos
                                  ? std::string_view()
                                  : std::string_view(label).substr(hash);

  bool elided = false;
  // A Selectable spans the full content width and draws its label at the
  // left, so the room for text is what is left of the row.
  std::string shown = FitText(visible, 0.0f, &elided);
  shown += id;
  // An id-less label that elided to a bare "…" would collide with every
  // other such row; give it back its full text as the id.
  if (id.empty() && elided)
    shown += "###" + std::string(visible);
  const bool clicked = ImGui::Selectable(shown.c_str(), selected, flags);
  if (elided)
    FullTextTooltip(visible);
  return clicked;
}

} // namespace ui
