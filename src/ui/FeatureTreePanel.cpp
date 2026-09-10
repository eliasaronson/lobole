#include "ui/FeatureTreePanel.h"
#include "core/Units.h"
#include "ui/DerivedActions.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include "ui/TextFit.h"

#include "cad/CADMaterials.h"
#include "cad/SketchPlane.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h" // Measure::MassOfBodies (mass-props section)
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "ui/FeatureEditor.h"     // ArmSlot (blend edge re-pick)
#include "ui/ListSelection.h"     // ListRowClicked / DeleteSelection
#include "ui/SelectionPipeline.h" // SelectFeature / SelectedFeatureId
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h" // DrawSketchModelSections (U8a)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

// Mass properties for the active part, shown on demand (the OCC volume-props
// pass isn't per-frame cheap, so it's cached by document version). When the
// section is open the document's center of mass is stamped into AppState so
// the viewport draws a CoG marker. Lives at the bottom of the Model panel
// (U7e — was the Properties panel's last tenant).
void DrawMassPropertiesSection(AppState& state) {
  state.showCog = false;
  if (!state.document || state.document->GetBodies().empty())
    return;
  ImGui::Spacing();
  if (!ImGui::CollapsingHeader("Mass properties"))
    return;

  static Measure::DocumentMass cache;
  static const void* cachedDoc = nullptr;
  static uint64_t cachedVersion = UINT64_MAX;
  static bool cachedOk = false;
  const uint64_t version = state.document->Version();
  if (cachedDoc != static_cast<const void*>(state.document.get()) ||
      cachedVersion != version) {
    const auto m = Measure::MassOfBodies(state.document->GetBodies());
    cachedOk = m.has_value();
    if (cachedOk)
      cache = *m;
    cachedDoc = state.document.get();
    cachedVersion = version;
  }
  if (!cachedOk) {
    ImGui::TextDisabled("Mass properties unavailable.");
    return;
  }

  TextEllipsis("Volume: " +
               units::FormatVolume(cache.volume, state.ActiveUnits()));
  double area = 0.0;
  for (const auto& b : cache.bodies)
    area += b.surfaceArea;
  ImGui::Text("Surface area: %.3f mm\xc2\xb2", area);
  if (cache.massKnown)
    ImGui::Text("Mass: %.3f g", cache.massG);
  else
    ImGui::TextDisabled("Mass: unknown (assign a material)");
  ImGui::Text("Center of mass:");
  ImGui::Text("  (%.3f, %.3f, %.3f)", cache.centerOfMass.x,
              cache.centerOfMass.y, cache.centerOfMass.z);

  // Draw the CoG marker in the viewport while this section is open.
  state.showCog = true;
  state.cogWorld = glm::vec3(cache.centerOfMass);
}

// Per-kind glyph for the feature tree (U5) — same atlas as the toolbar.
const char* FeatureIcon(const std::string& type) {
  if (type == "Sketch")
    return ICON_FA_PENCIL;
  if (type == "Extrude")
    return ICON_FA_UP_LONG;
  if (type == "Revolve")
    return ICON_FA_ARROWS_SPIN;
  if (type == "Box")
    return ICON_FA_CUBE;
  if (type == "Sphere")
    return ICON_FA_CIRCLE;
  if (type == "Cylinder")
    return ICON_FA_DATABASE;
  if (type == "Boolean")
    return ICON_FA_OBJECT_GROUP;
  if (type == "Pattern")
    return ICON_FA_GRIP;
  if (type == "DatumPlane")
    return ICON_FA_TABLE_CELLS_LARGE;
  if (type == "DatumAxis")
    return ICON_FA_ARROWS_UP_DOWN;
  if (type == "Frame")
    return ICON_FA_LOCATION_CROSSHAIRS;
  if (type == "Curve3D")
    return ICON_FA_WAVE_SQUARE;
  if (type == "Pipe")
    return ICON_FA_ROAD;
  if (type == "Fillet")
    return ICON_FA_BORDER_TOP_LEFT;
  if (type == "Chamfer")
    return ICON_FA_CROP_SIMPLE;
  if (type == "Rib")
    return ICON_FA_BARS_STAGGERED;
  if (type == "Thread")
    return ICON_FA_CIRCLE_NOTCH;
  if (type == "Hole")
    return ICON_FA_CIRCLE_DOT;
  if (type == "Shell")
    return ICON_FA_BOX_OPEN;
  if (type == "Draft")
    return ICON_FA_ANGLES_RIGHT;
  if (type == "Split")
    return ICON_FA_SCISSORS;
  if (type == "Refine")
    return ICON_FA_BROOM;
  if (type == "Loft")
    return ICON_FA_BEZIER_CURVE;
  if (type == "Sweep")
    return ICON_FA_ROUTE;
  if (type == "Import")
    return ICON_FA_FILE_IMPORT;
  if (type == "Derived")
    return ICON_FA_CLONE;
  return ICON_FA_SHAPES;
}

