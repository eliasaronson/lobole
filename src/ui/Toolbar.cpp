#include "ui/Toolbar.h"
#include "ui/AssemblyActions.h"
#include "ui/JobActions.h" // ActiveCamSetup
#include "ui/SimulationActions.h"

#include "common/Logger.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h" // BuildLoops (extrude gating)
#include "render/RayTracingRenderer.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamPanel.h" // ActiveCamSetup (the job bar's rotary button)
#include "ui/Commands.h"
#include "ui/DrawingActions.h"
#include "ui/FastenerTool.h"
#include "ui/FeatureEditor.h" // kCamToolSlot (the CAM session's pick slot)
#include "ui/FileDialogs.h"
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/RendererSync.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h" // BeginSketchExtrude/Revolve (U8a)
#include "ui/ViewActions.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <string>

namespace ui {

namespace {

// The offline-render dialog: resolution + sample count, a live progress bar
// while a render is in flight, and the last result. Opened by the toolbar's
// Render button; the actual tracing is pumped by main.cpp (PumpRenderJob).
void DrawRenderModal(AppState& state) {
  struct Res {
    const char* label;
    uint32_t w, h;
  };
  // Ordered biggest first, because the complaint about a still is almost
  // always that it is too small: the list stopped at 1920x1080, and this
  // machine's own panel is 3200x2000, so even the largest render was being
  // upscaled 1.7x the moment anyone opened it full-screen and it read as a
  // low-resolution picture of a high-resolution model.
  static const Res kRes[] = {
      {"3840 x 2160 (4K)", 3840, 2160}, {"2560 x 1440", 2560, 1440},
      {"1920 x 1080", 1920, 1080},      {"1280 x 720", 1280, 720},
      {"960 x 540", 960, 540},          {"640 x 480", 640, 480}};
  static int resIdx = 2; // 1920 x 1080, the old default
  // 512, not 128: measured on a 4060, tracing is not what a still costs. At
  // 720p the sample count does not move the wall clock at all; at 4K eight
  // times the samples buys 1.7x, because scene sync, the AS build, the
  // readback and the PNG encode dominate. Noise falls as 1/sqrt(N), so this
  // is most of a stop and a half of cleanliness for nothing.
  static int samples = 512;
  static bool ground = true;
  static bool studio = true;

  ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Render Still", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  RenderJob& job = state.renderJob;

  if (job.active) {
    float frac = job.totalSamples ? static_cast<float>(job.done) /
                                        static_cast<float>(job.totalSamples)
                                  : 0.0f;
    ImGui::Text("Rendering %u x %u ...", job.width, job.height);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0));
    ImGui::Text("%u / %u samples", job.done, job.totalSamples);
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
      job.cancel = true;
  } else {
    if (job.finished) {
      if (job.success) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "%s Saved: %s",
                           ICON_FA_CIRCLE_CHECK, job.resultPath.c_str());
        // Saved is not the same as good: a still that framed nothing is a
        // valid PNG of the backdrop, and it used to be reported as a success
        // with no further comment.
        if (!job.warning.empty())
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s",
                             job.warning.c_str());
      } else
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "Failed: %s",
                           job.error.c_str());
      ImGui::Separator();
    }

    ImGui::TextUnformatted("Resolution");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##res", kRes[resIdx].label)) {
      for (int i = 0; i < IM_ARRAYSIZE(kRes); ++i)
        if (ImGui::Selectable(kRes[i].label, i == resIdx))
          resIdx = i;
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Samples");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##spp", &samples, 16, 1024);
    ImGui::Checkbox("Ground shadows", &ground);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Catch soft shadows on an invisible floor plane");
    ImGui::Checkbox("Studio backdrop", &studio);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("A neutral sweep behind the model. Off shows the "
                        "environment photograph itself, which is a 1k HDRI "
                        "and reads as a blur at this size.");
    }

    // What the ray tracer will actually be given: a part's bodies, or — on an
    // assembly tab, where `document` is null — its visible occurrences. The
    // part-only test left this button dead behind a working Render entry.
    const bool hasBodies =
        state.assembly
            ? state.assembly->OccurrenceCount() > 0
            : (state.document && !state.document->GetBodies().empty());
    const bool rtReady = state.rayTracer && state.rayTracer->IsInitialized();
    const bool canRender = hasBodies && rtReady;

    ImGui::Spacing();
    ImGui::BeginDisabled(!canRender);
    // "###" ids: the visible label starts with an icon glyph, which no test
    // or script should have to spell in a ref.
    if (ImGui::Button(ICON_FA_CAMERA " Render###render_go", ImVec2(150, 0))) {
      job = RenderJob{};
      job.active = true;
      job.width = kRes[resIdx].w;
      job.height = kRes[resIdx].h;
      job.totalSamples = static_cast<uint32_t>(samples);
      job.seed = 1;
      job.groundPlane = ground;
      job.studioBackdrop = studio;
      job.outPath = "render.png";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close###render_close", ImVec2(120, 0)))
      ImGui::CloseCurrentPopup();

    if (!rtReady)
      ImGui::TextDisabled("Ray tracing not available on this GPU.");
    else if (!hasBodies)
      ImGui::TextDisabled(state.assembly ? "Insert a part to render."
                                         : "Add a body to render.");
  }
  ImGui::EndPopup();
}

} // namespace

