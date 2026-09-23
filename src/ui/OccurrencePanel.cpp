#include "ui/OccurrencePanel.h"
#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/Measure.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/DocumentSlots.h"
#include "ui/ExprInput.h"     // expression-capable numeric fields
#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (mate slot rows)
#include "ui/FileDialogs.h"
#include "ui/FloatingWindow.h"
#include "ui/ListSelection.h"
#include "ui/MotionActions.h"
#include "ui/PanelLayout.h" // LeftLabel / WrapRow (readable at dock width)
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h" // BeginToolWindowFrame (mate session window)
#include "ui/SelectionPipeline.h"
#include "ui/TextFit.h"
#include "ui/Theme.h" // ElevatedWindowBorder
#include "ui/ToleranceActions.h"
#include "ui/Widgets.h"

#include "core/io/Serialization.h" // RelationKindToString

#include "imgui.h"

#include <glm/gtc/quaternion.hpp> // eulerAngles, quat(vec3)
#include <glm/trigonometric.hpp>  // degrees, radians

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace ui {

namespace {
std::string PartNameById(const AppState& state, uint64_t partId) {
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.id == partId)
      return s.name;
  return "(missing part)";
}

// A5: an occurrence references a part OR an assembly, so the panel resolves
// both — and says which, since "Gangway" placed as a unit behaves differently
// from a part (it is one row, one selection, one explode step).
std::string RefNameById(const AppState& state, const Occurrence& o) {
  const DocKind want =
      o.refKind == OccurrenceRef::Assembly ? DocKind::Assembly : DocKind::Part;
  for (const PartSlot& s : state.parts)
    if (s.kind == want && s.id == o.partId)
      return s.name;
  return o.refKind == OccurrenceRef::Assembly ? "(missing assembly)"
                                              : "(missing part)";
}

// The tab index of an occurrence's referenced document, for "Open".
std::optional<size_t> RefSlotIndex(const AppState& state, const Occurrence& o) {
  const DocKind want =
      o.refKind == OccurrenceRef::Assembly ? DocKind::Assembly : DocKind::Part;
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == want && state.parts[i].id == o.partId)
      return i;
  return std::nullopt;
}

const char* MateTypeLabel(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return "fastened";
  case MateType::Revolute:
    return "revolute";
  case MateType::Slider:
    return "slider";
  case MateType::Cylindrical:
    return "cylindrical";
  case MateType::Planar:
    return "planar";
  case MateType::Ball:
    return "ball";
  }
  return "?";
}

// The mate list and its selected-mate editor. The create/edit session's
// controls (connector slot rows, flip, accept/cancel) live in the floating
// Tool window (DrawMateToolWindow below, U7c).
// Relations (docs/assemblies/relations-plan.md): listed with their ratio
// and phase editable and a remove button. Made by RPC in this round — a
// relation names two mates and the panel selects one — so there is no
// creation tool here for the defaults gate to hold.
void DrawRelationsSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  if (asmDoc.RelationCount() == 0)
    return;
  if (!SectionHeader("Relations###rel_section",
                     static_cast<int>(asmDoc.RelationCount()), true))
    return;
  uint64_t remove = 0;
  std::vector<uint64_t> relOrder;
  for (const Relation& r : asmDoc.GetRelations())
    relOrder.push_back(r.id);
  for (Relation& r : asmDoc.GetRelations()) {
    const std::string rid = std::to_string(r.id);
    const Mate* a = asmDoc.FindMateById(r.mateA);
    const Mate* b = asmDoc.FindMateById(r.mateB);
    const auto mateName = [](const Mate* m) {
      return m == nullptr      ? std::string("?")
             : m->name.empty() ? "mate " + std::to_string(m->id)
                               : m->name;
    };
    ListRowSpec row;
    row.glyph = r.error.empty() ? ICON_FA_GEARS : ICON_FA_TRIANGLE_EXCLAMATION;
    if (!r.error.empty())
      row.glyphColor = ImVec4(0.95f, 0.5f, 0.4f, 1.0f);
    row.label = (r.name.empty() ? mateName(a) + " – " + mateName(b) : r.name) +
                "###rel_row_" + rid;
    row.selected = IsSelected(state, SelectionItem::Kind::Relation, r.id);
    row.tail = DocumentIO::RelationKindToString(r.kind);
    char tip[256];
    std::snprintf(tip, sizeof(tip), "%s drives %s, ratio %.5g, phase %.5g%s%s",
                  mateName(a).c_str(), mateName(b).c_str(), r.ratio, r.phase,
                  r.error.empty() ? "" : "\n", r.error.c_str());
    row.tooltip = tip;
    row.menuId = "###rel_ctx_" + rid;
    row.menu = [&state, &remove, id = r.id]() {
      if (ImGui::MenuItem("Edit###rel_edit"))
        EditAssemblyRelation(state, id);
      ImGui::Separator();
      if (ImGui::MenuItem("Delete###rel_delete"))
        remove = id;
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked)
      ListRowClicked(state, SelectionItem::Kind::Relation, r.id, relOrder,
                     /*ownKindOnly=*/true);
    if (res.doubleClicked)
      EditAssemblyRelation(state, r.id);
  }
  if (remove != 0) {
    SnapshotAssembly(state);
    asmDoc.RemoveRelation(remove);
    if (state.asmEditRelation == remove)
      state.asmEditRelation = 0;
    SolveActiveAssembly(state);
  }
}

const char* MateGlyph(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return ICON_FA_LOCK;
  case MateType::Revolute:
    return ICON_FA_ROTATE;
  case MateType::Slider:
    return ICON_FA_ARROWS_LEFT_RIGHT;
  case MateType::Cylindrical:
    return ICON_FA_CIRCLE_NOTCH;
  case MateType::Planar:
    return ICON_FA_LAYER_GROUP;
  case MateType::Ball:
    return ICON_FA_CIRCLE;
  }
  return ICON_FA_LINK;
}

void DrawMatesSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;

  if (state.mateTool.active && state.mateTool.previewMateId)
    return; // the Tool window owns the mid-preview UI; hide the transient mate

  if (!SectionHeader("Mates###mate_section",
                     static_cast<int>(asmDoc.MateCount()), true))
    return;
  if (asmDoc.MateCount() == 0) {
    TextDisabledWrapped("None. Use the mate tools in the toolbar.");
    return;
  }

  std::vector<uint64_t> mateOrder;
  for (const Mate& mate : asmDoc.GetMates())
    mateOrder.push_back(mate.id);
  bool wantDelete = false;
  for (const Mate& mate : asmDoc.GetMates()) {
    const std::string mid = std::to_string(mate.id);
    ListRowSpec row;
    const bool failed = !mate.error.empty();
    row.glyph = failed ? ICON_FA_TRIANGLE_EXCLAMATION : MateGlyph(mate.type);
    if (failed)
      row.glyphColor = ImVec4(0.95f, 0.5f, 0.4f, 1.0f);
    // WHICH two occurrences: a column of "Mate [revolute]" said nothing
    // until each one was clicked.
    const std::string joins =
        OccurrenceLabel(state, asmDoc, mate.a.occurrenceId) + " – " +
        OccurrenceLabel(state, asmDoc, mate.b.occurrenceId);
    row.label = (mate.name.empty() ? joins : mate.name) + "###mate_row_" + mid;
    row.selected = IsSelected(state, SelectionItem::Kind::Mate, mate.id);
    row.tail = MateTypeLabel(mate.type);
    row.tooltip = joins + " (" + MateTypeLabel(mate.type) + ")" +
                  (failed ? "\n" + mate.error : std::string()) +
                  "\nDouble-click to edit";
    row.menuId = "###mate_ctx_" + mid;
    row.menu = [&state, &wantDelete, id = mate.id]() {
      if (ImGui::MenuItem("Edit###mate_edit"))
        StartMateEdit(state, id);
      ImGui::Separator();
      ListRowContext(state, SelectionItem::Kind::Mate, id,
                     /*ownKindOnly=*/true);
      const std::string del =
          DeleteLabel(SelectedIds(state, SelectionItem::Kind::Mate).size(),
                      "mate") +
          "###mate_delete";
      if (ImGui::MenuItem(del.c_str()))
        wantDelete = true;
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked)
      ListRowClicked(state, SelectionItem::Kind::Mate, mate.id, mateOrder,
                     /*ownKindOnly=*/true, /*clickClears=*/true);
    if (res.doubleClicked)
      StartMateEdit(state, mate.id);
  }
  if (wantDelete)
    DeleteSelection(state);
}

// A mate's degrees of freedom, by type -- what the panel's "Selected mate"
// section held, drawn by the mate tool's Edit Mate window now
// (docs/ui/assembly-panel-tidy-plan.md). Each field is one undo step per drag.
void DrawMateDofFields(AppState& state, Mate* sel) {
  auto angleDof = [&](const char* label) {
    float deg = glm::degrees(sel->rotation);
    const bool changed =
        ExprDragFloat(state, label, &deg, 1.0f, -FLT_MAX, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->rotation = glm::radians(deg);
      SolveActiveAssembly(state);
    }
  };
  auto offsetDof = [&](const char* label) {
    const bool changed = ExprDragFloat(state, label, &sel->offset, 0.1f,
                                       -FLT_MAX, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed)
      SolveActiveAssembly(state);
  };
  switch (sel->type) {
  case MateType::Revolute:
    angleDof("Angle (deg)");
    break;
  case MateType::Slider:
    offsetDof("Offset");
    break;
  case MateType::Cylindrical:
    angleDof("Angle (deg)");
    offsetDof("Offset");
    break;
  case MateType::Planar: {
    float dxy[2] = {sel->dx, sel->dy};
    const bool changed = ExprDragFloat2(state, "Slide (x,y)", dxy, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->dx = dxy[0];
      sel->dy = dxy[1];
      SolveActiveAssembly(state);
    }
    angleDof("Spin (deg)");
    break;
  }
  case MateType::Ball: {
    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->ballRotation));
    const bool changed =
        ExprDragFloat3(state, "Rotation (deg)", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->ballRotation = glm::quat(glm::radians(euler));
      SolveActiveAssembly(state);
    }
    break;
  }
  case MateType::Fastened:
    // No DOF, but the twist about the connector axis is still AUTHORED here:
    // Flip answers "which side", this answers "which way up". Without it the
    // tool window's Rotate could set an angle nothing could read back.
    angleDof("Angle (deg)");
    break;
  }
}

// What the motion row shows while a run is in flight: how far in, how long
// it has been, and a way out. `samples` is the study's, which is what the
// worker's fraction is a fraction OF.
//
// This used not to cover the whole wait: StartMotionRun copies every document
// under the assembly before the worker exists, no frame is drawn while it
// does, and on a real assembly that was seconds. It is ~11 ms since those
// copies stopped re-deriving geometry they were handed
// (docs/performance/motion-prep-copy.md), so the bar now starts about when
// the button is pressed.
void DrawMotionProgress(BackgroundJob& job, int samples) {
  const double f = job.fraction.load();
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - job.started)
                             .count();
  // The same widget the import modal draws (ui/Widgets.h), which is where
  // the "indeterminate rather than a bar at 0%" rule this used to restate
  // now lives.
  char overlay[64];
  JobProgress bar;
  if (f >= 0.0 && samples > 0) {
    const int done =
        std::clamp(static_cast<int>(f * samples + 0.5), 0, samples);
    std::snprintf(overlay, sizeof(overlay), "sample %d / %d   %.0fs", done,
                  samples, elapsed);
    bar.fraction = f;
  } else {
    std::snprintf(overlay, sizeof(overlay), "starting   %.0fs", elapsed);
  }
  bar.overlay = overlay;
  // The widget's own Cancel, as every job's (ui/Widgets.h): a request the
  // worker reads between samples, disabled once asked, since a second
  // request means nothing.
  const bool asked = job.cancel.load();
  bar.cancelAsked = asked;
  bar.cancelId = "###motion_cancel";
  bar.cancelTip = "Stops after the current sample and keeps the ones already "
                  "checked: the result comes back marked incomplete.";
  if (DrawJobProgress(bar))
    job.cancel.store(true);
  if (asked)
    TextDisabledWrapped("finishing the current sample");
}
} // namespace

// The name of the occurrence an id refers to (its label, else its part name).
// At ui:: scope, not in the anonymous namespace above: the Configurations
// panel's assembly arm lists occurrences and has to name them the same way.
std::string OccurrenceLabel(const AppState& state,
                            const AssemblyDocument& asmDoc, uint64_t occId) {
  const Occurrence* o = asmDoc.FindById(occId);
  if (!o)
    return "(missing)";
  return o->name.empty() ? PartNameById(state, o->partId) : o->name;
}

