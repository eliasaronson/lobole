#include "ui/OccurrencePanel.h"

#include "cad/CADMaterials.h"
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
#include "ui/FlexibleActions.h"
#include "ui/FloatingWindow.h"
#include "ui/Icons.h"
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
#include <filesystem>
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
// Relations (docs/assemblies/relations.md): listed with their ratio
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
    // A side may be a mate inside a flexible placement (v129).
    const Mate* a = assembly::MateAtPath(asmDoc, r.mateAPath, r.mateA,
                                         AssemblyResolverFor(state));
    const Mate* b = assembly::MateAtPath(asmDoc, r.mateBPath, r.mateB,
                                         AssemblyResolverFor(state));
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

// The mate's glyph is the glyph of the toolbar button that makes that type,
// so the list and the bar agree (they had drifted apart on four of six).
const char* MateGlyph(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return CommandIcon("assembly.mate_fastened");
  case MateType::Revolute:
    return CommandIcon("assembly.mate_revolute");
  case MateType::Slider:
    return CommandIcon("assembly.mate_slider");
  case MateType::Cylindrical:
    return CommandIcon("assembly.mate_cylindrical");
  case MateType::Planar:
    return CommandIcon("assembly.mate_planar");
  case MateType::Ball:
    return CommandIcon("assembly.mate_ball");
  }
  return kUnknownIcon;
}