// Progress for a background STEP import (ui/ImportJob.h). Same shape as the
// render modal above: a bar, a line of text, and a Cancel that only exists
// while cancelling is still safe.
//
// Not a BeginPopupModal keyed to a button press — an import can be started
// from the menu or the file browser, so the modal opens itself whenever a job
// is running and closes when it finishes.
//
// Called from DockLayout, NOT from a toolbar, and that is load-bearing.
// ImGui builds a popup's id from the ID stack of the window that opens it,
// and DrawToolbar is an if/else over the active tab kind inside a child
// window — so a modal opened from the model toolbar would be orphaned the
// moment materialization creates the assembly tab and the branch switches to
// DrawAssemblyToolbar. Half of an import is spent on the far side of exactly
// that switch. Same trap as a keybinding calling OpenPopup (U12).
void DrawImportModal(AppState& state) {
  if (!ImportRunning(state))
    return; // not calling BeginPopupModal is what closes it
  // Asked every frame rather than on a rising edge off a static bool. The
  // static version desynced the moment anything reset around it (the GUI
  // suite re-running this test was enough): it came back believing the popup
  // was already open, so OpenPopup never fired and the import ran with no
  // modal at all. IsPopupOpen is the actual state, so this cannot drift.
  if (!ImGui::IsPopupOpen("Importing STEP"))
    ImGui::OpenPopup("Importing STEP");

  ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Importing STEP", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  BackgroundJob* bg = FindBackgroundJob(state, "import");
  if (state.importJob == nullptr || bg == nullptr) {
    ImGui::EndPopup();
    return;
  }
  const ImportJob& job = *state.importJob;
  const double fraction = job.fraction.load(std::memory_order_relaxed);
  const auto phase =
      static_cast<ImportJob::Phase>(job.phase.load(std::memory_order_relaxed));

  ImGui::TextUnformatted(ImportStatusLine(state).c_str());
  if (fraction >= 0.0) {
    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-FLT_MIN, 0));
  } else {
    // Indeterminate: OCC gives no hook for the file parse, and a bar sitting
    // at 0% reads as "stuck" rather than "working".
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                       ImVec2(-FLT_MIN, 0), "working");
  }

  ImGui::Spacing();
  // Cancelling once tabs exist would leave a half-built assembly behind, so
  // the button goes away exactly when unwinding stops being free.
  const bool cancellable = phase != ImportJob::Phase::Materializing;
  ImGui::BeginDisabled(!cancellable || bg->cancel.load());
  if (ImGui::Button("Cancel###import_cancel", ImVec2(120, 0)))
    bg->cancel.store(true);
  ImGui::EndDisabled();
  if (!cancellable) {
    ImGui::SameLine();
    ImGui::TextDisabled("finishing");
  }
  ImGui::EndPopup();
}

namespace {

// Start a new row when the next item would not fit. U4 chose a horizontal
// scrollbar over clipping so no button was lost — but a dogfooding round
// measured what that costs: at 1280x720 the row overflows, and the tools
// past the edge are not merely off-screen, they are CULLED, so nothing can
// click them until the bar is scrolled. The ones that fall off are the last
// in the row, which is Simulation, CAM, Section and Measure — the
// mode-level tools, and CAM had no other route at all. Wrapping keeps U4's
// intent (lose nothing) and costs one row of height instead.
void WrapIfNeeded(float itemWidth) {
  const float x = ImGui::GetCursorPosX();
  if (x > ImGui::GetStyle().WindowPadding.x &&
      x + itemWidth > ImGui::GetContentRegionMax().x)
    ImGui::NewLine();
}

// An icon button with a tooltip. Returns true when clicked. The low-level
// one: prefer CommandButton, which fills in the chord and the gate.
bool ToolButton(const char* icon, const char* tooltip, bool enabled = true) {
  WrapIfNeeded(ImGui::CalcTextSize(icon, nullptr, true).x +
               ImGui::GetStyle().FramePadding.x * 2.0f +
               ImGui::GetStyle().ItemSpacing.x);
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::Button(icon);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", tooltip);
  ImGui::SameLine();
  return clicked;
}

// The toolbar's normal button: an icon, the command it runs, and a tooltip
// that carries the command's LIVE chord — "Fit view (F)" is generated, not
// typed. Enablement comes from the command's own gate, so the button and the
// key can never disagree about whether the action is available, and the
// palette agrees with both (docs/ui/u12-keybindings.md).
//
// Every button on every toolbar goes through this. A new button without a
// command id fails the keys/every_toolbar_button_is_a_command GUI test.
bool CommandButton(AppState& state, const char* icon, const char* id,
                   const char* tooltip, bool active = false) {
  if (active)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  const bool clicked = ToolButton(icon, WithChord(state, id, tooltip).c_str(),
                                  CommandEnabled(state, id));
  if (active)
    ImGui::PopStyleColor();
  if (clicked)
    RunCommand(state, id);
  return clicked;
}

void Divider() {
  ImGui::SameLine();
  // A divider that lands at the wrap point would open a row with a stray
  // separator, so it wraps too — and then the new row starts with a button.
  WrapIfNeeded(ImGui::CalcTextSize("|").x +
               ImGui::GetStyle().ItemSpacing.x * 2.0f);
  if (ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x) {
    ImGui::TextDisabled("|");
    ImGui::SameLine();
  }
}

// The disclosure caret of a flyout group, hung off the tool button it
// belongs to. It used to be ICON_FA_CARET_DOWN in a button of its own, which
// made it the loudest thing on the sketch bar: the glyph is drawn at the ICON
// size (the tool icons' size) and the frame is a full tool-button frame, so a
// hint read as a second tool. Commercial CAD hangs a small unframed arrow
// off the icon instead.
//
// Sized off the FRAME, not the icon font: a caret must stay a fraction of the
// button it is attached to at any UI scale (Theme.cpp ScaleAllSizes), which a
// glyph cannot do. Frameless until pointed at, so the group reads as one
// tool with an affordance rather than two buttons.
bool FlyoutCaret() {
  const float h = ImGui::GetFrameHeight();
  const float w = std::floor(h * 0.45f);
  ImGui::SameLine(0.0f, 0.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("###more", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hovered || held) {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                              : ImGuiCol_ButtonHovered),
                      ImGui::GetStyle().FrameRounding);
  }
  // ~8 x 5 px at the default font, the size both references use.
  const float halfW = std::max(2.0f, ImGui::GetFontSize() * 0.25f);
  const float halfH = halfW * 0.62f;
  const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
  dl->AddTriangleFilled(
      ImVec2(c.x - halfW, c.y - halfH), ImVec2(c.x + halfW, c.y - halfH),
      ImVec2(c.x, c.y + halfH),
      ImGui::GetColorU32(ImGuiCol_Text, hovered || held ? 1.0f : 0.6f));
  return pressed;
}

// The model-mode toolbar: create features, fit, planes.
// The Section tool button. Shared by the part and assembly toolbars — a
// section is a property of the VIEW, so it means the same thing on either,
// and one id (###tb_section) is what tests and agents address. Only one
// toolbar is drawn at a time, so the shared id is unambiguous.
void SectionToolButton(AppState& state) {
  CommandButton(state, ICON_FA_SCISSORS "###tb_section", "view.section",
                "Section view (up to four cut planes at once)",
                state.sectionToolOpen);
}