// The rollback bar: a grabbable divider drawn where regeneration stops (or at
// the end of the list). Sets `dragging` while the user holds it.
void RollbackBar(bool& dragging) {
  const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
  ImGui::PushStyleColor(ImGuiCol_Button,
                        ImVec4(accent.x, accent.y, accent.z, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
  ImGui::Button("##RollbackBar", ImVec2(-FLT_MIN, 5.0f));
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImGui::SetTooltip("Rollback bar - drag to replay history up to a point");
  }
  if (ImGui::IsItemActive())
    dragging = true;
}

} // namespace

const char* FeatureRowBadge(const Feature& feature, bool rolledPast) {
  // Precedence matches the colour the row is already given, so the two
  // channels can never disagree: broken beats suppressed beats warned.
  // Suppressed and rolled-past are BOTH dimmed and were indistinguishable;
  // they get different glyphs, matching what their tooltips already said.
  if (feature.IsSuppressed())
    return ICON_FA_BAN;
  if (rolledPast)
    return ICON_FA_CLOCK_ROTATE_LEFT;
  if (!feature.GetError().empty())
    return ICON_FA_TRIANGLE_EXCLAMATION;
  if (!feature.GetWarning().empty())
    return ICON_FA_CIRCLE_EXCLAMATION; // a CIRCLE, not a smaller triangle
  return nullptr;
}

void DrawFeatureTreePanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);

  Document* doc = state.document.get();
  if (doc) {
    // Default geometry: the three base planes a sketch can be started on.
    // Named through SketchPlane so these rows always match the plane object
    // DefaultPlane(i) actually creates.
    ImGui::SeparatorText("Default geometry");
    // While a plane pick is armed (New Sketch, the Hole tool), these rows are
    // the pick: a single click on the NAMED plane is easier than hunting the
    // right quad in the viewport, and it used to do nothing at all.
    const bool armedForPlane = state.sketchEdit.pickingPlane;
    for (int i = 0; i < 3; i++) {
      if (ImGui::Selectable(
              SketchPlane::StandardPlaneName(static_cast<PlaneType>(i)), false,
              ImGuiSelectableFlags_AllowDoubleClick)) {
        const int n = static_cast<int>(doc->FeatureCount()) + 1;
        if (armedForPlane) {
          const SketchPurpose purpose = state.sketchEdit.purpose;
          state.sketchEdit.pickingPlane = false;
          BeginSketchOnPlane(state, DefaultPlane(i),
                             "Sketch " + std::to_string(n), purpose);
        } else if (ImGui::IsMouseDoubleClicked(0)) {
          BeginSketchOnPlane(state, DefaultPlane(i),
                             "Sketch " + std::to_string(n));
        }
      }
    }
    if (armedForPlane)
      ImGui::TextDisabled("  ^ or click a plane / flat face in the viewport");

    ImGui::SeparatorText("Features");
    // Transient refusal shown when a drag-reorder would dangle a reference
    // (fades after ~5 s). Written by the deferred-move handler below.
    static std::string moveError;
    static double moveErrorAt = -100.0;
    if (!moveError.empty()) {
      if (ImGui::GetTime() - moveErrorAt < 5.0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.45f, 0.42f, 1.0f));
        ImGui::TextWrapped("%s", moveError.c_str());
        ImGui::PopStyleColor();
      } else {
        moveError.clear();
      }
    }
    const int rollback = doc->Rollback();
    // Where regeneration ACTUALLY stops — the user's bar, or the transient
    // one a sketch edit put in front of it. Rows past it really are skipped,
    // so they dim; the grabbable bar itself stays on `rollback`, since the
    // view marker is not something the user can drag.
    const int skipFrom = doc->EffectiveRollback();
    const size_t count = doc->FeatureCount();
    // Drag-reorder request captured this frame, applied after the list is
    // drawn (mutating the feature vector mid-loop invalidates iteration).
    int moveFrom = -1, moveTo = -1;
    // Inline rename (context menu > Rename): the row becomes a text field.
    static int renaming = -1;
    static char renameBuf[128];
    static bool renameFocus = false;
    // Rollback-bar drag: gap y positions collected this frame resolve the
    // drop target after the list is drawn.
    static bool draggingBar = false;
    std::vector<float> gapY; // screen y of the gap above row i; [count] = end
    // The rows as listed — what a Shift-click range spans (ListSelection.h).
    std::vector<uint64_t> featureOrder;
    featureOrder.reserve(count);
    for (size_t i = 0; i < count; i++)
      featureOrder.push_back(doc->GetFeature(i)->GetId());

    for (size_t i = 0; i < count; i++) {
      gapY.push_back(ImGui::GetCursorScreenPos().y);
      if (rollback >= 0 && i == static_cast<size_t>(rollback))
        RollbackBar(draggingBar);
      Feature* feature = doc->GetFeature(i);

      if (renaming == static_cast<int>(i)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (renameFocus) {
          ImGui::SetKeyboardFocusHere();
          renameFocus = false;
        }
        const bool entered =
            ImGui::InputText("##rename", renameBuf, sizeof(renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
          if (renameBuf[0] != '\0' && feature->GetName() != renameBuf) {
            nlohmann::json before = DocumentIO::FeatureToJson(*feature);
            feature->SetName(renameBuf);
            doc->Regenerate();
            nlohmann::json after = DocumentIO::FeatureToJson(*feature);
            state.commands->PushApplied(
                std::make_unique<ModifyFeatureCommand>(i, before, after));
          }
          renaming = -1;
        } else if (ImGui::IsItemDeactivated()) {
          renaming = -1; // escape / focus lost without an edit: cancel
        }
        continue;
      }

      // The rollback bar: features at/after the marker are skipped by
      // regeneration and render dimmed below the divider.
      const bool rolledPast =
          skipFrom >= 0 && i >= static_cast<size_t>(skipFrom);
      // Suffix keeps IDs unique and test paths stable even if names repeat
      const std::string label = feature->GetName() + "##" + std::to_string(i);
      // Datum planes double-click into a new sketch on them; sketches
      // double-click into EDIT mode (the Properties Edit Sketch button);
      // every other feature double-clicks into its editor — the Properties
      // panel's parameter widgets.
      auto* datum = dynamic_cast<DatumPlaneFeature*>(feature);
      auto* sketchFeat = dynamic_cast<SketchFeature*>(feature);
      const ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
      // Features that failed to regenerate show red, with the reason on
      // hover; suppressed and rolled-past features show dimmed.
      const bool suppressed = feature->IsSuppressed();
      const bool dimmed = suppressed || rolledPast;
      const bool errored = !dimmed && !feature->GetError().empty();
      const char* badge = FeatureRowBadge(*feature, rolledPast);
      // A warning (built, but a likely-unintended no-op) tints amber — only
      // when the feature is neither broken nor dimmed, so error/dim win.
      const bool warned = !dimmed && !errored && !feature->GetWarning().empty();
      // A Derived copy whose source has moved wears the warning tint (it
      // built fine, but it is behind); a fresh one the linked-copy blue, so
      // "this is another part's geometry" reads at a glance.
      auto* derivedFeat = dynamic_cast<DerivedFeature*>(feature);
      const bool derivedStale =
          derivedFeat != nullptr && !dimmed && !errored &&
          DerivedStale(state, *derivedFeat) == std::optional<bool>{true};
      if (errored)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.35f, 0.35f, 1.0f));
      else if (warned || derivedStale)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
      else if (derivedFeat != nullptr && !dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.75f, 0.95f, 1.0f));
      else if (dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      // Per-kind glyph, drawn apart from the Selectable so item ids (and the
      // test refs built on them) stay exactly "<Name>##<index>".
      ImGui::TextUnformatted(FeatureIcon(feature->TypeName()));
      ImGui::SameLine(ImGui::GetFontSize() * 1.8f);
      // A cut leaves no body to click, so with a pattern's Source slot armed
      // the CUT'S OWN ROW is the pick — that is how a hole gets patterned in
      // the GUI at all (R14). Any other feature row keeps its normal click.
      auto* cutExt = dynamic_cast<ExtrudeFeature*>(feature);
      const bool armedForCutSource =
          state.slotPick.Active() &&
          state.slotPick.slot == static_cast<int>(PickSlot::PatternSource) &&
          cutExt != nullptr && cutExt->mode == ExtrudeMode::Cut;
      // A plain click replaces the WHOLE selection (a feature row drops
      // picked faces, as it always has); Ctrl toggles, Shift ranges.
      if (ImGui::Selectable(
              label.c_str(),
              IsSelected(state, SelectionItem::Kind::Feature, feature->GetId()),
              flags)) {
        ListRowClicked(state, SelectionItem::Kind::Feature, feature->GetId(),
                       featureOrder, /*ownKindOnly=*/false);
        if (armedForCutSource) {
          // A Body item IS {createdBy, ""} — a feature id — so the cut's id
          // travels the same slot path a body pick does.
          SlotPickClicked(state, {SelectionItem::Kind::Body, feature->GetId()},
                          /*meshId=*/0, /*index=*/0);
        } else if (datum && state.sketchEdit.pickingPlane) {
          // A datum row IS the pick while one is armed, exactly as the base
          // planes are (U10) — a named plane in the tree beats hunting its
          // quad in the viewport.
          const SketchPurpose purpose = state.sketchEdit.purpose;
          state.sketchEdit.pickingPlane = false;
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnDatum(state, *datum, "Sketch " + std::to_string(n),
                             purpose);
        } else if (datum && ImGui::IsMouseDoubleClicked(0)) {
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnDatum(state, *datum, "Sketch " + std::to_string(n));
        } else if (sketchFeat && ImGui::IsMouseDoubleClicked(0)) {
          state.sketchEdit = SketchEditState{};
          state.sketchEdit.activeSketchId = sketchFeat->GetId();
          state.measure = MeasureState{}; // sketch mode replaces measure
          if (state.camera)
            state.camera->AlignToFace(sketchFeat->plane.GetOrigin(),
                                      sketchFeat->plane.GetNormal());
        } else if (ImGui::IsMouseDoubleClicked(0)) {
          // U7e: editing a committed feature is explicit — the Tool window.
          OpenFeatureEditor(state, feature->GetId());
        }
      }
      // Drag a row onto another to reorder (validated on drop). The row being
      // renamed `continue`s above, so it is never a drag source. Placed right
      // after the Selectable so it, not the tooltip below, is the drag item.
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        int src = static_cast<int>(i);
        ImGui::SetDragDropPayload("REORDER_FEATURE", &src, sizeof(int));
        ImGui::Text("%s %s", FeatureIcon(feature->TypeName()),
                    feature->GetName().c_str());
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl =
                ImGui::AcceptDragDropPayload("REORDER_FEATURE")) {
          moveFrom = *static_cast<const int*>(pl->Data);
          moveTo = static_cast<int>(i);
        }
        ImGui::EndDragDropTarget();
      }
      // The state badge, right-aligned INSIDE the row rather than appended to
      // the label: the dock ships at 210 px and names already clip there, so
      // a trailing glyph in the layout would push the name further out of
      // view — trading one legibility problem for another. Drawn to the
      // window list at the row's own rect, so it costs the label nothing and
      // cannot change the item id the tests address.
      if (badge != nullptr) {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const float w = ImGui::CalcTextSize(badge).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(hi.x - w - 4.0f, lo.y),
                                            ImGui::GetColorU32(ImGuiCol_Text),
                                            badge);
      }
      if (errored) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", feature->GetError().c_str());
      } else if (warned) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", feature->GetWarning().c_str());
      } else if (derivedStale) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Stale: '%s' has changed since this copy was "
                            "taken. Right-click > Update from source.",
                            derivedFeat->sourceName.c_str());
      } else if (derivedFeat != nullptr && !dimmed) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s copy of '%s'",
                            derivedFeat->reference ? "Reference (ghost)"
                                                   : "Solid",
                            derivedFeat->sourceName.c_str());
      } else if (dimmed) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              suppressed ? "Suppressed"
              : doc->ViewRollback() >= 0 && rollback < 0
                  ? "Built from the sketch being edited \xe2\x80\x94 hidden "
                    "until you finish"
                  : "After the rollback bar");
      }
      if (ImGui::BeginPopupContextItem()) {
        // Inside a multi-selection the right-click keeps it (Delete below
        // acts on all of it); outside, the row becomes the selection.
        ListRowContext(state, SelectionItem::Kind::Feature, feature->GetId(),
                       /*ownKindOnly=*/false);
        // U7e: the explicit path to the committed-feature editor session
        // (double-click is the shortcut).
        if (ImGui::MenuItem("Edit…")) {
          OpenFeatureEditor(state, feature->GetId());
        }
        if (ImGui::MenuItem("Rename")) {
          renaming = static_cast<int>(i);
          snprintf(renameBuf, sizeof(renameBuf), "%s",
                   feature->GetName().c_str());
          renameFocus = true;
        }
        // Suppress/unsuppress: skipped by regeneration, params kept. One
        // undoable edit (suppressed round-trips through the feature JSON).
        if (ImGui::MenuItem(suppressed ? "Unsuppress" : "Suppress")) {
          nlohmann::json before = DocumentIO::FeatureToJson(*feature);
          feature->SetSuppressed(!suppressed);
          doc->Regenerate();
          nlohmann::json after = DocumentIO::FeatureToJson(*feature);
          state.commands->PushApplied(
              std::make_unique<ModifyFeatureCommand>(i, before, after));
        }
        // A Derived copy's verbs (docs/modeling/derived-plan.md): retake the
        // snapshot (one undo step), or freeze it as an unlinked body.
        // Both replace the feature object (a ModifyFeatureCommand), so
        // `feature` is dangling afterwards: leave the row like Delete does.
        if (derivedFeat != nullptr && derivedFeat->sourcePart != 0) {
          bool replaced = false;
          if (ImGui::MenuItem("Update from source###feat_derived_update")) {
            if (const std::string why = UpdateDerived(state, feature->GetId());
                !why.empty())
              state.sketchEdit.actionMsg = why;
            replaced = true;
          }
          if (ImGui::MenuItem("Break link###feat_derived_break")) {
            if (const std::string why =
                    BreakDerivedLink(state, feature->GetId());
                !why.empty())
              state.sketchEdit.actionMsg = why;
            replaced = true;
          }
          if (replaced) {
            ImGui::EndPopup();
            break; // feature list changed; redraw next frame
          }
        }
        // Rollback bar (not undoable — a view of the history, not an edit).
        if (ImGui::MenuItem("Rollback to here")) {
          doc->SetRollback(static_cast<int>(i));
        }
        if (rollback >= 0 && ImGui::MenuItem("Roll to end")) {
          doc->SetRollback(-1);
        }
        // A face-attached sketch can be re-pointed at another face (e.g. after
        // its face was consumed): arms face-picking for this sketch's faceRef.
        auto* sk = dynamic_cast<SketchFeature*>(feature);
        if (sk != nullptr && sk->faceRef.IsSet()) {
          if (ImGui::MenuItem("Reattach face…")) {
            state.sketchEdit = SketchEditState{};
            state.sketchEdit.pickingPlane = true;
            state.sketchEdit.reattachSketchId = sk->GetId();
          }
        }
        // A fillet/chamfer's edges can be re-picked (e.g. after a ref went
        // dangling): arms the edge slot for this feature.
        auto* fil = dynamic_cast<FilletFeature*>(feature);
        if (fil != nullptr) {
          if (ImGui::MenuItem("Pick edges…")) {
            ArmSlot(state, *fil, PickSlot::BlendEdges);
          }
        }
        // The selection, not just this row: "Delete 3 features" when three
        // are picked, as one undo step (RemoveFeaturesCommand).
        const size_t picked =
            SelectedIds(state, SelectionItem::Kind::Feature).size();
        if (ImGui::MenuItem(
                (DeleteLabel(picked, "feature") + "###feat_delete").c_str())) {
          DeleteSelection(state);
          ImGui::EndPopup();
          break; // feature list changed; redraw next frame
        }
        ImGui::EndPopup();
      }
    }
    gapY.push_back(ImGui::GetCursorScreenPos().y);
    if (count > 0 && rollback < 0)
      RollbackBar(draggingBar); // parked at the end: full history replays

    // Resolve a rollback-bar drag: preview the nearest gap while held, move
    // the marker on release. SetRollback regenerates, so only on release.
    if (draggingBar) {
      const float my = ImGui::GetIO().MousePos.y;
      size_t best = 0;
      float bestD = FLT_MAX;
      for (size_t k = 0; k < gapY.size(); k++) {
        const float d = std::abs(my - gapY[k]);
        if (d < bestD) {
          bestD = d;
          best = k;
        }
      }
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const ImVec2 wp = ImGui::GetWindowPos();
        const float x0 = wp.x + ImGui::GetStyle().WindowPadding.x;
        const float x1 =
            wp.x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, gapY[best]),
                                            ImVec2(x1, gapY[best]),
                                            ImGui::GetColorU32(accent), 2.0f);
      } else {
        draggingBar = false;
        doc->SetRollback(best == count ? -1 : static_cast<int>(best));
      }
    }
    if (count == 0) {
      TextDisabledWrapped("No features yet - start from the toolbar "
                          "above (or Ctrl+K)");
    }

    ImGui::SeparatorText("Bodies");
    // A committing menu action regenerates the document, which REBUILDS the
    // bodies vector — so every row draws from values copied up front, the
    // menu resolves the creator fresh by id, and the loop breaks after any
    // commit (the earlier `body.shape.Volume()` after the popup was a
    // use-after-free segfault).
    const size_t bodyCount = doc->GetBodies().size();
    for (size_t bi = 0; bi < bodyCount && bi < doc->GetBodies().size(); bi++) {
      const std::string bodyName = doc->GetBodies()[bi].name;
      const double bodyVolume = doc->GetBodies()[bi].shape.Volume();
      const uint64_t createdBy = doc->GetBodies()[bi].createdBy;
      ImGui::PushID(static_cast<int>(bi));
      const bool hidden = state.hiddenBodies.count(createdBy) != 0;
      // The glyph doubles as the visibility toggle (the eye every CAD tree
      // has); the context menu's Hide/Show stays as the discoverable path.
      ImGui::TextUnformatted(hidden ? ICON_FA_EYE_SLASH : ICON_FA_CUBES);
      if (ImGui::IsItemClicked()) {
        if (hidden)
          state.hiddenBodies.erase(createdBy);
        else
          state.hiddenBodies.insert(createdBy);
        state.syncedDocumentVersion = UINT64_MAX; // force renderer resync
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(hidden ? "Show body" : "Hide body");
      ImGui::SameLine(ImGui::GetFontSize() * 1.8f);
      if (hidden)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      // With a Body slot armed (Shell, Boolean, Pattern, Split, Refine…),
      // this named row IS a pick — the same rule the plane rows follow.
      // Clicking a body here used to do nothing at all, so the viewport was
      // the only way to fill a slot.
      const bool armedForBody =
          state.slotPick.Active() &&
          (state.slotPick.kinds &
           SelectionKindBit(SelectionItem::Kind::Body)) != 0;
      if (ImGui::Selectable((bodyName + "##body").c_str(), false,
                            ImGuiSelectableFlags_AllowOverlap) &&
          armedForBody) {
        SlotPickClicked(state, {SelectionItem::Kind::Body, createdBy, {}},
                        /*meshId=*/0, /*index=*/0);
      }
      if (hidden)
        ImGui::PopStyleColor();
      ImGui::OpenPopupOnItemClick("##bodyctx",
                                  ImGuiPopupFlags_MouseButtonRight);
      ImGui::SameLine();
      TextDisabledEllipsis(
          units::FormatVolume(bodyVolume, state.ActiveUnits()));

      bool committed = false;
      if (ImGui::BeginPopup("##bodyctx")) {
        if (ImGui::MenuItem(hidden ? "Show" : "Hide")) {
          if (hidden)
            state.hiddenBodies.erase(createdBy);
          else
            state.hiddenBodies.insert(createdBy);
          state.syncedDocumentVersion = UINT64_MAX; // force renderer resync
        }
        if (ImGui::BeginMenu("Material")) {
          using CADMaterials::MaterialLibrary;
          size_t creatorIndex = 0;
          Feature* creator = doc->FindFeatureById(createdBy, &creatorIndex);
          const auto setMaterial = [&](const std::string& slug) {
            if (creator == nullptr)
              return;
            nlohmann::json before = DocumentIO::FeatureToJson(*creator);
            creator->SetMaterial(slug);
            doc->Regenerate();
            nlohmann::json after = DocumentIO::FeatureToJson(*creator);
            state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
                creatorIndex, before, after));
            committed = true; // the bodies vector was rebuilt: stop the loop
          };
          const std::string current =
              creator != nullptr ? creator->GetMaterial() : std::string{};
          if (ImGui::MenuItem("Default", nullptr, current.empty()))
            setMaterial({});
          for (auto type : MaterialLibrary::AllTypes()) {
            const auto& mat = MaterialLibrary::GetMaterial(type);
            if (ImGui::MenuItem(mat.name.c_str(), nullptr,
                                current == MaterialLibrary::Slug(type)))
              setMaterial(MaterialLibrary::Slug(type));
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Appearance")) {
          // V-appearance (v54): a visual override on top of the material.
          // Widgets show DERIVED values (the material's own color/roughness/
          // metallic where nothing is overridden — the U10 rule); edits go
          // live per tick and land as ONE undo step when the widget
          // releases.
          size_t creatorIndex = 0;
          Feature* creator = doc->FindFeatureById(createdBy, &creatorIndex);
          if (creator != nullptr) {
            static nlohmann::json s_before;
            static uint64_t s_editing = 0;
            const Appearance ap = creator->GetAppearance();
            const auto& mat =
                CADMaterials::MaterialLibrary::GetMaterialOrDefault(
                    creator->GetMaterial());
            glm::vec3 color = ap.color.value_or(mat.baseColor);
            float rough = ap.roughness.value_or(mat.roughness);
            float metal = ap.metallic.value_or(mat.metallic);
            const auto apply = [&](const Appearance& next) {
              if (s_editing != createdBy) {
                s_before = DocumentIO::FeatureToJson(*creator);
                s_editing = createdBy;
              }
              creator->SetAppearance(next);
              doc->Regenerate(); // cache hit: restamp + resync only
              state.syncedDocumentVersion = UINT64_MAX;
              committed = true; // bodies vector rebuilt: restart the loop
            };
            const auto endEdit = [&] {
              if (s_editing != createdBy)
                return;
              nlohmann::json after = DocumentIO::FeatureToJson(*creator);
              if (after != s_before)
                state.commands->PushApplied(
                    std::make_unique<ModifyFeatureCommand>(creatorIndex,
                                                           s_before, after));
              s_editing = 0;
            };
            if (ImGui::ColorEdit3("Color", &color.x,
                                  ImGuiColorEditFlags_NoInputs)) {
              Appearance next = ap;
              next.color = color;
              apply(next);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
              endEdit();
            if (ImGui::SliderFloat("Roughness", &rough, 0.0f, 1.0f)) {
              Appearance next = ap;
              next.roughness = rough;
              apply(next);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
              endEdit();
            if (ImGui::SliderFloat("Metallic", &metal, 0.0f, 1.0f)) {
              Appearance next = ap;
              next.metallic = metal;
              apply(next);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
              endEdit();
            ImGui::BeginDisabled(!ap.Any());
            if (ImGui::MenuItem("Reset to material")) {
              apply(Appearance{});
              endEdit();
            }
            ImGui::EndDisabled();
          }
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
      if (committed)
        break; // redraw next frame against the regenerated body list
    }

    // Apply a deferred drag-reorder now that the frame's drawing is done. The
    // command regenerates and, on a dependency violation, reverts itself and
    // reports the casualty; the new order shows next frame. Effect is already
    // applied by Execute(), so push (not re-Execute) it onto the undo stack.
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
      auto cmd = std::make_unique<MoveFeatureCommand>(
          static_cast<size_t>(moveFrom), static_cast<size_t>(moveTo));
      if (cmd->Execute(*doc)) {
        moveError.clear();
        state.commands->PushApplied(std::move(cmd));
      } else {
        moveError = cmd->BrokenFeature().empty()
                        ? std::string("Can't reorder the feature there.")
                        : "Can't move here \xe2\x80\x94 it would break '" +
                              cmd->BrokenFeature() + "'.";
        moveErrorAt = ImGui::GetTime();
      }
    }
  }

  // Sketch-mode sections (U8a): images/constraints/patterns/linked copies
  // live here while a sketch is being edited (the Sketch panel is gone).
  DrawSketchModelSections(state);

  DrawMassPropertiesSection(state);

  ImGui::End();
}

} // namespace ui