namespace {

// Motion studies (G2): the list, an Add that picks the first mate with a
// degree of freedom, and for the active study a driver combo, a scrub
// slider, Run (a background job) and the last result's verdict. The range
// and sample count are edited over the RPC for now; a scrub is a look, not
// an edit, and pushes no undo step (the explode slider's rule).
void DrawMotionBody(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  for (MotionStudy& s : asmDoc.GetMotionStudies()) {
    const bool active = state.activeMotionStudy == s.id;
    const std::string label = (s.name.empty() ? "(unnamed)" : s.name) +
                              "###motion_study_" + std::to_string(s.id);
    if (ImGui::Selectable(label.c_str(), active)) {
      state.activeMotionStudy = s.id;
      state.motionT = 0.0f;
    }
  }
  if (ImGui::SmallButton("Add study###motion_add")) {
    // The first mate that turns or slides. A rotation gets a full turn; a
    // slide gets a range derived from the placed part's own size, because a
    // "10 mm" default is right for a drawer and wrong for a dock.
    const Mate* driver = nullptr;
    for (const Mate& m : asmDoc.GetMates())
      if (assembly::MateHasDof(m.type, MateDof::Rotation) ||
          assembly::MateHasDof(m.type, MateDof::Offset)) {
        driver = &m;
        break;
      }
    if (driver == nullptr) {
      state.motionAddRefusal =
          "no mate here turns or slides — a motion study drives a revolute, "
          "slider or cylindrical mate";
    } else {
      state.motionAddRefusal.clear();
      MotionStudy s;
      s.name = "Motion " + std::to_string(asmDoc.GetMotionStudies().size() + 1);
      s.driverMate = driver->id;
      if (assembly::MateHasDof(driver->type, MateDof::Rotation)) {
        s.driverDof = MateDof::Rotation;
        s.from = 0.0;
        s.to = 2.0 * 3.14159265358979323846;
      } else {
        s.driverDof = MateDof::Offset;
        s.from = driver->offset;
        double reach = 10.0;
        if (const Occurrence* o = asmDoc.FindById(driver->b.occurrenceId))
          if (const Document* pd = PartDocById(state, o->partId)) {
            glm::vec3 lo, hi;
            if (Measure::WorldBounds(pd->GetBodies(), glm::mat4(1.0f), lo, hi))
              reach = glm::length(hi - lo);
          }
        s.to = driver->offset + reach;
      }
      SnapshotAssembly(state);
      state.activeMotionStudy = asmDoc.AddMotionStudy(std::move(s));
      state.motionT = 0.0f;
    }
  }
  if (!state.motionAddRefusal.empty())
    TextDisabledWrapped(state.motionAddRefusal.c_str());

  MotionStudy* sel = asmDoc.FindMotionStudyById(state.activeMotionStudy);
  if (sel == nullptr)
    return;
  // Driver: every mate with a degree of freedom, by name.
  LeftLabel("Driver");
  const Mate* cur = asmDoc.FindMateById(sel->driverMate);
  const std::string curLabel =
      cur ? (cur->name.empty() ? "mate " + std::to_string(cur->id) : cur->name)
          : "(missing)";
  if (ImGui::BeginCombo("###motion_driver", curLabel.c_str())) {
    for (const Mate& m : asmDoc.GetMates()) {
      const bool rot = assembly::MateHasDof(m.type, MateDof::Rotation);
      const bool slide = assembly::MateHasDof(m.type, MateDof::Offset);
      if (!rot && !slide)
        continue;
      const std::string item =
          (m.name.empty() ? "mate " + std::to_string(m.id) : m.name) +
          "###motion_driver_" + std::to_string(m.id);
      if (ImGui::Selectable(item.c_str(), m.id == sel->driverMate)) {
        SnapshotAssembly(state);
        sel->driverMate = m.id;
        sel->driverDof = rot ? MateDof::Rotation : MateDof::Offset;
        state.motionResults.erase(sel->id);
        asmDoc.BumpVersion();
      }
    }
    ImGui::EndCombo();
  }
  const bool rot = sel->driverDof == MateDof::Rotation;
  const double k = rot ? angles::kDegPerRad : 1.0;
  // %g, not a fixed decimal: a watch's escape pinion turns one tooth on
  // 0.0104 degrees of its barrel, and "0.0 to 0.0 deg" reads as a study
  // that does nothing.
  ImGui::Text("%.4g to %.4g %s in %d samples", sel->from * k, sel->to * k,
              rot ? "deg" : "mm", sel->samples);
  const std::string why = motion::WhyNot(asmDoc, *sel);
  if (!why.empty())
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", why.c_str());

  // The scrub: live, no snapshot.
  LeftLabel("Pose");
  if (ImGui::SliderFloat("###motion_t", &state.motionT, 0.0f, 1.0f)) {
    // The scrub poses the DOCUMENT; playback would paint over it.
    StopMotionPlayback(state);
    std::string err;
    ApplyMotionPose(state, sel->id, state.motionT, &err);
  }

  // A run is off the frame (ui/MotionActions.cpp) and reports a fraction per
  // sample, so while one is going the BUTTON ROW becomes that bar and a
  // Cancel. It used to be three greyed-out buttons and nothing else for the
  // length of the run — a responsive app that looks hung, which is exactly
  // what the STEP-import modal exists to avoid.
  //
  // The button row and nothing more. The verdict below stays up while a run
  // is in flight, because StartMotionRun deliberately does NOT erase the
  // previous result — and the first version of this drew the bar with an
  // early `return`, which threw away the very thing that was being kept. The
  // stale line is what says which run you are reading.
  if (BackgroundJob* job = FindBackgroundJob(state, "motion")) {
    DrawMotionProgress(*job, sel->samples);
  } else {
    WrapRow row;
    for (bool preview : {true, false}) {
      const char* label = preview ? "Preview motion###motion_preview"
                                  : "Validate motion###motion_run";
      row.Next(ButtonWidth(label));
      if (ImGui::SmallButton(label)) {
        // A RUN, not an edit: the study keeps the clash settings a script
        // gave it, and no undo step is pushed for pressing a button.
        RunOverride run;
        run.checkClash = !preview;
        run.clashMode = "exact";
        std::string err;
        if (!StartMotionRun(state, sel->id, &err, &run))
          state.motionAddRefusal = err;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(preview
                              ? "Solve sampled poses without collision checks."
                              : "Exact collisions at every study sample. "
                                "Standing assembly checks are separate.");
    }
    row.Next(ButtonWidth("Delete"));
    if (ImGui::SmallButton("Delete###motion_delete")) {
      SnapshotAssembly(state);
      asmDoc.RemoveMotionStudy(sel->id);
      state.motionResults.erase(sel->id);
      state.activeMotionStudy = 0;
      asmDoc.BumpVersion();
      return;
    }
  }
  if (const motion::Result* r = MotionResultFor(state, sel->id)) {
    const bool stale = MotionResultStale(state, sel->id);
    // One verdict, from core (motion::Summarize): whether it MOVED through
    // every sample first, collisions second — a preview that locks at 90
    // degrees must not read like one that works.
    const motion::Verdict v = motion::Summarize(*r, *sel, stale);
    const ImVec4 colour = v.level == motion::Verdict::Level::Good
                              ? ImVec4(0.4f, 0.85f, 0.4f, 1.0f)
                          : v.level == motion::Verdict::Level::Bad
                              ? ImVec4(0.9f, 0.4f, 0.4f, 1.0f)
                              : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", v.text.c_str());
    ImGui::PopStyleColor();
    if (r->checkedClashes && !stale)
      ImGui::TextDisabled("Reused %zu pair results; executed %zu",
                          r->pairsCached, r->pairsChecked - r->pairsCached);

    // What a run is FOR once it has run: watch it, and take it away. Both
    // refuse the same way the export always has — no result, a stale one,
    // or a sample with no pose.
    const std::string no = MotionPlaybackRefusal(state, sel->id);
    const bool playingThis = state.motionPlaybackStudy == sel->id;
    WrapRow transport;
    // One button whose label changes: one ###id, so the test engine and the
    // widget-id lint see one widget.
    const std::string playLabel =
        std::string(playingThis && state.motionPlaying ? "Pause" : "Play") +
        "###motion_play";
    transport.Next(ButtonWidth(playLabel.c_str()));
    ImGui::BeginDisabled(!no.empty());
    if (ImGui::SmallButton(playLabel.c_str())) {
      if (playingThis && state.motionPlaying) {
        state.motionPlaying = false;
      } else {
        std::string why;
        if (!StartMotionPlayback(state, sel->id, &why))
          state.motionAddRefusal = why;
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", no.empty() ? "Play the poses this run made, "
                                           "in the viewport only: nothing "
                                           "is solved or changed."
                                         : no.c_str());
    if (playingThis) {
      transport.Next(ButtonWidth("Stop"));
      if (ImGui::SmallButton("Stop###motion_stop"))
        StopMotionPlayback(state);
    }
    const bool canSave = FileDialogsAvailable();
    const char* exportLabel = "Export animation...###motion_export";
    transport.Next(ButtonWidth(exportLabel));
    ImGui::BeginDisabled(!no.empty() || !canSave);
    if (ImGui::SmallButton(exportLabel))
      ExportMotionDialog(state, sel->id);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", !no.empty() ? no.c_str()
                              : !canSave
                                  ? "No file dialog here; export_motion "
                                    "{study, path} writes the same file."
                                  : "An animated GLB of this run, 24 samples "
                                    "a second.");
    if (playingThis) {
      const size_t n = r->samples.size();
      const size_t at =
          std::min(n - 1, static_cast<size_t>(state.motionPlaybackPos + 0.5));
      const bool rotDof = sel->driverDof == MateDof::Rotation;
      ImGui::TextDisabled("%s sample %zu of %zu (%.4g %s)",
                          state.motionPlaying ? "Playing" : "Paused at", at,
                          n - 1, r->samples[at].value * (rotDof ? k : 1.0),
                          rotDof ? "deg" : "mm");
    }
  } else {
    ImGui::TextWrapped("Not run yet: Preview solves every sample, Validate "
                       "also checks collisions.");
  }
}

// Exploded views (A4): persistent named views; the ACTIVE one + factor are
// transient display state. Activating (or sliding the factor) never touches
// the document — the offset composes into rendered transforms only.
void DrawExplodeBody(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;

  // "None" + one selectable per view; the active one drives the display.
  if (ImGui::Selectable("None##explode", state.activeExplodedView == 0)) {
    state.activeExplodedView = 0;
    state.syncedDocumentVersion = UINT64_MAX; // display change: force resync
  }
  uint64_t deleteId = 0;
  for (ExplodedView& v : asmDoc.GetExplodedViews()) {
    // The id goes in the LABEL, not a PushID(int) scope: an integer scope is
    // not addressable as a path segment, so no test could ever select a view
    // (docs/ui/gui-coverage.md).
    const bool active = state.activeExplodedView == v.id;
    const std::string label = (v.name.empty() ? "(unnamed)" : v.name) +
                              "###explode_view_" + std::to_string(v.id);
    if (ImGui::Selectable(label.c_str(), active)) {
      state.activeExplodedView = v.id;
      state.syncedDocumentVersion = UINT64_MAX;
    }
  }
  if (ImGui::SmallButton("Add view###explode_add")) {
    SnapshotAssembly(state);
    ExplodedView v;
    v.name = "Exploded " + std::to_string(asmDoc.GetExplodedViews().size() + 1);
    state.activeExplodedView = asmDoc.AddExplodedView(std::move(v));
    state.syncedDocumentVersion = UINT64_MAX;
  }

  if (ExplodedView* sel =
          asmDoc.FindExplodedViewById(state.activeExplodedView)) {
    char name[128];
    snprintf(name, sizeof name, "%s", sel->name.c_str());
    LeftLabel("Name");
    if (ImGui::InputText("###explode_name", name, sizeof name,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }
    // Factor slider: cheap live path (matrix updates only); picking catches
    // up on the resync forced when the slider is released.
    LeftLabel("Factor");
    if (ImGui::SliderFloat("###explode_factor", &state.explodeFactor, 0.0f,
                           1.0f))
      ApplyExplodeTransforms(state);
    if (ImGui::IsItemDeactivatedAfterEdit())
      state.syncedDocumentVersion = UINT64_MAX;
    // SameLine does not wrap, so "Delete view" was drawn past the panel edge
    // at the width the dock ships at.
    WrapRow row;
    row.Next(ButtonWidth("Auto-explode"));
    if (ImGui::SmallButton(
            "Auto-explode###explode_auto")) { // radial seed, then drag
      if (AutoExplode(state, sel->id))
        state.syncedDocumentVersion = UINT64_MAX;
    }
    row.Next(ButtonWidth("Delete view"));
    if (ImGui::SmallButton("Delete view###explode_delete")) {
      SnapshotAssembly(state);
      asmDoc.RemoveExplodedView(sel->id);
      state.activeExplodedView = 0;
      state.syncedDocumentVersion = UINT64_MAX;
      asmDoc.BumpVersion();
    }
    TextDisabledWrapped("Drag the gizmo to set an occurrence's offset.");
  }
}

} // namespace

// Exploded views and Motion are the assembly's two VIEWPORT MODES: each poses
// the occurrences and is driven with a slider or a drag gizmo while you watch
// the model. They used to be two more sections in the Model dock, scrolled
// off under the occurrence tree that they were moving
// (docs/ui/u15-assembly-panel.md). The one-at-a-time tool slot is the right
// home twice over — it puts the controls over the thing they move, and it
// makes the two mutually exclusive, which they have to be: both place the
// same occurrences.
void DrawExplodeToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Exploded view");
  DrawExplodeBody(state);
  ImGui::Separator();
  // Close, not Esc: the window is a way in to a display state that outlives
  // it, and it applies as you go (docs/ui/consistency-round-plan.md).
  if (ImGui::Button("Close###explode_close"))
    state.explodeToolOpen = false;
  ImGui::End();
}

void DrawMotionToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Motion");
  DrawMotionBody(state);
  ImGui::Separator();
  if (ImGui::Button("Close###motion_close"))
    state.motionToolOpen = false;
  ImGui::End();
}