void DrawModelToolbar(AppState& state) {
  // Icon buttons carry a stable "###tb_*" id (hidden from the glyph label) so
  // the GUI/remote tests can address them regardless of the icon codepoint.
  // The tooltip's chord and the enable gate both come from the command.
  const auto btn = [&state](const char* icon, const char* id, const char* tip) {
    CommandButton(state, icon, id, tip);
  };

  btn(ICON_FA_PENCIL "###tb_new_sketch", "part.new_sketch", "New Sketch");

  Divider();

  btn(ICON_FA_CUBE "###tb_box", "part.box", "Box");
  btn(ICON_FA_CIRCLE "###tb_sphere", "part.sphere", "Sphere");
  btn(ICON_FA_DATABASE "###tb_cylinder", "part.cylinder", "Cylinder");
  btn(ICON_FA_CLONE "###tb_derive", "part.derive",
      "Derive (a copy of another part, to draw on)");

  Divider();

  // Turn a FINISHED sketch into a solid. The same operations live in the
  // sketch toolbar for a sketch you are still editing; without these, closing
  // a sketch with the check mark left no way to extrude it at all.
  btn(ICON_FA_UP_LONG "###tb_extrude", "part.extrude",
      "Extrude a sketch (the selected one, else the last)");
  btn(ICON_FA_ROTATE "###tb_revolve", "part.revolve",
      "Revolve a sketch (the selected one, else the last)");

  Divider();

  // Curve-driven features (curves-3d): a 3D curve, and the sweep/loft/pipe
  // that consume it. Each command gates on the references it needs.
  btn(ICON_FA_WAVE_SQUARE "###tb_curve3d", "part.curve3d",
      "3D Curve (helix / fit-spline)");
  btn(ICON_FA_ROUTE "###tb_sweep", "part.sweep",
      "Sweep a profile along a path");
  btn(ICON_FA_BEZIER_CURVE "###tb_loft", "part.loft", "Loft through sections");
  btn(ICON_FA_ROAD "###tb_pipe", "part.pipe", "Pipe / tube along a path");

  Divider();

  // Datums.
  btn(ICON_FA_TABLE_CELLS_LARGE "###tb_datum_plane", "part.datum_plane",
      "Datum Plane");
  btn(ICON_FA_ARROWS_UP_DOWN "###tb_datum_axis", "part.datum_axis",
      "Datum Axis");
  btn(ICON_FA_LOCATION_CROSSHAIRS "###tb_frame", "part.frame",
      "Datum Frame (a named origin + axes a mate anchors to)");

  Divider();

  // Body combinators (need existing bodies).
  btn(ICON_FA_OBJECT_GROUP "###tb_boolean", "part.boolean",
      "Boolean (union / subtract / intersect)");
  btn(ICON_FA_LEFT_RIGHT "###tb_mirror", "part.mirror", "Mirror a body");
  btn(ICON_FA_GRIP "###tb_pattern", "part.pattern",
      "Pattern (linear / circular)");

  Divider();

  // Edge / wall / detail ops on a body.
  btn(ICON_FA_BORDER_TOP_LEFT "###tb_fillet", "part.fillet", "Fillet edges");
  btn(ICON_FA_CROP_SIMPLE "###tb_chamfer", "part.chamfer", "Chamfer edges");
  btn(ICON_FA_BARS_STAGGERED "###tb_rib", "part.rib",
      "Rib (thin web from an open sketch)");
  btn(ICON_FA_BULLSEYE "###tb_hole", "part.hole",
      "Hole: pick a flat face, click the centres");
  btn(ICON_FA_CIRCLE_NOTCH "###tb_thread", "part.thread",
      "Thread (cosmetic; pick a cyl. face)");
  btn(ICON_FA_BOX_OPEN "###tb_shell", "part.shell",
      "Shell (hollow a body; pick faces to open)");
  // A tapered face, not ANGLES_RIGHT — that glyph is "»", which every other
  // toolbar in the world uses to mean "more items", so a tool wearing it
  // mid-row reads as broken chrome rather than as a modelling operation.
  btn(ICON_FA_MOUNTAIN "###tb_draft", "part.draft",
      "Draft (taper faces for moulding)");
  // A rectangle divided in two. Split used to be SCISSORS, which Section
  // (SectionToolButton, above) also wears — two commands, one glyph, one
  // bar. Section keeps the scissors: it is the more-used of the two, and a
  // view cut is what people reach for scissors expecting.
  btn(ICON_FA_TABLE_COLUMNS "###tb_split", "part.split",
      "Split a body with a plane or one of its faces");
  btn(ICON_FA_BROOM "###tb_refine", "part.refine",
      "Refine (merge coplanar faces and boolean seams)");

  Divider();

  btn(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_fit", "view.fit",
      "Fit view");
  // An eye, not a second grid: TABLE_CELLS sat one row from Datum Plane's
  // TABLE_CELLS_LARGE, so the bar carried two near-identical grids meaning
  // unrelated things. A visibility toggle should look like one — and it
  // shows what the NEXT click does, which a static glyph cannot. The icon
  // font is loaded with a fixed advance (Theme.cpp GlyphMinAdvanceX), so
  // swapping the glyph cannot change the button width or re-wrap the row.
  const std::string planesIcon =
      std::string(state.showPlanes ? ICON_FA_EYE : ICON_FA_EYE_SLASH) +
      "###tb_planes";
  btn(planesIcon.c_str(), "view.toggle_planes", "Toggle planes");

  // Enter the FEM simulation workspace (needs a body to analyze).
  btn(ICON_FA_GAUGE_HIGH "###tb_simulation", "part.simulation",
      "Simulation (FEM stress study)");

#ifdef HAVE_CAM
  // CAM left the part bar (docs/cam/job-document.md, round two, M3): a job
  // is made from the tab bar's "+" the way an assembly is, then given a
  // machine, a stock and parts. The one-click "machine this part" built a
  // job around one part, which is the shape of "cut this" and not of "set
  // up a job".
#endif

  SectionToolButton(state);

  // Measure tool (C3): a viewport mode — pick a face/edge for its own measure,
  // a second element for the distance. The button reflects the armed state,
  // snapshotted BEFORE the click toggles it (a live read would unbalance the
  // style stack and trip ImGui's PopStyleColor assert).
  CommandButton(state, ICON_FA_RULER "###tb_measure", "view.measure",
                "Measure (pick 1-2 faces/edges; Esc to exit)",
                state.measure.active);

  Divider();

  // Offline path-traced still (KeyShot-style). Opens the render dialog — the
  // command records the request rather than calling OpenPopup, because the
  // same command runs from a key press outside any window (Commands.h).
  btn(ICON_FA_CAMERA "###tb_render", "view.render",
      "Render (offline raytrace)");
  if (TakePopupRequest(state, "Render Still"))
    ImGui::OpenPopup("Render Still");
  DrawRenderModal(state);

  ImGui::NewLine();
}