// A FLEXIBLE placement's inside, under its row
// (docs/assemblies/README.md#flexible-sub-assemblies): the occurrences that
// move and the mates that move them, read-only and addressed by path. A part
// row selects that part in the viewport -- where it drags within its mate --
// and a mate row selects the part the mate moves. Editing them happens on the
// sub-assembly's own tab; what is posed here is this placement's.
void DrawFlexibleChildren(AppState& state, const Occurrence& F) {
  const std::vector<const Occurrence*> moving =
      MovingInnerOccurrences(state, F);
  const std::vector<const Mate*> mates = InnerDofMates(state, F);
  if (moving.empty() && mates.empty())
    return;
  const std::string fid = std::to_string(F.id);
  ImGui::Indent();
  // No tree push: the rows below keep the flat "###occ_in_<unit>_<part>" ids
  // the rest of the panel uses.
  const std::string head = std::to_string(moving.size()) + " moving, " +
                           std::to_string(mates.size()) +
                           (mates.size() == 1 ? " mate" : " mates") +
                           "###occ_flex_" + fid;
  const bool open =
      ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                          ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                          ImGuiTreeNodeFlags_SpanAvailWidth);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("What moves inside '%s' in this assembly. Click a part "
                      "to select it; drag it in the viewport to turn or slide "
                      "it within its mate.",
                      F.name.c_str());
  if (open) {
    ImGui::Indent();
    for (const Occurrence* in : moving) {
      const std::string iid = std::to_string(in->id);
      ListRowSpec row;
      row.glyph =
          in->refKind == OccurrenceRef::Assembly ? ICON_FA_CUBES : ICON_FA_CUBE;
      row.label = (in->name.empty() ? "occurrence " + iid : in->name) +
                  "###occ_in_" + fid + "_" + iid;
      row.selected = IsInnerOccurrenceSelected(state, F.id, in->id);
      const Mate* by = InnerMateDriving(state, F, in->id);
      row.tail = by != nullptr ? MateTypeLabel(by->type) : "rides";
      row.tooltip =
          "Moves inside '" + F.name + "' (path [" + fid + "], " + iid + ")" +
          (by != nullptr ? "\nDrag it in the viewport to move it "
                           "within its " +
                               std::string(MateTypeLabel(by->type)) + " mate"
                         : "\nRides on another moving part");
      if (ListRow(row).clicked)
        SelectInnerOccurrence(state, F.id, in->id);
    }
    for (const Mate* m : mates) {
      const std::string mid = std::to_string(m->id);
      ListRowSpec row;
      row.glyph = MateGlyph(m->type);
      row.label = (m->name.empty() ? "mate " + mid : m->name) +
                  "###occ_inmate_" + fid + "_" + mid;
      const bool rot = assembly::MateHasDof(m->type, MateDof::Rotation);
      const std::optional<double> v =
          assembly::MateDofAt(*state.assembly, {F.id}, m->id,
                              rot ? MateDof::Rotation : MateDof::Offset,
                              AssemblyResolverFor(state));
      // What its configuration holds wins: that is where the part stands.
      std::optional<double> held;
      if (const AssemblyDocument* sub = FlexibleSub(state, F))
        if (const Configuration* c = sub->ConfigurationFor(F.configurationId))
          if (const MateOverride* h = c->OverrideForMate(m->id))
            if (rot ? h->rotation.has_value() : h->offset.has_value())
              held = rot ? *h->rotation : *h->offset;
      if (held || v) {
        const double show = held ? *held : *v;
        char tail[48];
        std::snprintf(tail, sizeof(tail), rot ? "%s%.1f deg" : "%s%.2f mm",
                      held ? "held " : "",
                      rot ? angles::ToDegrees(show) : show);
        row.tail = tail;
      }
      row.selected = IsInnerOccurrenceSelected(state, F.id, m->b.occurrenceId);
      row.tooltip = std::string(MateTypeLabel(m->type)) + " inside '" + F.name +
                    "', as this placement has it posed (path [" + fid +
                    "], mate " + mid +
                    ").\nA motion study or a relation can drive it.";
      if (ListRow(row).clicked && IsMovingInner(state, F, m->b.occurrenceId))
        SelectInnerOccurrence(state, F.id, m->b.occurrenceId);
    }
    ImGui::Unindent();
  }
  ImGui::Unindent();
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
// (docs/ui/panels.md#assembly-tab-the-model-panel). Each field is one undo step
// per drag.
void DrawMateDofFields(AppState& state, Mate* sel) {
  auto angleDof = [&](const char* label) {
    double deg = glm::degrees(sel->rotation);
    const bool changed =
        ExprDragDouble(state, label, &deg, 1.0f, -DBL_MAX, DBL_MAX, "%.3f");
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->rotation = glm::radians(deg);
      SolveActiveAssembly(state);
    }
  };
  auto offsetDof = [&](const char* label) {
    const bool changed = ExprDragDouble(state, label, &sel->offset, 0.1f,
                                        -DBL_MAX, DBL_MAX, "%.3f");
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
    double dxy[2] = {sel->dx, sel->dy};
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
    glm::dvec3 euler = glm::degrees(glm::eulerAngles(sel->ballRotation));
    const bool changed =
        ExprDragFloat3(state, "Rotation (deg)", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->ballRotation = glm::dquat(glm::radians(euler));
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
// (docs/assemblies/README.md#background-motion-copies-without-regenerating), so
// the bar now starts about when the button is pressed.
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
    // The first mate that turns or slides -- this assembly's own first, then
    // one inside a flexible placement (v129). A rotation gets a full turn; a
    // slide gets a range derived from the placed part's own size, because a
    // "10 mm" default is right for a drawer and wrong for a dock.
    const std::vector<DofMateChoice> choices = DofMateChoices(state);
    const Mate* driver =
        choices.empty() ? nullptr
                        : assembly::MateAtPath(asmDoc, choices.front().path,
                                               choices.front().mate,
                                               AssemblyResolverFor(state));
    if (driver == nullptr) {
      state.motionAddRefusal =
          "no mate here turns or slides — a motion study drives a revolute, "
          "slider or cylindrical mate";
    } else {
      state.motionAddRefusal.clear();
      const std::vector<uint64_t>& path = choices.front().path;
      MotionStudy s;
      s.name = "Motion " + std::to_string(asmDoc.GetMotionStudies().size() + 1);
      s.driverMate = driver->id;
      s.driverPath = path;
      if (assembly::MateHasDof(driver->type, MateDof::Rotation)) {
        s.driverDof = MateDof::Rotation;
        s.from = 0.0;
        s.to = 2.0 * 3.14159265358979323846;
      } else {
        s.driverDof = MateDof::Offset;
        const double from =
            assembly::MateDofAt(asmDoc, path, driver->id, MateDof::Offset,
                                AssemblyResolverFor(state))
                .value_or(driver->offset);
        s.from = from;
        double reach = 10.0;
        // The placed part lives in the sub-assembly for an inner mate.
        const AssemblyDocument* owner =
            path.empty()
                ? &asmDoc
                : AssemblyDocById(state, asmDoc.FindById(path.front())->partId);
        if (const Occurrence* o =
                owner ? owner->FindById(driver->b.occurrenceId) : nullptr)
          if (const Document* pd = OccurrencePartDoc(state, *o)) {
            glm::dvec3 lo, hi;
            if (Measure::WorldBounds(pd->GetBodies(), glm::dmat4(1.0), lo, hi))
              reach = glm::length(hi - lo);
          }
        s.to = from + reach;
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
  // Driver: every mate with a degree of freedom, by name -- this assembly's
  // own, then those inside each flexible placement, by path ("Pump /
  // camshaft"), which is how driver_path names them (v129).
  LeftLabel("Driver");
  const std::string curLabel =
      MateLabelAt(state, sel->driverPath, sel->driverMate);
  if (ImGui::BeginCombo("###motion_driver", curLabel.c_str())) {
    for (const DofMateChoice& c : DofMateChoices(state)) {
      std::string id = std::to_string(c.mate);
      if (!c.path.empty())
        id = std::to_string(c.path.front()) + "_" + id;
      const std::string item = c.label + "###motion_driver_" + id;
      if (ImGui::Selectable(item.c_str(), sel->driverPath == c.path &&
                                              c.mate == sel->driverMate)) {
        SnapshotAssembly(state);
        sel->driverMate = c.mate;
        sel->driverPath = c.path;
        sel->driverDof = c.rotation ? MateDof::Rotation : MateDof::Offset;
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
  const std::string why =
      motion::WhyNot(asmDoc, *sel, AssemblyResolverFor(state));
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
    const char* exportLabel = "Export GLB...###motion_export";
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
                                  : "An animated GLB of this run (for a 3D "
                                    "viewer), 24 samples "
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

  // A video of the study (docs/assemblies/README.md#motion-video). It needs
  // no run — the frames are posed, not replayed — so it sits outside the
  // result's block, under the GLB export. While one is being made the button
  // becomes its bar and Cancel, as the run's buttons do.
  if (BackgroundJob* job = FindBackgroundJob(state, "motion-video")) {
    JobProgress bar;
    bar.status = job->label;
    bar.fraction = job->fraction.load();
    bar.cancelAsked = job->cancel.load();
    bar.cancelId = "###motion_video_cancel";
    bar.cancelTip = "Stops after the frame being captured; no file is left "
                    "behind.";
    if (DrawJobProgress(bar))
      job->cancel.store(true);
  } else {
    const std::string whyNot =
        motion::WhyNot(asmDoc, *sel, AssemblyResolverFor(state));
    const bool canSave =
        FileDialogsAvailable() || state.saveDialogAnswer.has_value();
    const char* videoLabel = "Export animation...###motion_video";
    ImGui::BeginDisabled(!whyNot.empty() || !canSave);
    if (ImGui::SmallButton(videoLabel))
      ExportMotionVideoDialog(state, sel->id);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "%s", !whyNot.empty() ? whyNot.c_str()
                : !canSave
                    ? "No file dialog here; export_motion_video {study, "
                      "path} writes the same file."
                    : "A video of the study from this view: MP4, WebM or "
                      "GIF (ffmpeg), or PNG frames. The file type picks.");
    if (const auto& last = state.motionVideoLast) {
      if (last->ok)
        TextDisabledWrapped(
            ("Wrote " +
             std::filesystem::path(last->plan.path).filename().string() + " (" +
             std::to_string(last->framesWritten) + " frames, " +
             std::to_string(last->plan.width) + "x" +
             std::to_string(last->plan.height) + ")")
                .c_str());
      else
        TextDisabledWrapped(("Video not written: " + last->error).c_str());
    }
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
    // (docs/ui/testing.md#making-a-widget-addressable).
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
// (docs/ui/panels.md#assembly-tab-the-model-panel). The one-at-a-time tool slot
// is the right home twice over — it puts the controls over the thing they move,
// and it makes the two mutually exclusive, which they have to be: both place
// the same occurrences.
void DrawExplodeToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Exploded view");
  DrawExplodeBody(state);
  ImGui::Separator();
  // Close, not Esc: the window is a way in to a display state that outlives
  // it, and it applies as you go (docs/ui/panels.md).
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
  // width (docs/ui/panels.md#assembly-tab-the-model-panel). In the dock it was
  // two columns with material and mass stacked under each name; the five-column
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
      // "piece (pawn)": one part in two configurations is two lines
      const std::string label = r.configuration.empty()
                                    ? r.partName
                                    : r.partName + " (" + r.configuration + ")";
      const std::string full = label + "  x" + std::to_string(r.quantity) +
                               "  " + material + "  " + unit + " each, " +
                               total + " total";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(label.c_str());
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
  // (docs/parts/README.md#checking-placed-fasteners). Next to the BOM because
  // that is where a person looks at what the assembly is MADE of, and because
  // the answer has the same shape as the mass warnings above it.
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
  // first (docs/tolerances/README.md#checks).
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
      else if (o.flexible)
        row.tail = "flexible";
      row.tooltip = std::string(isSub ? "Assembly: " : "Part: ") +
                    RefNameById(state, o) +
                    (o.flexible ? "\nFlexible: its mechanism moves here" : "") +
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
      if (o.flexible)
        DrawFlexibleChildren(state, o);
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
        if (const Document* part = OccurrencePartDoc(state, *o))
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
        const double turned = glm::degrees(live->rotation) + 90.0;
        live->rotation =
            glm::radians(turned - 360.0 * std::floor(turned / 360.0));
        edited();
      }
      if (editM == nullptr) {
        double deg = glm::degrees(live->rotation);
        if (ExprDragDouble(state, "Angle (deg)###mateToolAngle", &deg, 1.0f,
                           -DBL_MAX, DBL_MAX, "%.3f")) {
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
  // The feature tool window's buttons (docs/ui/panels.md):
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

namespace {
// A relation's two sides, each a mate that turns or slides -- this
// assembly's own or one inside a flexible placement, by path (v129), as
// add_relation's a_path/b_path name them. Changing a side is one undo step
// and a solve; a mate lacking the DOF the side reads is refused in place.
// A screw is one mate's rotation to its own slide, so its sides move together.
void DrawRelationSides(AppState& state, Relation& r) {
  AssemblyDocument& asmDoc = *state.assembly;
  const std::vector<DofMateChoice> choices = DofMateChoices(state);
  struct Side {
    const char* label;
    const char* id;
    bool b;
  };
  for (const Side side : {Side{"Drives", "rel_side_a", false},
                          Side{"Driven", "rel_side_b", true}}) {
    if (r.kind == RelationKind::Screw && side.b)
      continue; // one mate, both DOFs
    const std::vector<uint64_t>& path = side.b ? r.mateBPath : r.mateAPath;
    const uint64_t mate = side.b ? r.mateB : r.mateA;
    const MateDof dof = side.b ? r.dofB : r.dofA;
    LeftLabel(side.label);
    const std::string combo = std::string("###") + side.id;
    const DofMateChoice* pick = nullptr;
    if (ImGui::BeginCombo(combo.c_str(),
                          MateLabelAt(state, path, mate).c_str())) {
      for (const DofMateChoice& c : choices) {
        std::string cid = std::to_string(c.mate);
        if (!c.path.empty())
          cid = std::to_string(c.path.front()) + "_" + cid;
        const std::string item = c.label + "###" + side.id + "_" + cid;
        if (ImGui::Selectable(item.c_str(), c.path == path && c.mate == mate))
          pick = &c;
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", side.b ? "The mate this relation moves."
                                     : "The mate that drives it.");
    if (pick == nullptr || (pick->path == path && pick->mate == mate))
      continue;
    state.asmEditRefusalFor = r.id;
    const bool has = dof == MateDof::Rotation ? pick->rotation : pick->offset;
    if (!has) {
      state.asmEditRefusal = "'" + pick->label + "' has no " +
                             (dof == MateDof::Rotation ? "rotation" : "slide") +
                             " for the " + (side.b ? "driven" : "driving") +
                             " side of a " +
                             DocumentIO::RelationKindToString(r.kind) +
                             " relation — pick a mate that " +
                             (dof == MateDof::Rotation ? "turns" : "slides");
      continue;
    }
    if (!pick->path.empty() &&
        (r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click)) {
      state.asmEditRefusal =
          std::string("a ") + DocumentIO::RelationKindToString(r.kind) +
          " keeps history (its lead), which would be per-placement state — "
          "a relation into a flexible placement cannot be one";
      continue;
    }
    state.asmEditRefusal.clear();
    SnapshotAssembly(state);
    if (side.b || r.kind == RelationKind::Screw) {
      r.mateB = pick->mate;
      r.mateBPath = pick->path;
    }
    if (!side.b) {
      r.mateA = pick->mate;
      r.mateAPath = pick->path;
    }
    asmDoc.BumpVersion();
    SolveActiveAssembly(state);
  }
  if (state.asmEditRefusalFor == r.id && !state.asmEditRefusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", state.asmEditRefusal.c_str());
    ImGui::PopStyleColor();
  }
}

// A part placement's two settings: which of the part's configurations it
// is, and what it is made of
// (docs/assemblies/README.md#configurations-per-placement). The same actions
// set_occurrence_configuration and set_occurrence_material are
// (ui/FlexibleActions.h); a refusal shows under them in the app's words.
void DrawPartPlacement(AppState& state, Occurrence& sel) {
  // the live part: the configurations there are to choose from
  const Document* part = PartDocById(state, sel.partId);
  if (part == nullptr)
    return;
  const uint64_t occId = sel.id;
  const bool refusedHere =
      state.asmEditRefusalFor == occId && !state.asmEditRefusal.empty();
  ImGui::Separator();
  if (!part->GetConfigurations().empty()) {
    LeftLabel("Configuration");
    const auto cfgName = [&](uint64_t id) -> std::string {
      if (id == 0) {
        const Configuration* active =
            part->FindConfiguration(part->ActiveConfiguration());
        return std::string("Its active one (") +
               (active ? active->name : std::string("base")) + ")";
      }
      const Configuration* c = part->FindConfiguration(id);
      return c ? c->name : "(missing " + std::to_string(id) + ")";
    };
    uint64_t pickCfg = UINT64_MAX;
    if (ImGui::BeginCombo("###occ_part_config",
                          cfgName(sel.configurationId).c_str())) {
      if (ImGui::Selectable((cfgName(0) + "###occ_part_config_0").c_str(),
                            sel.configurationId == 0))
        pickCfg = 0;
      for (const Configuration& c : part->GetConfigurations())
        if (ImGui::Selectable(
                (c.name + "###occ_part_config_" + std::to_string(c.id)).c_str(),
                sel.configurationId == c.id))
          pickCfg = c.id;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Which configuration of '%s' this placement is. Two "
                        "placements of one part can be two of them.",
                        RefNameById(state, sel).c_str());
    if (pickCfg != UINT64_MAX && pickCfg != sel.configurationId) {
      std::string why;
      state.asmEditRefusalFor = occId;
      if (SetOccurrenceConfiguration(state, occId, pickCfg, &why))
        state.asmEditRefusal.clear();
      else
        state.asmEditRefusal = why;
      return; // the document moved under `sel`; draw it again next frame
    }
  }
  LeftLabel("Material");
  const auto matName = [](const std::string& slug) -> std::string {
    CADMaterials::MaterialType t;
    if (!CADMaterials::MaterialLibrary::TryMaterialTypeFromString(slug, t))
      return slug;
    return CADMaterials::MaterialLibrary::GetMaterial(t).name;
  };
  std::string pickMat;
  bool picked = false;
  if (ImGui::BeginCombo("###occ_material",
                        sel.material.empty() ? "The part's own"
                                             : matName(sel.material).c_str())) {
    if (ImGui::Selectable("The part's own###occ_material_own",
                          sel.material.empty())) {
      picked = true;
      pickMat.clear();
    }
    for (CADMaterials::MaterialType t :
         CADMaterials::MaterialLibrary::AllTypes()) {
      const std::string slug = CADMaterials::MaterialLibrary::Slug(t);
      if (ImGui::Selectable((matName(slug) + "###occ_material_" + slug).c_str(),
                            sel.material == slug)) {
        picked = true;
        pickMat = slug;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("What this placement is made of, for its mass, the BOM "
                      "and how it draws. One part in two woods is one part.");
  if (picked && pickMat != sel.material) {
    std::string why;
    state.asmEditRefusalFor = occId;
    if (SetOccurrenceMaterial(state, occId, pickMat, &why))
      state.asmEditRefusal.clear();
    else
      state.asmEditRefusal = why;
    return;
  }
  if (refusedHere) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", state.asmEditRefusal.c_str());
    ImGui::PopStyleColor();
  }
}

// A sub-assembly placement's two settings
// (docs/assemblies/README.md#flexible-sub-assemblies): which of its
// configurations it uses, and whether its own mechanism moves here. Both go
// through the actions set_occurrence_configuration and
// set_occurrence_flexible are (ui/FlexibleActions.h) -- one undo step each --
// and a refusal is shown under the control that was clicked, in the app's
// words.
void DrawSubAssemblyPlacement(AppState& state, Occurrence& sel) {
  const AssemblyDocument* sub = AssemblyDocById(state, sel.partId);
  if (sub == nullptr)
    return;
  const uint64_t occId = sel.id;
  const bool refusedHere =
      state.asmEditRefusalFor == occId && !state.asmEditRefusal.empty();
  ImGui::Separator();
  LeftLabel("Configuration");
  const auto cfgName = [&](uint64_t id) -> std::string {
    if (id == 0) {
      const Configuration* active = sub->ActiveConfig();
      return std::string("Its active one (") +
             (active ? active->name : std::string("base")) + ")";
    }
    const Configuration* c = sub->FindConfiguration(id);
    return c ? c->name : "(missing " + std::to_string(id) + ")";
  };
  uint64_t pickCfg = UINT64_MAX;
  if (ImGui::BeginCombo("###occ_config",
                        cfgName(sel.configurationId).c_str())) {
    if (ImGui::Selectable((cfgName(0) + "###occ_config_0").c_str(),
                          sel.configurationId == 0))
      pickCfg = 0;
    for (const Configuration& c : sub->GetConfigurations())
      if (ImGui::Selectable(
              (c.name + "###occ_config_" + std::to_string(c.id)).c_str(),
              sel.configurationId == c.id))
        pickCfg = c.id;
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Which configuration of '%s' this placement uses. A flexible "
        "placement's mechanism runs under it: what it holds stays held.",
        RefNameById(state, sel).c_str());
  if (pickCfg != UINT64_MAX && pickCfg != sel.configurationId) {
    std::string why;
    state.asmEditRefusalFor = occId;
    if (SetOccurrenceConfiguration(state, occId, pickCfg, &why))
      state.asmEditRefusal.clear();
    else
      state.asmEditRefusal = why;
    return; // the document moved under `sel`; draw it again next frame
  }
  const Occurrence* live = state.assembly->FindById(occId);
  bool flexible = live->flexible;
  if (ImGui::Checkbox("Flexible###occ_flexible", &flexible)) {
    std::string why;
    state.asmEditRefusalFor = occId;
    if (SetOccurrenceFlexible(state, occId, flexible, &why))
      state.asmEditRefusal.clear();
    else
      state.asmEditRefusal = why;
    return;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Its own mechanism moves in this assembly: its mates "
                      "join the solve and motion studies, posed for this "
                      "placement only. Off, it is one rigid block.");
  if (refusedHere) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", state.asmEditRefusal.c_str());
    ImGui::PopStyleColor();
  } else if (live->flexible) {
    const size_t moving = MovingInnerOccurrences(state, *live).size();
    TextDisabledWrapped(
        (std::to_string(moving) +
         (moving == 1 ? " part moves" : " parts move") +
         " inside it; drag one in the viewport to turn or slide it.")
            .c_str());
  }
}
} // namespace

void EditAssemblyOccurrence(AppState& state, uint64_t occId) {
  if (!state.assembly)
    return;
  // One tool window at a time: an open mate session or relation editor ends.
  if (state.mateTool.active)
    CancelMateTool(state);
  state.asmEditRelation = 0;
  state.asmEditOccurrence = occId;
  state.asmEditRefusal.clear();
}

void EditAssemblyRelation(AppState& state, uint64_t relId) {
  if (!state.assembly)
    return;
  if (state.mateTool.active)
    CancelMateTool(state);
  state.asmEditOccurrence = 0;
  state.asmEditRelation = relId;
  state.asmEditRefusal.clear();
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
    glm::dvec3 pos = sel->position;
    const bool posChanged =
        ExprDragFloat3(state, "Position###occ_pos", &pos.x, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state); // one snapshot per drag session
    if (posChanged) {
      sel->position = pos;
      asmDoc.BumpVersion();
    }
    glm::dvec3 euler = glm::degrees(glm::eulerAngles(sel->orientation));
    const bool rotChanged =
        ExprDragFloat3(state, "Rotation (deg)###occ_rot", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (rotChanged) {
      sel->orientation = glm::dquat(glm::radians(euler));
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
    if (isSub)
      DrawSubAssemblyPlacement(state, *sel);
    else
      DrawPartPlacement(state, *sel);
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
    ImGui::TextDisabled("%s", DocumentIO::RelationKindToString(r->kind));
    DrawRelationSides(state, *r);
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