// Bill of materials (A4): derived rows per referenced part. The mass math
// (OCC volume properties per body) is not per-frame cheap, so the table is
// cached and recomputed only when the assembly or a referenced part changed.
// A mass a 40 px column can hold. The BOM printed raw grams — a 385 kg dock
// totalled "385169.06" — and an ImGui table cell CLIPS rather than elides, so
// what a person read was "385" under a header saying "Total g". A truncated
// number still looks like a number, which is the worst way for a bill of
// materials to be wrong: every unit mass on that screen was out by a factor
// of ten to a thousand while looking perfectly reasonable.
//
// Now core/Units: the g/kg switch this used to make per value is exactly
// what a document unit system replaces. A column of masses has to be
// comparable DOWN THE COLUMN, and "1.2 kg" over "900 g" is not.
std::string MassText(const AppState& state, double grams) {
  return units::FormatMass(grams, state.ActiveUnits());
}

// Full text on hover, for whatever the column still cannot hold. Kept
// alongside TextFit's own tooltip because a TABLE cell clips on its own,
// before any of our text lands — the column width is the table's business,
// not the string's.
void CellTooltip(const std::string& text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", text.c_str());
}

void DrawBomWindow(AppState& state) {
  if (!state.bomWindowOpen)
    return;
  if (!state.assembly) {
    state.bomWindowOpen = false;
    return;
  }

  static BomTable cache;
  static const void* cachedAsm = nullptr;
  static uint64_t cachedKey = 0;
  // While an assembly tab is active every part doc is stashed in its slot, so
  // this folds all referenced-part versions plus the assembly's own.
  uint64_t key = state.assembly->Version();
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.doc)
      key = key * 1315423911ull + s.doc->Version();
  if (cachedAsm != static_cast<const void*>(state.assembly.get()) ||
      cachedKey != key) {
    cache = ComputeBom(state);
    cachedAsm = state.assembly.get();
    cachedKey = key;
  }

  // A floating window, because a bill of materials is a TABLE and wants
  // width (docs/ui/assembly-panel-tidy-plan.md). In the dock it was two
  // columns with material and mass stacked under each name; the five-column
  // form only appeared when the dock was wide, which it never was. The total
  // is in the title, so it reads without scrolling to the bottom.
  const std::string title = "Bill of materials" +
                            (cache.massKnown && !cache.rows.empty()
                                 ? " — " + MassText(state, cache.totalMassG)
                                 : std::string()) +
                            "###BomWindow";
  // Under the tool window when a mate or an occurrence is being edited there,
  // instead of on top of it (ui/FloatingWindow.h).
  static FloatingPlacement placement;
  ImGui::SetNextWindowSize(
      ImVec2(ImGui::GetFontSize() * 36.0f, ImGui::GetFontSize() * 28.0f),
      ImGuiCond_Appearing);
  bool open = true;
  const bool visible = BeginFloatingWindow(
      state, title, &open, placement, FloatAnchor::BelowToolWindow, 0, 0.97f);
  if (!open)
    state.bomWindowOpen = false;
  if (!visible) {
    ImGui::End();
    return;
  }
  if (cache.rows.empty()) {
    ImGui::TextDisabled("No occurrences.");
    ImGui::End();
    return;
  }
  const bool wide = true; // the window is sized for the full table

  // Sorted by the header the person clicked; listed order until they do.
  std::vector<const BomRow*> rows;
  for (const BomRow& r : cache.rows)
    rows.push_back(&r);
  if (ImGui::BeginTable(
          "###bom_table", wide ? 5 : 2,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable |
              ImGuiTableFlags_ScrollY,
          ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 3.0f))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    // Narrow: the second column carries a MASS on the stacked row, so it
    // needs room for "385.17 kg", not just for "19".
    ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch,
                            wide ? 1.6f : 1.3f);
    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch,
                            wide ? 0.4f : 1.0f);
    if (wide) {
      ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch,
                              1.2f);
      ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 0.9f);
      ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch,
                              0.9f);
      ImGui::TableHeadersRow();
    }
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs();
        sort != nullptr && sort->SpecsCount > 0) {
      const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
      const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
      const auto less = [&](const BomRow* x, const BomRow* y) {
        switch (spec.ColumnIndex) {
        case 1:
          return x->quantity < y->quantity;
        case 2:
          return x->material < y->material;
        case 3:
          return x->unitMassG < y->unitMassG;
        case 4:
          return x->unitMassG * x->quantity < y->unitMassG * y->quantity;
        default:
          return x->partName < y->partName;
        }
      };
      std::stable_sort(rows.begin(), rows.end(),
                       [&](const BomRow* x, const BomRow* y) {
                         return asc ? less(x, y) : less(y, x);
                       });
    }
    for (const BomRow* rp : rows) {
      const BomRow& r = *rp;
      const std::string material = r.material.empty() ? "-" : r.material;
      const std::string unit = r.massKnown ? MassText(state, r.unitMassG) : "?";
      const std::string total =
          r.massKnown ? MassText(state, r.unitMassG * r.quantity) : "?";
      // Everything about the row, for the hover — the escape hatch for
      // whatever the column still cannot hold.
      const std::string full = r.partName + "  x" + std::to_string(r.quantity) +
                               "  " + material + "  " + unit + " each, " +
                               total + " total";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.partName.c_str());
      CellTooltip(full);
      ImGui::TableNextColumn();
      ImGui::Text("%d", r.quantity);
      CellTooltip(full);
      if (wide) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(material.c_str());
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextUnformatted(unit.c_str());
        else
          ImGui::TextDisabled("?"); // no material/density assigned
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextUnformatted(total.c_str());
        else
          ImGui::TextDisabled("?");
        CellTooltip(full);
      } else {
        // Second row: what the dropped columns were carrying.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", material.c_str());
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextDisabled("%s", total.c_str());
        else
          ImGui::TextDisabled("?");
        CellTooltip(full);
      }
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Total");
    for (int c = 1; c < (wide ? 4 : 1); ++c)
      ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (cache.massKnown) {
      ImGui::TextUnformatted(MassText(state, cache.totalMassG).c_str());
      CellTooltip(MassText(state, cache.totalMassG));
    } else {
      ImGui::TextDisabled("?");
    }
    ImGui::EndTable();
  }
  // V2: name the reason for every "?" so a null total explains itself.
  for (const std::string& w : cache.warnings)
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%s", w.c_str());
  const bool canSave = FileDialogsAvailable();
  ImGui::BeginDisabled(!canSave);
  if (ImGui::Button("Export CSV###bom_export"))
    ExportBomDialog(state);
  ImGui::EndDisabled();
  if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Needs a system file dialog (unavailable here) - use "
                      "export_bom over the script API");
  }

  // Every placed fastener, re-derived against the stack it is IN
  // (docs/parts/gui-plan.md, phase 4). Next to the BOM because that is where
  // a person looks at what the assembly is MADE of, and because the answer
  // has the same shape as the mass warnings above it.
  //
  // On demand rather than every frame: it walks the whole assembly along a
  // ray per fastener. And ON DEMAND rather than never — a length derived
  // from other documents' geometry cannot be a live binding (A2), so a stack
  // that moved under a screw has to be asked about. Until now it could only
  // be asked over the script API, which means in practice it was not.
  if (ImGui::Button("Check fasteners###bom_check_fasteners")) {
    state.fastenerChecks.clear();
    for (const FastenerCheck& c : CheckFasteners(state).findings)
      state.fastenerChecks.push_back(c.part + " " + c.why);
    state.fastenerChecked = true;
  }
  if (state.fastenerChecked) {
    if (state.fastenerChecks.empty()) {
      ImGui::TextDisabled("Every fastener still fits its stack.");
    } else {
      for (const std::string& w : state.fastenerChecks)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%s", w.c_str());
    }
  }

  // Fits, beside fasteners for the same reason fasteners are here: it is a
  // question about what the assembly is MADE of, and it is answered by
  // walking the whole thing, so it is asked for rather than run per frame.
  //
  // The assembly's own round mates ARE the pairing — nothing has to be set up
  // first (docs/tolerances/plan.md T2).
  if (ImGui::Button("Check fits###bom_check_fits")) {
    state.fitChecks.clear();
    int pass = 0, warn = 0, fail = 0, reported = 0, unverified = 0;
    for (const FitCheck& c : CheckFits(state)) {
      state.fitChecks.push_back(c.Sentence());
      if (c.status == "pass")
        pass++;
      else if (c.status == "warn")
        warn++;
      else if (c.status == "fail")
        fail++;
      else if (c.status == "reported")
        reported++;
      else
        unverified++;
    }
    // The summary counts each kind APART. An aggregate that hid unknowns
    // behind a green badge would be the one thing this check exists not to do.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%d passed, %d warned, %d failed, %d reported, %d unverified",
                  pass, warn, fail, reported, unverified);
    state.fitSummary = buf;
    state.fitsChecked = true;
  }
  ImGui::SetItemTooltip(
      "Check every round joint whose two faces carry a tolerance. The mate "
      "type is the intent: a joint that moves cannot be an interference fit.");
  if (state.fitsChecked) {
    if (state.fitChecks.empty()) {
      ImGui::TextDisabled("No round mate here carries a tolerance on both "
                          "sides yet.");
    } else {
      ImGui::TextDisabled("%s", state.fitSummary.c_str());
      for (const std::string& w : state.fitChecks)
        ImGui::TextWrapped("%s", w.c_str());
    }
  }
  ImGui::End();
}