// The sketch-mode toolbar: draw tools, generative ops, and constraints — the
// icon ribbon that replaces the model tools while a sketch is being edited.
void DrawSketchModeToolbar(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  // Finish (accept) the sketch. Escape is a MODAL VERB, not a command — it
  // resolves against the armed tool, then the selection, then the session —
  // so it is deliberately not in the keymap and its hint stays a literal
  // (docs/ui/u12-keybindings.md).
  const bool holeSession = edit.purpose == SketchPurpose::Hole;
  if (ToolButton(ICON_FA_CHECK "###tb_sk_finish",
                 holeSession ? "Hole options (Enter) - Esc cancels the hole"
                             : "Finish sketch (Esc)")) {
    if (holeSession)
      BeginSketchHole(state); // the sketch session is torn down inside
    else
      edit = SketchEditState{};
    ImGui::NewLine(); // close the row we are abandoning
    return;           // `edit` is gone; nothing below may touch it this frame
  }
  Divider();

  // A draw/modify tool button: the command arms the tool, and the button
  // lights up while that tool is the armed one.
  auto toolBtn = [&](const char* icon, const char* id, const char* tip,
                     SketchTool t) {
    CommandButton(state, icon, id, tip, edit.tool == t);
  };
  // A flyout group: the face shows the last-picked variant —
  // click it to use that tool — and the caret opens a popup to switch which
  // variant the group holds. `current` persists the pick across frames. Each
  // VARIANT is its own command, so every one of them can carry a chord.
  struct ToolVariant {
    const char* icon;
    const char* id;
    const char* label;
    SketchTool tool;
  };
  auto toolGroup = [&](const char* id, int& current,
                       std::initializer_list<ToolVariant> variants) {
    const ToolVariant* v = variants.begin();
    const int n = static_cast<int>(variants.size());
    if (current < 0 || current >= n)
      current = 0;
    bool groupActive = false;
    for (int i = 0; i < n; i++)
      if (edit.tool == v[i].tool) {
        groupActive = true;
        current = i;
        break;
      }
    ImGui::PushID(id);
    if (groupActive)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(v[current].icon))
      RunCommand(state, v[current].id);
    if (groupActive)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "%s", WithChord(state, v[current].id, v[current].label).c_str());
    if (FlyoutCaret())
      ImGui::OpenPopup("variants");
    if (ImGui::BeginPopup("variants")) {
      for (int i = 0; i < n; i++) {
        const std::string item = std::string(v[i].icon) + "   " +
                                 WithChord(state, v[i].id, v[i].label);
        if (ImGui::Selectable(item.c_str(), i == current)) {
          current = i;
          RunCommand(state, v[i].id);
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SameLine();
  };
  // Which variant each group currently holds (remembered across frames).
  static int circleVariant = 0;
  static int rectVariant = 0;
  static int arcVariant = 0;

  toolBtn(ICON_FA_SLASH, "sketch.line", "Line", SketchTool::Line);
  toolGroup(
      "circlegrp", circleVariant,
      {{ICON_FA_CIRCLE_NOTCH, "sketch.circle", "Circle", SketchTool::Circle},
       {ICON_FA_EGG, "sketch.ellipse", "Ellipse", SketchTool::Ellipse}});
  toolGroup("rectgrp", rectVariant,
            {{ICON_FA_VECTOR_SQUARE, "sketch.rectangle", "Rectangle",
              SketchTool::Rectangle},
             {ICON_FA_EXPAND, "sketch.rectangle_center", "Centre Rectangle",
              SketchTool::RectangleCenter}});
  toolGroup("arcgrp", arcVariant,
            {{ICON_FA_BEZIER_CURVE, "sketch.arc_3point", "3-Point Arc",
              SketchTool::ArcThreePoint},
             {ICON_FA_BEZIER_CURVE, "sketch.arc_center",
              "Centre Arc (centre, start, end)", SketchTool::Arc},
             {ICON_FA_BEZIER_CURVE, "sketch.ellipse_arc",
              "Elliptic Arc (centre, major, minor, start, end)",
              SketchTool::EllipseArc}});
  toolBtn(ICON_FA_PEN_NIB, "sketch.spline", "Spline", SketchTool::Spline);
  toolBtn(ICON_FA_CIRCLE_DOT, "sketch.point", "Point", SketchTool::Point);
  toolBtn(ICON_FA_SCISSORS, "sketch.trim", "Trim", SketchTool::Trim);
  toolBtn(ICON_FA_RULER_COMBINED, "sketch.dimension", "Dimension",
          SketchTool::Dimension);
  Divider();
  toolBtn(ICON_FA_LEFT_RIGHT, "sketch.mirror", "Mirror", SketchTool::Mirror);
  toolBtn(ICON_FA_DRAW_POLYGON, "sketch.fillet", "Fillet a corner",
          SketchTool::Fillet);
  toolBtn(ICON_FA_GRIP, "sketch.pattern", "Pattern", SketchTool::Pattern);
  toolBtn(ICON_FA_CLONE, "sketch.offset", "Offset", SketchTool::Offset);
  toolBtn(ICON_FA_DIAGRAM_PROJECT, "sketch.project",
          "Project geometry — click a body edge/face -> live reference",
          SketchTool::Project);
  Divider();

  // Constraints act on the current selection (validated in
  // ApplySketchConstraint; an invalid combination just no-ops with a hint).
  auto conBtn = [&](const char* label, const char* id, const char* tip) {
    CommandButton(state, label, id, tip);
  };
  // The everyday constraints stay visible; the rest live in the flyout below.
  conBtn("H", "sketch.constraint_horizontal", "Horizontal");
  conBtn("V", "sketch.constraint_vertical", "Vertical");
  conBtn(ICON_FA_ARROWS_TO_DOT, "sketch.constraint_coincident", "Coincident");
  conBtn(ICON_FA_ARROWS_LEFT_RIGHT, "sketch.constraint_parallel", "Parallel");
  conBtn(ICON_FA_ANGLE_RIGHT, "sketch.constraint_perpendicular",
         "Perpendicular");

  // "More constraints" flyout: the less-common relational constraints, one
  // click away. Always openable (so the menu is discoverable); each item
  // applies to the current selection and ApplySketchConstraint no-ops with a
  // hint if the combination doesn't fit. Dimensional constraints are NOT here —
  // the Dimension tool covers distance / radius / diameter / angle. These are
  // the keymap's unbound-but-bindable rows: a constraint used once a month
  // gets a Preferences row rather than an invented mnemonic.
  if (ImGui::Button(ICON_FA_ELLIPSIS "##morecon"))
    ImGui::OpenPopup("more_constraints");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("More constraints…");
  ImGui::SameLine();
  if (ImGui::BeginPopup("more_constraints")) {
    // Disabled without a selection, like the visible constraint buttons — a
    // Selectable that runs and silently does nothing is worse than one that
    // says it cannot.
    auto conItem = [&](const char* label, const char* id, const char* tip) {
      const bool enabled = CommandEnabled(state, id);
      ImGui::BeginDisabled(!enabled);
      if (ImGui::Selectable(WithChord(state, id, label).c_str()) && enabled)
        RunCommand(state, id);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);
    };
    conItem("Tangent", "sketch.constraint_tangent",
            "Tangent (line/curve + curve)");
    conItem("Equal", "sketch.constraint_equal",
            "Equal size (2 curves, or 2 lines)");
    conItem("Concentric", "sketch.constraint_concentric",
            "Concentric (2 curves share a centre)");
    ImGui::Separator();
    conItem("Point on line", "sketch.constraint_point_on_line",
            "Point lies on a line (point + line)");
    conItem("Symmetric", "sketch.constraint_symmetric",
            "Symmetric (2 points + a line)");
    conItem("Midpoint", "sketch.constraint_midpoint",
            "Point at a line's midpoint (point + line)");
    conItem("Fix / pin", "sketch.constraint_fixed", "Pin a point in place");
    conItem("Collinear", "sketch.constraint_collinear", "Collinear (2 lines)");
    ImGui::EndPopup();
  }
  Divider();

  CommandButton(state, ICON_FA_MINUS "##constr", "sketch.toggle_construction",
                "Construction — reference geometry, dashed");
  Divider();

  // Mode toggles (were the Sketch panel's checkboxes, U8a).
  CommandButton(state, ICON_FA_MAGNET "###tb_sk_autocon",
                "sketch.toggle_autoconstrain",
                "Auto-constrain — snap and add constraints while drawing "
                "(hold Ctrl to place freely)",
                edit.autoConstrain);
  CommandButton(state, ICON_FA_EYE "###tb_sk_glyphs", "sketch.toggle_glyphs",
                "Show constraint glyphs", edit.showConstraintGlyphs);
  // Slice at the sketch plane: what helps when the sketch is INSIDE existing
  // material, where there is no consuming feature for the automatic rollback
  // to stop before. Greyed with a reason rather than clicking to no effect.
  const std::string sliceTip =
      (state.sketchSlice || CanSketchSlice(state))
          ? "Slice at the sketch plane \xe2\x80\x94 hide everything between "
            "the camera and the sketch"
          : "Slice at the sketch plane \xe2\x80\x94 no clip plane free, all "
            "four section planes are in use";
  CommandButton(state, ICON_FA_CIRCLE_HALF_STROKE "###tb_sk_slice",
                "sketch.toggle_slice", sliceTip.c_str(), state.sketchSlice);
  Divider();

  // Insert reference image (U8 polish): file dialog, then the fresh image's
  // session (opacity/calibrate/delete) opens in the Tool window. Editing
  // later: double-click the image's Operations row in the Model panel.
  CommandButton(state, ICON_FA_IMAGE "###tb_sk_image", "sketch.image",
                "Insert reference image\xe2\x80\xa6");
  Divider();

  // Extrude/Revolve straight from the sketch (were panel buttons, U8a);
  // Finish already exits without a solid. BeginSketchHole tears down the
  // sketch session, so `edit` must not be touched after these run — every
  // one of them is the last thing this function does with it.
  CommandButton(state, ICON_FA_UP_LONG "###tb_sk_extrude", "sketch.extrude",
                "Extrude the sketch");
  CommandButton(state, ICON_FA_ROTATE "###tb_sk_revolve", "sketch.revolve",
                "Revolve the sketch");
  CommandButton(state, ICON_FA_BULLSEYE "###tb_sk_hole", "sketch.hole",
                "Drill holes at the sketch's points");
  Divider();

  CommandButton(state,
                ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_sk_fit",
                "view.fit", "Fit view");
  ImGui::NewLine();
}

