#include "ui/FeatureTreePanel.h"

#include "CADMaterials.h"
#include "Camera.h"
#include "core/Command.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/Feature.h"
#include "core/features/FilletFeature.h"
#include "core/io/Serialization.h"
#include "core/features/SketchFeature.h"
#include "ui/AppActions.h" // SelectFeature / SelectedFeatureId
#include "ui/SketchInteraction.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

// Per-kind glyph for the feature tree (U5) — same atlas as the toolbar.
const char* FeatureIcon(const std::string& type) {
  if (type == "Sketch") return ICON_FA_PENCIL;
  if (type == "Extrude") return ICON_FA_UP_LONG;
  if (type == "Revolve") return ICON_FA_ARROWS_SPIN;
  if (type == "Box") return ICON_FA_CUBE;
  if (type == "Sphere") return ICON_FA_CIRCLE;
  if (type == "Cylinder") return ICON_FA_DATABASE;
  if (type == "Boolean") return ICON_FA_OBJECT_GROUP;
  if (type == "Pattern") return ICON_FA_GRIP;
  if (type == "DatumPlane") return ICON_FA_TABLE_CELLS_LARGE;
  if (type == "DatumAxis") return ICON_FA_ARROWS_UP_DOWN;
  if (type == "Curve3D") return ICON_FA_WAVE_SQUARE;
  if (type == "Pipe") return ICON_FA_ROAD;
  if (type == "Fillet") return ICON_FA_BORDER_TOP_LEFT;
  if (type == "Chamfer") return ICON_FA_CROP_SIMPLE;
  if (type == "Rib") return ICON_FA_BARS_STAGGERED;
  if (type == "Thread") return ICON_FA_CIRCLE_NOTCH;
  if (type == "Hole") return ICON_FA_CIRCLE_DOT;
  if (type == "Shell") return ICON_FA_BOX_OPEN;
  if (type == "Draft") return ICON_FA_ANGLES_RIGHT;
  if (type == "Loft") return ICON_FA_BEZIER_CURVE;
  if (type == "Sweep") return ICON_FA_ROUTE;
  if (type == "Import") return ICON_FA_FILE_IMPORT;
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

void DrawFeatureTreePanel(AppState& state) {
  ImGui::Begin("Model");

  Document* doc = state.document.get();
  if (doc) {
    // Default geometry: the three base planes a sketch can be started on.
    ImGui::SeparatorText("Default geometry");
    static const char* kPlaneNames[3] = {"Front Plane", "Top Plane",
                                          "Right Plane"};
    for (int i = 0; i < 3; i++) {
      if (ImGui::Selectable(kPlaneNames[i], false,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnPlane(state, DefaultPlane(i),
                             "Sketch " + std::to_string(n));
        }
      }
    }

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
          rollback >= 0 && i >= static_cast<size_t>(rollback);
      // Suffix keeps IDs unique and test paths stable even if names repeat
      const std::string label =
          feature->GetName() + "##" + std::to_string(i);
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
      // A warning (built, but a likely-unintended no-op) tints amber — only
      // when the feature is neither broken nor dimmed, so error/dim win.
      const bool warned =
          !dimmed && !errored && !feature->GetWarning().empty();
      if (errored)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.35f, 0.35f, 1.0f));
      else if (warned)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
      else if (dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      // Per-kind glyph, drawn apart from the Selectable so item ids (and the
      // test refs built on them) stay exactly "<Name>##<index>".
      ImGui::TextUnformatted(FeatureIcon(feature->TypeName()));
      ImGui::SameLine(ImGui::GetFontSize() * 1.8f);
      if (ImGui::Selectable(label.c_str(),
                            SelectedFeatureId(state) == feature->GetId(),
                            flags)) {
        SelectFeature(state, feature->GetId());
        if (datum && ImGui::IsMouseDoubleClicked(0)) {
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
          ImGui::SetWindowFocus("Properties");
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
      if (errored) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", feature->GetError().c_str());
      } else if (warned) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", feature->GetWarning().c_str());
      } else if (dimmed) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(suppressed ? "Suppressed" : "After the rollback bar");
      }
      if (ImGui::BeginPopupContextItem()) {
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
        // dangling): arms edge picking for this feature.
        auto* fil = dynamic_cast<FilletFeature*>(feature);
        if (fil != nullptr) {
          if (ImGui::MenuItem("Pick edges…")) {
            state.edgePick = {};
            state.edgePick.filletId = fil->GetId();
          }
        }
        if (ImGui::MenuItem("Delete")) {
          state.commands->Execute(std::make_unique<RemoveFeatureCommand>(i),
                                  *doc);
          // A dangling selected id is pruned next frame (PruneSelection).
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
        const float x1 = wp.x + ImGui::GetWindowWidth() -
                         ImGui::GetStyle().WindowPadding.x;
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(x0, gapY[best]), ImVec2(x1, gapY[best]),
            ImGui::GetColorU32(accent), 2.0f);
      } else {
        draggingBar = false;
        doc->SetRollback(best == count ? -1 : static_cast<int>(best));
      }
    }
    if (count == 0) {
      ImGui::TextDisabled("No features yet - use the Create menu");
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
      ImGui::Selectable((bodyName + "##body").c_str(), false,
                        ImGuiSelectableFlags_AllowOverlap);
      if (hidden)
        ImGui::PopStyleColor();
      ImGui::OpenPopupOnItemClick("##bodyctx",
                                  ImGuiPopupFlags_MouseButtonRight);
      ImGui::SameLine();
      ImGui::TextDisabled("%.2f mm%s", bodyVolume, "\xc2\xb3");

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

  ImGui::End();
}

} // namespace ui