// The occurrence list + properties editor, drawn in the "Model" panel (where a
// part tab shows its feature tree) when an assembly tab is active. Edits
// snapshot for undo; a drag is one undo step (snapshot on IsItemActivated).
void DrawOccurrencePanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);
  if (!state.assembly) {
    ImGui::End();
    return;
  }
  AssemblyDocument& asmDoc = *state.assembly;

  if (SectionHeader("Occurrences###occ_section",
                    static_cast<int>(asmDoc.OccurrenceCount()), true)) {
    if (asmDoc.OccurrenceCount() == 0)
      TextDisabledWrapped("Empty - use Insert part on the toolbar.");

    const std::vector<uint64_t> picked = SelectedOccurrenceIds(state);
    const uint64_t primary = picked.empty() ? 0 : picked.back();
    const uint64_t bringIntoView =
        primary != state.occurrenceScrolledTo ? primary : 0;
    state.occurrenceScrolledTo = primary;

    std::vector<uint64_t> occOrder;
    for (const Occurrence& o : asmDoc.GetOccurrences())
      occOrder.push_back(o.id);
    // What a row's menu asked for, applied after the loop: several of them
    // change the list (or the document) being walked.
    enum class After { None, Open, Detach, Delete };
    After after = After::None;
    uint64_t afterId = 0;
    size_t afterSlot = 0;
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      const std::string suffix = std::to_string(o.id);
      const bool isSub = o.refKind == OccurrenceRef::Assembly;
      ListRowSpec row;
      row.glyph = isSub ? ICON_FA_CUBES : ICON_FA_CUBE;
      row.label = (o.name.empty() ? RefNameById(state, o) : o.name) +
                  "###occ_" + suffix;
      row.selected = IsOccurrenceSelected(state, o.id);
      // Hidden rows grey out, as in other CAD programs, and the eye -- on
      // hover, or always when hidden -- replaces a column of checkboxes that
      // read as "selected" rather than "shown".
      row.dimmed = !o.visible;
      row.eye = o.visible ? ListRowSpec::Eye::Shown : ListRowSpec::Eye::Hidden;
      row.eyeId = "###occ_eye_" + suffix;
      if (o.fixed)
        row.tailGlyph = ICON_FA_ANCHOR; // was "(fixed)" appended to the name
      if (o.attachedTo != 0)
        row.tail = "attached";
      row.tooltip = std::string(isSub ? "Assembly: " : "Part: ") +
                    RefNameById(state, o) +
                    (o.fixed ? "\nFixed (grounded)" : "") +
                    (o.visible ? "" : "\nHidden") + "\nDouble-click to edit";
      row.menuId = "###occ_ctx_" + suffix;
      row.menu = [&, occId = o.id, isSub]() {
        if (ImGui::MenuItem("Edit###occ_edit"))
          EditAssemblyOccurrence(state, occId);
        ListRowContext(state, SelectionItem::Kind::Occurrence, occId,
                       /*ownKindOnly=*/true);
        const Occurrence* live = asmDoc.FindById(occId);
        const std::optional<size_t> slot =
            live != nullptr ? RefSlotIndex(state, *live) : std::nullopt;
        const std::string item = std::string("Open ") +
                                 (isSub ? "assembly" : "part") + "###occ_open";
        if (ImGui::MenuItem(item.c_str(), nullptr, false, slot.has_value())) {
          after = After::Open;
          afterSlot = *slot;
        }
        if (!slot && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", isSub ? "That assembly is not in this project"
                                        : "That part is not in this project");
        const size_t pickedVis = SelectedOccurrenceIds(state).size();
        const std::string visItem =
            RowActionLabel(SelectionWouldHide(state) ? "Hide" : "Show",
                           pickedVis, "occurrence") +
            "###occ_hide";
        if (ImGui::MenuItem(visItem.c_str()))
          ToggleSelectedOccurrencesVisible(state); // rows keep their places
        if (live != nullptr && live->attachedTo != 0 &&
            ImGui::MenuItem("Detach from its part###occ_detach")) {
          after = After::Detach;
          afterId = occId;
        }
        ImGui::Separator();
        const std::string del =
            DeleteLabel(SelectedOccurrenceIds(state).size(), "occurrence") +
            "###occ_delete";
        if (ImGui::MenuItem(del.c_str()))
          after = After::Delete;
      };
      const ListRowResult res = ListRow(row);
      if (res.clicked)
        ListRowClicked(state, SelectionItem::Kind::Occurrence, o.id, occOrder,
                       /*ownKindOnly=*/true);
      if (res.doubleClicked)
        EditAssemblyOccurrence(state, o.id);
      if (res.eyeClicked)
        SetOccurrencesVisible(state, {o.id}, !o.visible);
      if (o.id == bringIntoView && !ImGui::IsItemVisible())
        ImGui::SetScrollHereY(0.5f);
      if (after != After::None)
        break; // the list, or the document, is about to change
    }
    switch (after) {
    case After::Open:
      SetActivePart(state, afterSlot);
      ImGui::End();
      return; // the panel is now drawing a different document
    case After::Detach:
      SnapshotAssembly(state);
      if (Occurrence* d = asmDoc.FindById(afterId))
        d->attachedTo = 0;
      asmDoc.BumpVersion();
      break;
    case After::Delete:
      DeleteSelection(state);
      ImGui::End();
      return; // the list we were walking just changed under us
    case After::None:
      break;
    }
  }

  DrawMatesSection(state);
  DrawRelationsSection(state);

  ImGui::End();
}