namespace {
// X to drop the next occurrence at: clear of everything already placed, by
// the shared rule (ui/Placement.h). Measured between the SHAPES, so the gap
// looks right whatever local origin each part models around.
double NextInsertX(AppState& state, uint64_t slotId, OccurrenceRef kind) {
  // A5: a slot is a part OR an assembly, and an assembly's span is its whole
  // flattened subtree. Resolving only parts meant a placed sub-assembly did
  // not count as occupied, so the next insert could land inside it.
  const auto spanOf = [&](uint64_t id, OccurrenceRef k, Span& out) {
    out = {1e30, -1e30};
    const auto addBox = [&](const glm::vec3& bmin, const glm::vec3& bmax) {
      out.lo = std::min(out.lo, static_cast<double>(bmin.x));
      out.hi = std::max(out.hi, static_cast<double>(bmax.x));
    };
    if (k == OccurrenceRef::Assembly) {
      const AssemblyDocument* sub = AssemblyDocById(state, id);
      if (sub == nullptr)
        return false;
      auto flat = assembly::Flatten(*sub, PartResolverFor(state),
                                    AssemblyResolverFor(state));
      if (!flat)
        return false;
      for (const assembly::FlatInstance& inst : *flat) {
        if (!inst.visible)
          continue;
        for (const Body& b : inst.part->GetBodies()) {
          glm::vec3 bmin, bmax;
          b.shape.GetBoundingBox(bmin, bmax);
          for (int c = 0; c < 8; c++) {
            const glm::vec3 corner((c & 1) ? bmax.x : bmin.x,
                                   (c & 2) ? bmax.y : bmin.y,
                                   (c & 4) ? bmax.z : bmin.z);
            const glm::vec3 w = glm::vec3(inst.world * glm::vec4(corner, 1.0f));
            addBox(w, w);
          }
        }
      }
      return out.lo <= out.hi;
    }
    Document* doc = PartDocById(state, id);
    if (doc == nullptr || doc->GetBodies().empty())
      return false;
    for (const Body& b : doc->GetBodies()) {
      glm::vec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      addBox(bmin, bmax);
    }
    return out.lo <= out.hi;
  };
  std::vector<Span> taken;
  for (const Occurrence& o : state.assembly->GetOccurrences()) {
    Span s;
    if (spanOf(o.partId, o.refKind, s))
      taken.push_back({s.lo + o.position.x, s.hi + o.position.x});
  }
  Span incoming;
  if (!spanOf(slotId, kind, incoming))
    return 0.0;
  return ClearOffset(taken, incoming);
}
} // namespace

// The assembly-mode toolbar: mate-creation tools (A3). Shown when an assembly
// tab is active (no part document). Fit reuses FitCameraToDocument.
void DrawAssemblyToolbar(AppState& state) {
  // Insert: place an occurrence of a part OR of another assembly (A5). ONE
  // button, because "insert" is one intent — you know the NAME of what you
  // want, and making the user classify it first is backwards. Two labelled
  // sections rather than a submenu: a submenu costs a click and hides the
  // smaller group, which is usually the assemblies, and those are the more
  // consequential insert. Assemblies come first for the same reason, each
  // with the part count you are about to place.
  // The command records the request rather than calling OpenPopup, because
  // the same command runs from a key press outside any window (Commands.h).
  CommandButton(state, ICON_FA_CIRCLE_PLUS "###tb_insert", "assembly.insert",
                "Insert a part or sub-assembly (place an occurrence)");
  if (TakePopupRequest(state, "insert_occurrence"))
    ImGui::OpenPopup("insert_occurrence");
  if (ImGui::BeginPopup("insert_occurrence")) {
    const auto place = [&](const PartSlot& slot, OccurrenceRef kind) {
      SnapshotAssembly(state);
      Occurrence occ;
      occ.partId = slot.id;
      occ.refKind = kind;
      occ.name = slot.name;
      occ.fixed = state.assembly->OccurrenceCount() == 0; // first = anchor
      // Everything models around its own origin, so inserting two of them
      // buries the second inside the first — you had to drag them apart
      // before you could even see what you inserted. Park each new one clear
      // of what is already placed, along +X (mates move it anyway).
      occ.position.x = NextInsertX(state, slot.id, kind);
      const uint64_t id = state.assembly->AddOccurrence(occ);
      SelectOccurrence(state, id);
      SolveActiveAssembly(state);
    };

    // Which slot is the assembly being edited? It can never contain itself.
    uint64_t selfId = 0;
    for (const PartSlot& s : state.parts)
      if (s.kind == DocKind::Assembly &&
          AssemblyDocById(state, s.id) == state.assembly.get())
        selfId = s.id;

    bool anyAsm = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Assembly || s.id == selfId)
        continue;
      if (!anyAsm) {
        ImGui::TextDisabled("Assemblies");
        ImGui::Separator();
        anyAsm = true;
      }
      // A reference that would close a cycle is shown DISABLED with the
      // reason, not hidden: hiding it reads as the app forgetting the
      // assembly exists.
      std::string why;
      const bool cycles = assembly::WouldCycle(
          *state.assembly, selfId, s.id, OccurrenceRef::Assembly,
          AssemblyResolverFor(state), &why);
      int leaves = 0;
      if (const AssemblyDocument* sub = AssemblyDocById(state, s.id)) {
        auto flat = assembly::Flatten(*sub, PartResolverFor(state),
                                      AssemblyResolverFor(state));
        if (flat)
          leaves = static_cast<int>(flat->size());
      }
      const std::string label =
          s.name + "  (" + std::to_string(leaves) + " parts)";
      ImGui::BeginDisabled(cycles);
      if (ImGui::MenuItem(label.c_str()))
        place(s, OccurrenceRef::Assembly);
      ImGui::EndDisabled();
      if (cycles && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why.c_str());
    }

    bool anyPart = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part)
        continue;
      if (!anyPart) {
        if (anyAsm)
          ImGui::Spacing();
        ImGui::TextDisabled("Parts");
        ImGui::Separator();
        anyPart = true;
      }
      if (ImGui::MenuItem(s.name.c_str()))
        place(s, OccurrenceRef::Part);
    }
    if (!anyAsm && !anyPart)
      ImGui::TextDisabled("Nothing to insert yet — add a part first.");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  Divider();

  // Every button carries a stable "###tb_*" id like the model toolbar's, so
  // tests and scripted humans address them by name rather than by icon
  // codepoint (the assembly/drawing/simulation bars had none at all).
  auto mateBtn = [&](const char* icon, const char* id, const char* tip,
                     MateType t) {
    CommandButton(state, icon, id, tip,
                  state.mateTool.active && state.mateTool.type == t);
  };
  mateBtn(ICON_FA_LOCK "###tb_mate_fastened", "assembly.mate_fastened",
          "Fastened mate (rigid)", MateType::Fastened);
  mateBtn(ICON_FA_ARROWS_SPIN "###tb_mate_revolute", "assembly.mate_revolute",
          "Revolute mate (hinge)", MateType::Revolute);
  mateBtn(ICON_FA_ARROWS_LEFT_RIGHT "###tb_mate_slider", "assembly.mate_slider",
          "Slider mate", MateType::Slider);
  mateBtn(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "###tb_mate_cylindrical",
          "assembly.mate_cylindrical", "Cylindrical mate (spin + slide)",
          MateType::Cylindrical);
  mateBtn(ICON_FA_TABLE_CELLS_LARGE "###tb_mate_planar", "assembly.mate_planar",
          "Planar mate (slide in a plane)", MateType::Planar);
  mateBtn(ICON_FA_CIRCLE_DOT "###tb_mate_ball", "assembly.mate_ball",
          "Ball mate (free rotation)", MateType::Ball);
  Divider();
  // A standard part is placed the way a mate is made: pick geometry, and the
  // numbers derive from it (docs/parts/gui-plan.md).
  CommandButton(state, ICON_FA_SCREWDRIVER "###tb_fastener",
                "assembly.fastener",
                "Fastener (pick a hole's rim; size and length derive)",
                state.fastenerTool.open);
  Divider();
  // The two viewport modes. They open the floating tool window rather than a
  // panel section, so they belong on the bar with the other tools that do
  // (docs/ui/u15-assembly-panel.md).
  CommandButton(state, ICON_FA_EXPLOSION "###tb_asm_explode",
                "assembly.explode", "Exploded view (slide the parts apart)",
                state.explodeToolOpen);
  CommandButton(state, ICON_FA_FILM "###tb_asm_motion", "assembly.motion",
                "Motion study (drive a mate and scrub the mechanism)",
                state.motionToolOpen);
  CommandButton(state, ICON_FA_GAUGE_HIGH "###tb_asm_simulation",
                "assembly.simulation",
                "Simulation (mesh one occurrence, clamp it where the others "
                "touch)",
                state.simulationToolOpen);

  Divider();
  CommandButton(state,
                ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_asm_fit",
                "view.fit", "Fit view");

  // Measure across the assembly: same tool as the part tab — picks carry
  // their occurrence, so distances span occurrences in world space. Snapshot
  // the armed state so the toggling click can't unbalance the style stack (see
  // the model-toolbar measure button).
  SectionToolButton(state);

  CommandButton(state, ICON_FA_RULER "###tb_asm_measure", "view.measure",
                "Measure (pick 1-2 faces/edges; Esc to exit)",
                state.measure.active);

  Divider();

  // Offline path-traced still, same as the part tab. SyncRayTracingScene has
  // walked assembly occurrences at their solved placements since the RT work
  // landed; only the buttons and the RPC guard said otherwise, so an assembly
  // could be modelled, mated and exploded but never rendered.
  CommandButton(state, ICON_FA_CAMERA "###tb_asm_render", "view.render",
                "Render (offline raytrace)");
  if (TakePopupRequest(state, "Render Still"))
    ImGui::OpenPopup("Render Still");
  DrawRenderModal(state);

  if (state.mateTool.active && !state.mateTool.message.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.mateTool.message.c_str());
  }
  if (state.mateTool.active && state.mateTool.hasHover &&
      state.mateTool.previewMateId == 0) {
    const bool edgeSnap = state.mateTool.hover.anchor.snapEdge.IsSet();
    ImGui::SameLine();
    ImGui::TextColored(edgeSnap ? ImVec4(1.0f, 0.78f, 0.22f, 1.0f)
                                : ImVec4(0.82f, 0.82f, 0.60f, 1.0f),
                       "%s",
                       !edgeSnap                          ? "Snap: face center"
                       : state.mateTool.hoverSnapIsCorner ? "Snap: corner"
                       : state.mateTool.hoverSnapIsMidpoint
                           ? "Snap: edge midpoint"
                           : "Snap: hole center");
  }
  ImGui::NewLine();
}