// The mate session's Tool-window contents (U7c): two connector slot rows
// (same "###slotN" convention as the feature dialogs), then the preview's
// flip/error/accept/cancel — or a Done button for a re-pick session.
void DrawMateToolWindow(AppState& state) {
  auto& tool = state.mateTool;
  AssemblyDocument& asmDoc = *state.assembly;
  Mate* editM =
      tool.editMateId ? asmDoc.FindMateById(tool.editMateId) : nullptr;
  Mate* pm =
      tool.previewMateId ? asmDoc.FindMateById(tool.previewMateId) : nullptr;
  if (tool.editMateId != 0 && editM == nullptr) {
    CancelMateTool(state); // the edited mate vanished (undo/delete)
    return;
  }

  BeginToolWindowFrame(state, editM != nullptr
                                  ? std::string("Edit Mate")
                                  : std::string(MateTypeLabel(tool.type)));

  // A connector as a slot-row value ("Bracket-2", "+ snapped edge").
  const auto connLabel = [&](const Connector& c, bool have) -> std::string {
    if (!have || c.occurrenceId == 0)
      return "";
    std::string s = OccurrenceLabel(state, asmDoc, c.occurrenceId);
    if (c.anchor.frameId != 0) { // v86: the mate as designed, by name
      std::string fname = "frame " + std::to_string(c.anchor.frameId);
      if (const Occurrence* o = asmDoc.FindById(c.occurrenceId))
        if (const Document* part = PartDocById(state, o->partId))
          for (const auto& f : part->GetFeatures())
            if (f->GetId() == c.anchor.frameId)
              fname = f->GetName();
      s += " @ " + fname;
    } else if (c.anchor.snapEdge.IsSet())
      s += " (snapped)";
    return s;
  };
  Connector a{}, b{};
  bool haveA = false, haveB = false;
  if (editM != nullptr) {
    a = editM->a;
    b = editM->b;
    haveA = haveB = true;
  } else if (pm != nullptr) {
    a = pm->a;
    b = pm->b;
    haveA = haveB = true;
  } else {
    a = tool.first;
    haveA = tool.haveFirst;
  }

  // Slot rows: click to (re)arm; the armed row shows in the accent color.
  // Mid-preview re-picking is locked (Cancel first) — except in edit mode,
  // where re-picking IS the session.
  const bool rowsLive = editM != nullptr || pm == nullptr;
  const auto slotRow = [&](PickSlot slot, const char* label,
                           const std::string& value) {
    const bool armed = state.slotPick.featureId == kMateSessionSlot &&
                       state.slotPick.slot == static_cast<int>(slot);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (armed) {
      const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(accent.x, accent.y, accent.z, 0.35f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(accent.x, accent.y, accent.z, 0.55f));
    }
    const std::string text =
        (value.empty() ? std::string(armed ? "(pick in viewport)" : "(none)")
                       : value) +
        "###slot" + std::to_string(static_cast<int>(slot));
    if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetFontSize() * 14.0f, 0)) &&
        rowsLive) {
      if (armed)
        state.slotPick = {};
      else
        ArmMateConnector(state, slot == PickSlot::MateConnectorB);
    }
    if (armed)
      ImGui::PopStyleColor(2);
  };
  slotRow(PickSlot::MateConnectorA, "Connector A", connLabel(a, haveA));
  slotRow(PickSlot::MateConnectorB, "Connector B", connLabel(b, haveB));

  if (!tool.message.empty())
    ImGui::TextDisabled("%s", tool.message.c_str());

  Mate* live = editM != nullptr ? editM : pm;
  if (live != nullptr) {
    // An edit session writes an EXISTING mate, so each change is its own undo
    // step; a preview mate is popped whole by Cancel and snapshots nothing.
    const auto edited = [&]() {
      if (editM != nullptr)
        SnapshotAssembly(state);
      SolveActiveAssembly(state);
    };
    if (editM != nullptr) {
      // Editing: the mate's name, and below its degrees of freedom by type
      // -- what the panel's "Selected mate" section held.
      char name[128];
      std::snprintf(name, sizeof(name), "%s", editM->name.c_str());
      if (ImGui::InputText("Name###mate_name", name, sizeof(name),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        SnapshotAssembly(state);
        editM->name = name;
        asmDoc.BumpVersion();
      }
    }
    if (ImGui::Checkbox("Flip", &live->flipped))
      edited();
    // Flip answers "which side", Rotate answers "which way up" — two faces
    // can mate on the right planes and still land a quarter turn out, which
    // no amount of flipping fixes. Ball is the exception: its twist about Z
    // folds into ballRotation, so the control would be a second name for a
    // DOF it already has.
    if (live->type != MateType::Ball) {
      ImGui::SameLine();
      if (ImGui::Button("Rotate 90°###mate_rotate90")) {
        // Wrapped into [0, 360), so four clicks come back to where they
        // started rather than winding the number up.
        const float turned = glm::degrees(live->rotation) + 90.0f;
        live->rotation =
            glm::radians(turned - 360.0f * std::floor(turned / 360.0f));
        edited();
      }
      if (editM == nullptr) {
        float deg = glm::degrees(live->rotation);
        if (ExprDragFloat(state, "Angle (deg)###mateToolAngle", &deg, 1.0f,
                          -FLT_MAX, FLT_MAX, "%.3f")) {
          live->rotation = glm::radians(deg);
          edited();
        }
      }
    }
    if (editM != nullptr)
      DrawMateDofFields(state, editM);
    if (!live->error.empty())
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s",
                         live->error.c_str());
  }

  ImGui::Separator();
  bool accept = false, cancel = false;
  // The feature tool window's buttons (docs/ui/consistency-round-plan.md):
  // Accept and Cancel where a mate is being made, Close where one is being
  // edited, since those edits are applied as they are made.
  if (editM != nullptr) {
    if (ImGui::Button("Close###mate_close"))
      accept = true; // edits were applied live (each re-pick = one undo step)
  } else {
    ImGui::BeginDisabled(pm == nullptr);
    ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentColorHovered());
    if (ImGui::Button(ICON_FA_CHECK " Accept###tw_accept"))
      accept = true;
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_XMARK " Cancel###tw_cancel"))
      cancel = true;
  }
  // Enter accepts / Escape flows through ViewportInput's cascade (slot
  // disarm first, then session cancel).
  if (!ImGui::IsAnyItemActive() && pm != nullptr &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false))) {
    accept = true;
  }
  ImGui::End();

  if (accept)
    AcceptMateTool(state);
  else if (cancel)
    CancelMateTool(state);
}