// The simulation-mode toolbar: boundary-condition tools, run, exit — replaces
// the model tools while a FEM study is being set up.
void DrawSimulationToolbar(AppState& state) {
  SimulationState& sim = state.simulation;

  // A boundary-condition tool button: armed state highlighted like Measure.
  auto bcBtn = [&](const char* icon, const char* id, const char* tip,
                   SimulationState::Tool t) {
    CommandButton(state, icon, id, tip, sim.tool == t); // toggles
  };
  bcBtn(ICON_FA_LOCK "###tb_sim_fixed", "sim.fixed",
        "Fixed support (click a face)", SimulationState::Tool::Fixed);
  bcBtn(ICON_FA_BOLT "###tb_sim_force", "sim.force",
        "Force load (click a face)", SimulationState::Tool::Force);
  bcBtn(ICON_FA_DOWN_LONG "###tb_sim_pressure", "sim.pressure",
        "Pressure load (click a face)", SimulationState::Tool::Pressure);

  Divider();

  // The command uses the same worker-thread paths the panel's Run uses. This
  // called the blocking RunStudyAndCache once, so the toolbar could freeze the
  // app for the length of a solve — and on a topology study, for the length of
  // an optimization.
  CommandButton(state, ICON_FA_PLAY "###tb_sim_run", "sim.run",
                "Run study (mesh + solve)");

  Divider();

  // Escape is a fixed modal verb, not a binding — the same rule the sketch
  // session's check mark follows (docs/ui/u12-keybindings.md).
  if (ToolButton(ICON_FA_ARROW_RIGHT_FROM_BRACKET "###tb_sim_exit",
                 "Exit Simulation (Esc)"))
    ExitSimulation(state);

  // Hint while a BC tool is armed.
  if (sim.tool != SimulationState::Tool::None) {
    ImGui::SameLine();
    const char* what = sim.tool == SimulationState::Tool::Fixed
                           ? "fixed support"
                       : sim.tool == SimulationState::Tool::Force ? "force"
                                                                  : "pressure";
    ImGui::TextDisabled("click a face to place the %s", what);
  }
  ImGui::NewLine();
}

} // namespace

// The drawing-mode toolbar: add a projected view of a part/assembly (the
// "add part" of a sheet), re-project, the dimension tools, fit, export.
void DrawDrawingToolbar(AppState& state) {
  // Add view: a picker of part/assembly sources, each with orientation
  // submenus — the sheet equivalent of inserting a part.
  CommandButton(state, ICON_FA_CIRCLE_PLUS "###tb_dwg_add_view",
                "drawing.add_view", "Add view (project a part/assembly)");
  if (TakePopupRequest(state, "add_view"))
    ImGui::OpenPopup("add_view");
  if (ImGui::BeginPopup("add_view")) {
    ImGui::TextDisabled("Add view of");
    ImGui::Separator();
    struct Ori {
      const char* label;
      ViewOrientation o;
    };
    static const Ori orients[] = {{"Front", ViewOrientation::Front},
                                  {"Top", ViewOrientation::Top},
                                  {"Right", ViewOrientation::Right},
                                  {"Iso", ViewOrientation::Iso}};
    bool any = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part && s.kind != DocKind::Assembly)
        continue;
      any = true;
      if (ImGui::BeginMenu(s.name.c_str())) {
        for (const Ori& ori : orients) {
          if (ImGui::MenuItem(ori.label)) {
            DrawingView v;
            v.name = ori.label;
            v.sourceDocId = s.id;
            v.orientation = ori.o;
            v.scale = 0.0; // 0 = fit a standard scale to the sheet
            AddDrawingViewPlaced(state, v);
          }
        }
        ImGui::EndMenu();
      }
    }
    if (!any)
      ImGui::TextDisabled("No parts/assemblies — add one with +Part/+Asm.");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  Divider();

  CommandButton(state, ICON_FA_ARROWS_ROTATE "###tb_dwg_update",
                "drawing.update", "Update views (re-project)");

  Divider();

  // Dimension tools: arm state.dimTool; the sheet then picks projected refs.
  auto dimBtn = [&](const char* icon, const char* id, const char* tip,
                    DrawingDimKind k) {
    CommandButton(state, icon, id, tip,
                  state.dimTool.active && state.dimTool.kind == k);
  };
  dimBtn(ICON_FA_LEFT_RIGHT "###tb_dwg_dist", "drawing.dim_distance",
         "Distance dimension", DrawingDimKind::Distance);
  dimBtn(ICON_FA_RULER_HORIZONTAL "###tb_dwg_distx", "drawing.dim_distance_x",
         "Horizontal (X) distance", DrawingDimKind::DistanceX);
  dimBtn(ICON_FA_UP_DOWN "###tb_dwg_disty", "drawing.dim_distance_y",
         "Vertical (Y) distance", DrawingDimKind::DistanceY);
  dimBtn(ICON_FA_CIRCLE_NOTCH "###tb_dwg_radius", "drawing.dim_radius",
         "Radius dimension", DrawingDimKind::Radius);
  dimBtn(ICON_FA_CIRCLE_HALF_STROKE "###tb_dwg_diameter",
         "drawing.dim_diameter", "Diameter dimension",
         DrawingDimKind::Diameter);
  dimBtn(ICON_FA_ANGLE_LEFT "###tb_dwg_angle", "drawing.dim_angle",
         "Angle dimension", DrawingDimKind::Angle);
  dimBtn(ICON_FA_BULLSEYE "###tb_dwg_hole_callout", "drawing.dim_hole_callout",
         "Hole callout (pick a drilled hole's circle)",
         DrawingDimKind::HoleCallout);
  dimBtn(ICON_FA_ELLIPSIS "###tb_dwg_pattern_callout",
         "drawing.dim_pattern_callout",
         "Pattern callout (pick 2+ circles, Enter finishes)",
         DrawingDimKind::PatternCallout);

  Divider();

  // `view.fit` knows a drawing tab has a sheet scale rather than a camera.
  CommandButton(state,
                ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_dwg_fit",
                "view.fit", "Fit sheet");
  // Export needs a native save dialog; without one (bare session, container)
  // the click used to do nothing at all. Say so on the button instead.
  CommandButton(state, ICON_FA_FILE_EXPORT "###tb_dwg_export", "drawing.export",
                FileDialogsAvailable()
                    ? "Export drawing (SVG / PDF)"
                    : "Export needs a system file dialog (unavailable here) - "
                      "use export_drawing over the script API");

  if (state.dimTool.active) {
    ImGui::SameLine();
    const int picked = static_cast<int>(state.dimTool.refs.size());
    if (state.dimTool.kind == DrawingDimKind::PatternCallout) {
      ImGui::TextDisabled("pick circles (%d so far) - Enter finishes, Esc "
                          "cancels",
                          picked);
    } else {
      const bool one = state.dimTool.kind == DrawingDimKind::Radius ||
                       state.dimTool.kind == DrawingDimKind::Diameter ||
                       state.dimTool.kind == DrawingDimKind::HoleCallout;
      ImGui::TextDisabled("pick edge %d of %d (Esc to cancel)", picked + 1,
                          one ? 1 : 2);
    }
  }
  ImGui::NewLine();
}