void EditAssemblyOccurrence(AppState& state, uint64_t occId) {
  if (!state.assembly)
    return;
  // One tool window at a time: an open mate session or relation editor ends.
  if (state.mateTool.active)
    CancelMateTool(state);
  state.asmEditRelation = 0;
  state.asmEditOccurrence = occId;
}

void EditAssemblyRelation(AppState& state, uint64_t relId) {
  if (!state.assembly)
    return;
  if (state.mateTool.active)
    CancelMateTool(state);
  state.asmEditOccurrence = 0;
  state.asmEditRelation = relId;
}

void DrawAssemblyEditWindow(AppState& state) {
  if (!state.assembly) {
    state.asmEditOccurrence = state.asmEditRelation = 0;
    return;
  }
  AssemblyDocument& asmDoc = *state.assembly;

  if (state.asmEditOccurrence != 0) {
    Occurrence* sel = asmDoc.FindById(state.asmEditOccurrence);
    if (sel == nullptr) { // deleted, or undone away
      state.asmEditOccurrence = 0;
      return;
    }
    const bool isSub = sel->refKind == OccurrenceRef::Assembly;
    BeginToolWindowFrame(state,
                         isSub ? "Edit sub-assembly" : "Edit occurrence");
    ImGui::TextDisabled("%s: %s", isSub ? "Assembly" : "Part",
                        RefNameById(state, *sel).c_str());
    if (const auto slot = RefSlotIndex(state, *sel)) {
      ImGui::SameLine();
      if (ImGui::SmallButton(isSub ? "Open###open_ref"
                                   : "Edit part###open_ref")) {
        state.asmEditOccurrence = 0;
        ImGui::End();
        SetActivePart(state, *slot);
        return;
      }
    }
    char name[128];
    std::snprintf(name, sizeof(name), "%s", sel->name.c_str());
    if (ImGui::InputText("Name###occ_name", name, sizeof(name),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }
    glm::vec3 pos = sel->position;
    const bool posChanged =
        ExprDragFloat3(state, "Position###occ_pos", &pos.x, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state); // one snapshot per drag session
    if (posChanged) {
      sel->position = pos;
      asmDoc.BumpVersion();
    }
    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->orientation));
    const bool rotChanged =
        ExprDragFloat3(state, "Rotation (deg)###occ_rot", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (rotChanged) {
      sel->orientation = glm::quat(glm::radians(euler));
      asmDoc.BumpVersion();
    }
    bool fixed = sel->fixed;
    if (ImGui::Checkbox("Fixed (grounded)###occ_fixed", &fixed)) {
      SnapshotAssembly(state);
      sel->fixed = fixed;
      asmDoc.BumpVersion();
    }
    bool visible = sel->visible;
    if (ImGui::Checkbox("Visible###occ_visible", &visible))
      SetOccurrencesVisible(state, {sel->id}, visible);
    ImGui::Separator();
    bool remove = false;
    if (ImGui::Button("Edit in context###occ_in_context")) {
      // A4: the part tab, with the assembly ghosted around it.
      const uint64_t id = sel->id;
      state.asmEditOccurrence = 0;
      ImGui::End();
      EnterInContextEdit(state, id);
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete###occ_edit_delete"))
      remove = true;
    ImGui::SameLine();
    if (ImGui::Button("Close###occ_edit_done"))
      state.asmEditOccurrence = 0;
    const uint64_t id = sel->id;
    ImGui::End();
    if (remove) {
      // Through the Delete verb, so mates on it and the undo step behave
      // as they do from the list.
      state.selection.items = {
          SelectionItem{SelectionItem::Kind::Occurrence, id}};
      state.asmEditOccurrence = 0;
      DeleteSelection(state);
    }
    return;
  }

  if (state.asmEditRelation != 0) {
    Relation* r = nullptr;
    for (Relation& rel : asmDoc.GetRelations())
      if (rel.id == state.asmEditRelation)
        r = &rel;
    if (r == nullptr) {
      state.asmEditRelation = 0;
      return;
    }
    BeginToolWindowFrame(state, "Edit relation");
    const Mate* a = asmDoc.FindMateById(r->mateA);
    const Mate* b = asmDoc.FindMateById(r->mateB);
    const auto mateName = [](const Mate* m) {
      return m == nullptr      ? std::string("?")
             : m->name.empty() ? "mate " + std::to_string(m->id)
                               : m->name;
    };
    ImGui::TextDisabled("%s: %s -> %s",
                        DocumentIO::RelationKindToString(r->kind),
                        mateName(a).c_str(), mateName(b).c_str());
    if (r->kind == RelationKind::Freewheel)
      ImGui::TextDisabled("drives %s, lead %.4g deg",
                          DocumentIO::FreewheelDrivesToString(r->drives),
                          angles::ToDegrees(r->lead));
    double ratio = r->ratio, phase = r->phase;
    if (ExprInputDouble(state, "Ratio###rel_ratio", &ratio, "%.5g") &&
        ratio != 0.0 && ratio != r->ratio) {
      SnapshotAssembly(state);
      r->ratio = ratio;
      asmDoc.BumpVersion();
      SolveActiveAssembly(state);
    }
    if (ExprInputDouble(state, "Phase###rel_phase", &phase, "%.5g") &&
        phase != r->phase) {
      SnapshotAssembly(state);
      r->phase = phase;
      asmDoc.BumpVersion();
      SolveActiveAssembly(state);
    }
    if (!r->error.empty())
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s",
                         r->error.c_str());
    ImGui::Separator();
    bool remove = false;
    if (ImGui::Button("Delete###rel_edit_delete"))
      remove = true;
    ImGui::SameLine();
    if (ImGui::Button("Close###rel_edit_done"))
      state.asmEditRelation = 0;
    const uint64_t id = r->id;
    ImGui::End();
    if (remove) {
      SnapshotAssembly(state);
      asmDoc.RemoveRelation(id);
      state.asmEditRelation = 0;
      SolveActiveAssembly(state);
    }
  }
}

} // namespace ui