// The JOB toolbar: the CAM verbs, one button each, the way the part bar has
// one button per feature (docs/cam/gui.md, docs/cam/job-document.md). A job
// tab used to draw the PART bar -- thirty modelling tools, every one of them
// disabled, and the way into CAM a floating window listing the verbs. The
// verbs ARE the tools of this document kind; this is where a person looks
// for them. Each one is a command, so the keys, the palette and the tooltips
// agree with the bar (keys/registry_is_one_to_one).
//
// Ids are "###tb_job_*" so a test or an agent addresses the verb, not the
// glyph. `job_tab_has_its_own_toolbar` asserts the row FITS and that no two
// wear one glyph, the way the part bar's tests do.
void DrawJobToolbar(AppState& state) {
  const auto btn = [&state](const char* icon, const char* id, const char* tip,
                            bool active = false) {
    CommandButton(state, icon, id, tip, active);
  };
  const bool armed = state.slotPick.featureId == kCamToolSlot;
  const auto armedFor = [&](PickSlot slot) {
    return armed && state.slotPick.slot == static_cast<int>(slot);
  };

  // Set up: Z zero first, and loudly, because it is the number that breaks
  // tools; then how the part is clamped.
  btn(ICON_FA_CROSSHAIRS "###tb_job_top_face", "job.top_face",
      "Pick the top face: Z zero for the job (also lays the part flat on it)",
      armedFor(PickSlot::CamTopFace));
  btn(ICON_FA_HAND "###tb_job_lay_face_up", "job.lay_face_up",
      "Lay a face up: clamp the part with this face towards the spindle",
      armedFor(PickSlot::CamLayFaceUp));
  // Only on a machine that has the axis -- the command's gate says so, and a
  // disabled button for an axis you do not own is a question with no answer.
#ifdef HAVE_CAM
  if (const cam::Setup* setup = ActiveCamSetup(state);
      setup != nullptr && setup->machine.hasRotaryAxis)
    btn(ICON_FA_ROTATE "###tb_job_index_face", "job.index_face",
        "Turn a face up: index the rotary table so this face points at the "
        "spindle",
        armedFor(PickSlot::CamIndexFace));
#endif
  btn(ICON_FA_VIDEO "###tb_job_spindle", "job.look_down_spindle",
      "Look down the spindle: the plan view of this setup, framed on the "
      "stock");

  Divider();

  // Machine it: each verb is one click on the model. The lit one is armed.
  btn(ICON_FA_SQUARE_MINUS "###tb_job_pocket", "job.pocket",
      "Clear a pocket: click its floor", armedFor(PickSlot::CamPocketFloor));
  btn(ICON_FA_VECTOR_SQUARE "###tb_job_profile", "job.profile",
      "Cut it out: click the wall to follow",
      armedFor(PickSlot::CamProfileOutline));
  btn(ICON_FA_CIRCLE_DOT "###tb_job_drill", "job.drill",
      "Drill a hole: click inside it", armedFor(PickSlot::CamDrillHole));
  btn(ICON_FA_BORDER_TOP_LEFT "###tb_job_edge", "job.edge",
      "Break an edge: click the wall whose top edge to round or chamfer",
      armedFor(PickSlot::CamEdgeWall));
  btn(ICON_FA_SCISSORS "###tb_job_whole", "job.whole",
      "Cut out the whole part (no pick)");
  btn(ICON_FA_GRIP_LINES "###tb_job_face", "job.face",
      "Face the top: skim the material above the part (no pick)");

  Divider();

  btn(ICON_FA_GEARS "###tb_job_generate", "job.generate",
      "Generate every toolpath and verify the job");
  btn(ICON_FA_PLAY "###tb_job_simulate", "job.simulate",
      "Simulate the cut: play the posted file against the stock",
      state.camPlaybackOpen);

  Divider();

  btn(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_job_fit", "view.fit",
      "Fit view");
  CommandButton(state, ICON_FA_RULER "###tb_job_measure", "view.measure",
                "Measure", state.measure.active);
  ImGui::NewLine();
}

void DrawToolbar(AppState& state) {
  if (state.sketchEdit.IsEditing())
    DrawSketchModeToolbar(state);
  else if (state.simulation.IsActive())
    DrawSimulationToolbar(state);
  else if (state.assembly)
    DrawAssemblyToolbar(state);
  else if (state.drawing)
    DrawDrawingToolbar(state);
  else if (state.job)
    DrawJobToolbar(state);
  else
    DrawModelToolbar(state);
}

} // namespace ui
