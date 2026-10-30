// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/Commands.h"
#include "ui/AssemblyActions.h"
#include "ui/CamGenerate.h" // OpenCamGenerate
#include "ui/CamPanel.h"    // RefreshCamToolpaths
#include "ui/CamPlayback.h" // BeginCamPlayback
#include "ui/CamTool.h"     // StartCamVerb, the job verbs

#include "common/Logger.h"
#include "core/Conventions.h"
#include "core/Document.h"
#include "core/features/SketchFeature.h"
#include "render/Camera.h"
#include "render/RayTracingRenderer.h"
#include "ui/AlignTool.h"
#include "ui/BearingTool.h"
#include "ui/CreateActions.h"
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/DrawingAnnotate.h"
#include "ui/FastenerTool.h"
#include "ui/FeatureEditor.h" // kCamToolSlot (the CAM session's pick slot)
#include "ui/FileDialogs.h"
#include "ui/FitTool.h"
#include "ui/JobActions.h"
#include "ui/MeasureActions.h"
#include "ui/PreviewFeature.h"
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/TemplateActions.h"
#include "ui/TerminalPanel.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include "imgui.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ui {

namespace {

// keymap::Key -> ImGuiKey. The ONE place the two enums meet: core owns the
// binding vocabulary (it does not link ImGui), ui owns the translation.
ImGuiKey ToImGuiKey(keymap::Key k) {
  using K = keymap::Key;
  const auto v = static_cast<uint16_t>(k);
  if (k >= K::A && k <= K::Z)
    return static_cast<ImGuiKey>(ImGuiKey_A +
                                 (v - static_cast<uint16_t>(K::A)));
  if (k >= K::Num0 && k <= K::Num9)
    return static_cast<ImGuiKey>(ImGuiKey_0 +
                                 (v - static_cast<uint16_t>(K::Num0)));
  if (k >= K::F1 && k <= K::F12)
    return static_cast<ImGuiKey>(ImGuiKey_F1 +
                                 (v - static_cast<uint16_t>(K::F1)));
  switch (k) {
  case K::Space:
    return ImGuiKey_Space;
  case K::Tab:
    return ImGuiKey_Tab;
  case K::Comma:
    return ImGuiKey_Comma;
  case K::Period:
    return ImGuiKey_Period;
  case K::Slash:
    return ImGuiKey_Slash;
  case K::Semicolon:
    return ImGuiKey_Semicolon;
  case K::Apostrophe:
    return ImGuiKey_Apostrophe;
  case K::Minus:
    return ImGuiKey_Minus;
  case K::Equal:
    return ImGuiKey_Equal;
  case K::LeftBracket:
    return ImGuiKey_LeftBracket;
  case K::RightBracket:
    return ImGuiKey_RightBracket;
  case K::Backslash:
    return ImGuiKey_Backslash;
  case K::Grave:
    return ImGuiKey_GraveAccent;
  case K::Home:
    return ImGuiKey_Home;
  case K::End:
    return ImGuiKey_End;
  case K::PageUp:
    return ImGuiKey_PageUp;
  case K::PageDown:
    return ImGuiKey_PageDown;
  case K::Insert:
    return ImGuiKey_Insert;
  default:
    return ImGuiKey_None;
  }
}

// The inverse, for click-to-capture in Preferences.
keymap::Key FromImGuiKey(ImGuiKey k) {
  using K = keymap::Key;
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z)
    return static_cast<K>(static_cast<uint16_t>(K::A) + (k - ImGuiKey_A));
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9)
    return static_cast<K>(static_cast<uint16_t>(K::Num0) + (k - ImGuiKey_0));
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12)
    return static_cast<K>(static_cast<uint16_t>(K::F1) + (k - ImGuiKey_F1));
  switch (k) {
  case ImGuiKey_Space:
    return K::Space;
  case ImGuiKey_Tab:
    return K::Tab;
  case ImGuiKey_Comma:
    return K::Comma;
  case ImGuiKey_Period:
    return K::Period;
  case ImGuiKey_Slash:
    return K::Slash;
  case ImGuiKey_Semicolon:
    return K::Semicolon;
  case ImGuiKey_Apostrophe:
    return K::Apostrophe;
  case ImGuiKey_Minus:
    return K::Minus;
  case ImGuiKey_Equal:
    return K::Equal;
  case ImGuiKey_LeftBracket:
    return K::LeftBracket;
  case ImGuiKey_RightBracket:
    return K::RightBracket;
  case ImGuiKey_Backslash:
    return K::Backslash;
  case ImGuiKey_GraveAccent:
    return K::Grave;
  case ImGuiKey_Home:
    return K::Home;
  case ImGuiKey_End:
    return K::End;
  case ImGuiKey_PageUp:
    return K::PageUp;
  case ImGuiKey_PageDown:
    return K::PageDown;
  case ImGuiKey_Insert:
    return K::Insert;
  default:
    return K::None;
  }
}

ImGuiKeyChord ToImGuiChord(keymap::Chord c) {
  ImGuiKeyChord chord = ToImGuiKey(c.key);
  if (c.mods & keymap::ModCtrl)
    chord |= ImGuiMod_Ctrl;
  if (c.mods & keymap::ModShift)
    chord |= ImGuiMod_Shift;
  if (c.mods & keymap::ModAlt)
    chord |= ImGuiMod_Alt;
  return chord;
}

// Did this chord fire this frame?
//
// Two paths, and the split is the behaviour the app already had. A Ctrl chord
// goes through ImGui's routing, so a focused text field keeps its own Ctrl+Z /
// Ctrl+C. A bare or Shift-only chord is a plain key test gated on
// WantTextInput, and it must match the modifier state EXACTLY — without that,
// `S` would also fire on Shift+S and arm two tools at once.
bool Pressed(keymap::Chord c) {
  if (!c)
    return false;
  const ImGuiIO& io = ImGui::GetIO();
  if (c.mods & keymap::ModCtrl)
    return ImGui::Shortcut(ToImGuiChord(c), ImGuiInputFlags_RouteGlobal);
  if (io.WantTextInput || io.KeyCtrl || io.KeySuper)
    return false;
  if (((c.mods & keymap::ModShift) != 0) != io.KeyShift)
    return false;
  if (((c.mods & keymap::ModAlt) != 0) != io.KeyAlt)
    return false;
  return ImGui::IsKeyPressed(ToImGuiKey(c.key), /*repeat=*/false);
}

// ---------------------------------------------------------------- gates

bool HasDocument(AppState& s) { return s.document != nullptr; }
// "A part tab, and no feature preview in flight" — creation is disabled
// mid-preview everywhere, exactly as the toolbar had it.
bool Idle(AppState& s) { return s.document && !HasPendingFeature(s); }
bool HasBody(AppState& s) {
  return s.document && !s.document->GetBodies().empty();
}
bool IdleBody(AppState& s) { return Idle(s) && HasBody(s); }
bool HasJob(AppState& s) {
#ifdef HAVE_CAM
  return s.job != nullptr && !JobBodies(s).empty();
#else
  (void)s;
  return false;
#endif
}
bool HasRotaryAxis(AppState& s) {
#ifdef HAVE_CAM
  if (!HasJob(s))
    return false;
  const cam::Setup* setup = ActiveCamSetup(s);
  return setup != nullptr && setup->machine.hasRotaryAxis;
#else
  (void)s;
  return false;
#endif
}
bool HasProfile(AppState& s) { return Idle(s) && ClosedProfileSketch(s); }
size_t Producers(AppState& s) {
  return s.document
             ? BodyProducers(*s.document, s.document->FeatureCount()).size()
             : 0;
}
bool HasStudy(AppState& s) {
  return s.document && s.document->FindStudy(s.simulation.activeStudyId);
}
// Cheap and side-effect free: ResolveActiveSketch can EXIT edit mode, which
// is not something a tooltip or a palette row should be able to do.
bool Sketching(AppState& s) { return s.sketchEdit.IsEditing(); }
bool SketchHasSelection(AppState& s) {
  return Sketching(s) && !s.sketchEdit.selected.empty();
}

// ------------------------------------------------------------- helpers

void ArmSketchTool(AppState& s, SketchTool t) { SetSketchTool(s, t); }

void Constrain(AppState& s, sketch::ConstraintKind kind) {
  ApplySketchConstraint(s, kind, 0.0);
}

// "view.front", "view.top", ... — static storage for Command::id, which
// borrows the pointer rather than owning a string.
static const std::array<std::string, std::size(conv::kNamedViews)>
    kViewCommandIds = [] {
      std::array<std::string, std::size(conv::kNamedViews)> a;
      for (size_t i = 0; i < a.size(); i++)
        a[i] = std::string("view.") + conv::kNamedViews[i].name;
      return a;
    }();

void StandardView(AppState& s, const conv::NamedView& v) {
  // The camera's OWN pivot, not the world origin: a standard view is an
  // orientation, not a place (docs/ui/README.md#viewport). The table's up
  // matters for top/bottom: before it was passed through, the GUI's top view
  // and the RPC's were rolled 180 degrees from each other.
  if (s.camera)
    s.camera->AlignToFace(s.camera->GetTarget(), glm::vec3(v.toCamera), 0.0f,
                          glm::vec3(v.up));
}

void ToggleMeasure(AppState& s) {
  const bool wasActive = s.measure.active;
  s.measure = MeasureState{};
  s.measure.active = !wasActive;
  // Select first, then measure — the U7b behaviour every other tool has had
  // for a year, via SeedSlotsFromSelection on entry. Measure was the one
  // that DISCARDED the selection instead of consuming it, so selecting two
  // faces and hitting Ctrl+Shift+M made you click both again.
  //
  // U1's premise is one typed, ordered selection set that every context
  // reads and writes; measure kept a private list beside it. Seeding is the
  // first half of closing that: the picks still live in MeasureState (they
  // carry an occurrence identity the selection set models differently), but
  // the SET is now where a measurement starts.
  if (s.measure.active)
    SeedMeasureFromSelection(s);
}

void ToggleSection(AppState& s) {
  s.sectionToolOpen = !s.sectionToolOpen;
  // A tool window with an empty list is a dead end: seed a cut through the
  // middle of whatever this tab shows.
  if (s.sectionToolOpen && s.sections.empty())
    AddSection(s, 0);
}

void RequestPopup(AppState& s, const char* name) { s.popupRequest = name; }

void ArmDimTool(AppState& s, DrawingDimKind kind) {
  EndDrawingTools(s); // one drawing session at a time
  s.dimTool.active = true;
  s.dimTool.kind = kind;
}

// Fit means something different per tab, and the one `F` has to mean the
// right one: a drawing tab has no 3D camera to fit, it has a sheet scale.
void FitActiveTab(AppState& s) {
  if (s.drawing) {
    s.sheetView.scale = 0.0; // refit on next paint
    return;
  }
  FitCameraToDocument(s);
}

void AlignToPickedFace(AppState& s) {
  if (!s.faceSelector || !s.camera)
    return;
  uint32_t mesh = 0, index = 0;
  bool have = false;
  if (const SelectionItem* it = s.selection.First(SelectionItem::Kind::Face))
    have = ResolveFaceItem(s, *it, mesh, index);
  // Pointing at a SECTION CAP: look straight at the cut. The cap is not a
  // B-Rep face, so the hovered face there is at best the wall the ray
  // entered through (an assembly) and at worst nothing (a part) — N turned
  // to a hidden wall, or did nothing at all.
  if (!have && s.pick.hoverCap) {
    s.camera->AlignToFace(s.pick.hoverCapPoint, s.pick.hoverCapNormal);
    return;
  }
  if (!have) {
    if (const Face* hovered = s.faceSelector->GetHoveredFace()) {
      mesh = hovered->meshId;
      index = hovered->faceIndex;
      have = true;
    }
  }
  if (have)
    s.camera->AlignToFace(s.faceSelector->GetFaceCenter(mesh, index),
                          s.faceSelector->GetFaceNormal(mesh, index));
}

void RunActiveStudy(AppState& s) {
  Document* doc = s.document.get();
  const fem::Study* study =
      doc ? doc->FindStudy(s.simulation.activeStudyId) : nullptr;
  if (!study)
    return;
  std::string error;
  const bool ok =
      study->type == fem::AnalysisType::Topology
          ? StartTopologyRun(s, *doc, s.simulation.activeStudyId, &error)
          : StartStudyRun(s, *doc, s.simulation.activeStudyId, &error);
  if (!ok)
    LOG_WARN("run study failed: {}", error);
}

const std::vector<Command>& Table() {
  static const std::vector<Command> kCommands = [] {
    std::vector<Command> c;
    const auto add = [&c](const char* id, std::function<bool(AppState&)> gate,
                          std::function<void(AppState&)> run) {
      c.push_back({id, std::move(gate), std::move(run)});
    };

    // ------------------------------------------------------------ Global
    // The WHOLE of File > New, not just the document swap. Ctrl+N used to do
    // only the swap, so a keyboard New left the old part tabs, tab folders and
    // section cuts in place while the menu's New cleared them — two commands
    // wearing one name.
    add("file.new", nullptr, [](AppState& s) {
      ReplaceDocument(s, std::make_unique<Document>());
      s.parts.clear();
      s.tabFolders.clear();
      ClearSections(s); // a new project starts uncut
      EnsureParts(s, "Part 1");
      MarkProjectOpened(s, ""); // a fresh document has no home yet
    });
    add(
        "file.open", [](AppState&) { return FileDialogsAvailable(); },
        [](AppState& s) { OpenProjectDialog(s); });
    add(
        "file.save", [](AppState&) { return FileDialogsAvailable(); },
        [](AppState& s) { SaveProjectAction(s); });
    add(
        "file.save_as", [](AppState&) { return FileDialogsAvailable(); },
        [](AppState& s) { SaveProjectDialog(s); });
    add("file.import", nullptr, [](AppState& s) { ImportFileDialog(s); });
    // A part with bodies, an assembly (visual export walks its occurrences),
    // or a drawing with views — and only where a system save dialog exists.
    add(
        "file.export",
        [](AppState& s) {
          const bool anything =
              HasBody(s) || (s.assembly && s.assembly->OccurrenceCount() > 0) ||
              (s.drawing && !s.drawing->GetViews().empty());
          return anything && FileDialogsAvailable();
        },
        [](AppState& s) { ExportDialog(s); });

    add(
        "edit.undo", [](AppState& s) { return CanGlobalUndo(s); },
        [](AppState& s) { GlobalUndo(s); });
    add(
        "edit.redo", [](AppState& s) { return CanGlobalRedo(s); },
        [](AppState& s) { GlobalRedo(s); });
    add("edit.preferences", nullptr,
        [](AppState& s) { s.settingsWindowOpen = true; });
    add("palette.open", nullptr,
        [](AppState& s) { s.paletteOpen = !s.paletteOpen; });
    add("help.shortcuts", nullptr,
        [](AppState& s) { s.showShortcuts = !s.showShortcuts; });

    add("view.fit", nullptr, FitActiveTab);
    add("view.zoom_selection", nullptr,
        [](AppState& s) { FitCameraToSelection(s); });
    add("view.align_to_face", nullptr, AlignToPickedFace);
    add("view.toggle_planes", HasDocument,
        [](AppState& s) { s.showPlanes = !s.showPlanes; });
    // One command per named view, straight from the conventions table — the
    // same directions the `view` RPC and the drawings use. Ids live in
    // kViewCommandIds because Command::id is a borrowed const char*.
    for (size_t i = 0; i < std::size(conv::kNamedViews); i++) {
      const conv::NamedView& v = conv::kNamedViews[i];
      add(kViewCommandIds[i].c_str(), nullptr,
          [&v](AppState& s) { StandardView(s, v); });
    }
    add("view.reset_layout", nullptr,
        [](AppState& s) { s.buildDefaultDockLayout = true; });
    add("view.controls_help", nullptr,
        [](AppState& s) { s.showControlsHelp = !s.showControlsHelp; });
    add(
        "view.measure",
        [](AppState& s) {
          return HasBody(s) || HasJob(s) ||
                 (s.assembly && s.assembly->OccurrenceCount());
        },
        ToggleMeasure);
    add("view.section", nullptr, ToggleSection);
    add(
        "view.render",
        [](AppState& s) {
          // A quick render needs a renderer, not ray tracing, so the dialog
          // opens wherever either kind of still can be made.
          const bool ready = (s.rayTracer && s.rayTracer->IsInitialized()) ||
                             s.captureRaster != nullptr;
          return ready &&
                 (HasBody(s) || (s.assembly && s.assembly->OccurrenceCount()));
        },
        [](AppState& s) { RequestPopup(s, "Render Still"); });
    add("view.perf_hud", nullptr,
        [](AppState& s) { s.perfHudOpen = !s.perfHudOpen; });

    // -------------------------------------------------------------- Part
    add("part.new_sketch", Idle, [](AppState& s) { StartNewSketch(s); });
    add("part.extrude", HasProfile, [](AppState& s) {
      if (SketchFeature* sk = ClosedProfileSketch(s))
        BeginExtrudeOfSketch(s, *sk);
    });
    add("part.revolve", HasProfile, [](AppState& s) {
      if (SketchFeature* sk = ClosedProfileSketch(s))
        BeginRevolveOfSketch(s, *sk);
    });
    add(
        "part.sweep",
        [](AppState& s) {
          return Idle(s) && SketchFeatureCount(*s.document) >= 1;
        },
        [](AppState& s) { CreateSweep(s); });
    add(
        "part.loft",
        [](AppState& s) {
          return Idle(s) && SketchFeatureCount(*s.document) >= 2;
        },
        [](AppState& s) { CreateLoft(s); });
    add(
        "part.pipe",
        [](AppState& s) { return Idle(s) && HasCurveOrSketch(*s.document); },
        [](AppState& s) { CreatePipe(s); });
    add("part.curve3d", Idle, [](AppState& s) { CreateCurve3D(s); });
    add("part.box", Idle, [](AppState& s) { CreateBox(s); });
    add("part.sphere", Idle, [](AppState& s) { CreateSphere(s); });
    add("part.cylinder", Idle, [](AppState& s) { CreateCylinder(s); });
    add("part.datum_plane", Idle, [](AppState& s) { CreateDatumPlane(s); });
    add("part.datum_axis", Idle, [](AppState& s) { CreateDatumAxis(s); });
    add("part.frame", Idle, [](AppState& s) { CreateFrame(s); });
    // A copy of another part to draw on (docs/modeling/derived.md).
    // Enabled on any part tab: with nothing to derive from the window says
    // so and Accept stays disabled (U10), the same rule as the fastener.
    add("part.derive", Idle, [](AppState& s) { CreateDerived(s); });
    // A template from the project's library, standing on a picked face or
    // plane (docs/modeling/templates.md#in-the-gui). Enabled on any part
    // tab: with an empty library the pick's window says so.
    add("part.template", Idle, [](AppState& s) { CreateTemplateInstance(s); });

    add("part.fillet", IdleBody, [](AppState& s) { CreateFillet(s); });
    add("part.chamfer", IdleBody, [](AppState& s) { CreateChamfer(s); });
    add("part.hole", IdleBody, [](AppState& s) { StartHoleTool(s); });
    add("part.thread", IdleBody, [](AppState& s) { CreateThread(s); });
    add("part.worm", IdleBody, [](AppState& s) { CreateWorm(s); });
    add("part.worm_wheel", IdleBody, [](AppState& s) { CreateWormWheel(s); });
    add("part.cam_profile", IdleBody, [](AppState& s) { CreateCamProfile(s); });
    add("part.draft", IdleBody, [](AppState& s) { CreateDraft(s); });
    add("part.shell", IdleBody, [](AppState& s) { CreateShell(s); });
    add("part.delete_face", IdleBody, [](AppState& s) { CreateDeleteFace(s); });
    add("part.offset_face", IdleBody, [](AppState& s) { CreateOffsetFace(s); });
    add("part.move_face", IdleBody, [](AppState& s) { CreateMoveFace(s); });
    add("part.replace_face", IdleBody,
        [](AppState& s) { CreateReplaceFace(s); });
    add("part.rib", IdleBody, [](AppState& s) { CreateRib(s); });
    add(
        "part.boolean",
        [](AppState& s) { return Idle(s) && Producers(s) >= 2; },
        [](AppState& s) { CreateBoolean(s); });
    add(
        "part.cradle", [](AppState& s) { return Idle(s) && Producers(s) >= 2; },
        [](AppState& s) { CreateCradle(s); });
    add(
        "part.align",
        [](AppState& s) { return Idle(s) && HasImportToAlign(s); },
        [](AppState& s) { OpenAlignTool(s); });
    add(
        "part.fit", [](AppState& s) { return Idle(s) && Producers(s) >= 1; },
        [](AppState& s) { OpenFitTool(s); });
    add(
        "part.mirror", [](AppState& s) { return Idle(s) && Producers(s) >= 1; },
        [](AppState& s) { CreateMirror(s); });
    add(
        "part.pattern",
        [](AppState& s) { return Idle(s) && Producers(s) >= 1; },
        [](AppState& s) { CreatePattern(s); });
    add("part.split", IdleBody, [](AppState& s) { CreateSplit(s); });
    add("part.refine", IdleBody, [](AppState& s) { CreateRefine(s); });

    // Sheet metal (docs/sheetmetal/README.md). The base flange needs a closed
    // profile like an extrude does; the other two need a body to grow off or
    // to lay flat. None of them needs the rules to be SET — each refuses
    // visibly and names set_sheet_metal_rules, which is more use than a
    // greyed-out button that says nothing.
    add("part.sheet_base_flange", HasProfile,
        [](AppState& s) { CreateBaseFlange(s); });
    add("part.sheet_edge_flange", IdleBody,
        [](AppState& s) { CreateEdgeFlange(s); });
    add("part.sheet_flat_pattern", IdleBody,
        [](AppState& s) { CreateFlatPattern(s); });
    add("part.sheet_unfold", IdleBody, [](AppState& s) { CreateUnfold(s); });
    add("part.sheet_refold", Idle, [](AppState& s) { CreateRefold(s); });
    add("part.sheet_hem", IdleBody, [](AppState& s) { CreateHem(s); });
    add("part.sheet_jog", Idle, [](AppState& s) { CreateJog(s); });
    add("part.sheet_corner_relief", IdleBody,
        [](AppState& s) { CreateCornerRelief(s); });
    add("part.sheet_normal_cut", IdleBody,
        [](AppState& s) { CreateNormalCut(s); });
    add("part.simulation", IdleBody, [](AppState& s) { EnterSimulation(s); });

    // ------------------------------------------------------------- Job (CAM)
    // The verbs, one command each (docs/cam/gui.md). Each opens ITS window
    // and arms its pick; the two that need no pick make the cut at once.
    const auto verb = [&add](const char* id, AppState::CamVerb v,
                             std::function<bool(AppState&)> gate = HasJob) {
      add(id, std::move(gate), [v](AppState& s) { StartCamVerb(s, v); });
    };
    verb("job.top_face", AppState::CamVerb::TopFace);
    verb("job.lay_face_up", AppState::CamVerb::LayFaceUp);
    // The ROTARY index is a different claim and only true of a machine that
    // has the axis. Offering it unconditionally is what let a 3-axis job be
    // handed `G0 A90` for wanting a panel flat
    // (docs/cam/setup.md#fixturing-frame-and-rotary-index).
    verb("job.index_face", AppState::CamVerb::IndexFace, HasRotaryAxis);
    add("job.look_down_spindle", HasJob,
        [](AppState& s) { LookDownTheSpindle(s); });
    verb("job.pocket", AppState::CamVerb::Pocket);
    verb("job.profile", AppState::CamVerb::Profile);
    verb("job.drill", AppState::CamVerb::Drill);
    verb("job.edge", AppState::CamVerb::Edge);
    verb("job.engrave", AppState::CamVerb::Engrave);
    verb("job.whole", AppState::CamVerb::Whole);
    verb("job.face", AppState::CamVerb::Face);
    // 3D and 4D finishing (docs/cam/gui.md#the-job-toolbar). The two surface
    // verbs were written, with their picks and derivations, and never given
    // a command; the rotary ones only mean something with the axis.
    verb("job.surface", AppState::CamVerb::Surface);
    verb("job.walls", AppState::CamVerb::Walls);
    verb("job.rotary_finish", AppState::CamVerb::RotaryFinish, HasRotaryAxis);
    verb("job.rotary_rough", AppState::CamVerb::RotaryRough, HasRotaryAxis);
    // Rough a 3D part, and clean up after the finish with a smaller ball
    // (docs/cam/operations.md#rough-a-surface-and-rest-machining).
    verb("job.surface_rough", AppState::CamVerb::SurfaceRough);
    verb("job.rest", AppState::CamVerb::Rest);
    // A tool with a window, like Simulate: the run and what it found, where
    // the button was pressed. It used to generate silently and put the
    // verdict in the docked panel, below the operations list.
    add("job.generate", HasJob,
        [](AppState& s) { OpenCamGenerate(s, /*regenerate=*/true); });
    // Refuses in words when there is nothing it can play -- the u10 rule --
    // and says so AT THE CLICK. The words used to go only into the CAM panel,
    // below a screenful of verdict lines and out of sight of the toolbar
    // button that was pressed; from there a refusal is indistinguishable from
    // a button that does nothing.
    add("job.simulate", HasJob, [](AppState& s) {
#ifdef HAVE_CAM
      if (!BeginCamPlayback(s))
        s.camRefusalPopup = true;
#else
      (void)s;
#endif
    });

    // ------------------------------------------------------------ Sketch
    const auto tool = [&add](const char* id, SketchTool t) {
      add(id, Sketching, [t](AppState& s) { ArmSketchTool(s, t); });
    };
    tool("sketch.line", SketchTool::Line);
    tool("sketch.circle", SketchTool::Circle);
    tool("sketch.ellipse", SketchTool::Ellipse);
    tool("sketch.rectangle", SketchTool::Rectangle);
    tool("sketch.rectangle_center", SketchTool::RectangleCenter);
    tool("sketch.arc_3point", SketchTool::ArcThreePoint);
    tool("sketch.arc_center", SketchTool::Arc);
    tool("sketch.ellipse_arc", SketchTool::EllipseArc);
    tool("sketch.spline", SketchTool::Spline);
    tool("sketch.point", SketchTool::Point);
    tool("sketch.text", SketchTool::Text);
    tool("sketch.project", SketchTool::Project);
    tool("sketch.trim", SketchTool::Trim);
    tool("sketch.offset", SketchTool::Offset);
    tool("sketch.fillet", SketchTool::Fillet);
    tool("sketch.mirror", SketchTool::Mirror);
    tool("sketch.pattern", SketchTool::Pattern);
    tool("sketch.dimension", SketchTool::Dimension);
    // More than "open the dialog": the fresh image's session (opacity /
    // calibrate / delete) opens in the Tool window, which means arming the
    // Image tool on the image that was just added. OpenReferenceImageDialog
    // commits and REPLACES the sketch feature, so the count has to be taken
    // before and the sketch re-resolved after.
    add("sketch.image", Sketching, [](AppState& s) {
      const SketchFeature* before = ResolveActiveSketch(s);
      const size_t had = before != nullptr ? before->images.size() : 0;
      OpenReferenceImageDialog(s);
      SketchFeature* after = ResolveActiveSketch(s);
      if (after != nullptr && after->images.size() > had) {
        SetSketchTool(s, SketchTool::Image);
        s.sketchEdit.selectedImageId = after->images.back().id;
      }
    });

    add("sketch.toggle_construction", SketchHasSelection,
        [](AppState& s) { ToggleConstruction(s); });
    add("sketch.clear_selection", Sketching,
        [](AppState& s) { s.sketchEdit.selected.clear(); });
    add("sketch.copy", SketchHasSelection, [](AppState& s) {
      if (SketchFeature* sk = ResolveActiveSketch(s))
        CopySketchSelection(s, *sk);
    });
    add(
        "sketch.paste",
        [](AppState& s) { return Sketching(s) && !s.sketchClipboard.empty(); },
        [](AppState& s) {
          size_t index = 0;
          if (SketchFeature* sk = ResolveActiveSketch(s, &index))
            PasteSketchClipboard(s, *sk, index);
        });
    add(
        "sketch.paste_linked",
        [](AppState& s) {
          return Sketching(s) && !s.sketchClipboardIds.empty();
        },
        [](AppState& s) {
          size_t index = 0;
          if (SketchFeature* sk = ResolveActiveSketch(s, &index))
            PasteSketchClipboardLinked(s, *sk, index);
        });

    using K = sketch::ConstraintKind;
    const auto constraint = [&add](const char* id, K kind) {
      add(id, SketchHasSelection, [kind](AppState& s) { Constrain(s, kind); });
    };
    constraint("sketch.constraint_horizontal", K::Horizontal);
    constraint("sketch.constraint_vertical", K::Vertical);
    constraint("sketch.constraint_coincident", K::Coincident);
    constraint("sketch.constraint_parallel", K::Parallel);
    constraint("sketch.constraint_perpendicular", K::Perpendicular);
    constraint("sketch.constraint_tangent", K::Tangent);
    constraint("sketch.constraint_curvature", K::Curvature);
    constraint("sketch.constraint_equal", K::Equal);
    constraint("sketch.constraint_concentric", K::Concentric);
    constraint("sketch.constraint_point_on_line", K::PointOnLine);
    constraint("sketch.constraint_symmetric", K::Symmetric);
    constraint("sketch.constraint_midpoint", K::Midpoint);
    constraint("sketch.constraint_fixed", K::Fixed);
    constraint("sketch.constraint_collinear", K::Collinear);

    add("sketch.toggle_autoconstrain", Sketching, [](AppState& s) {
      s.sketchEdit.autoConstrain = !s.sketchEdit.autoConstrain;
    });
    add("sketch.toggle_glyphs", Sketching, [](AppState& s) {
      s.sketchEdit.showConstraintGlyphs = !s.sketchEdit.showConstraintGlyphs;
    });
    // Enabled while it is ON even with no slot free, so a slice can always be
    // switched off; turning it ON needs a clip-plane slot, and the toolbar
    // tooltip says which of the two reasons a greyed button has.
    add(
        "sketch.toggle_slice",
        [](AppState& s) {
          return s.sketchEdit.activeSketchId != 0 &&
                 (s.sketchSlice || CanSketchSlice(s));
        },
        [](AppState& s) { s.sketchSlice = !s.sketchSlice; });
    add("sketch.extrude", Sketching,
        [](AppState& s) { BeginSketchExtrude(s); });
    add("sketch.revolve", Sketching,
        [](AppState& s) { BeginSketchRevolve(s); });
    add("sketch.hole", CanBeginSketchHole,
        [](AppState& s) { BeginSketchHole(s); });

    // ---------------------------------------------------------- Assembly
    add("assembly.insert", nullptr,
        [](AppState& s) { RequestPopup(s, "insert_occurrence"); });
    const auto mate = [&add](const char* id, MateType t) {
      add(
          id, [](AppState& s) { return s.assembly != nullptr; },
          [t](AppState& s) { StartMateTool(s, t); });
    };
    mate("assembly.mate_fastened", MateType::Fastened);
    mate("assembly.mate_revolute", MateType::Revolute);
    mate("assembly.mate_slider", MateType::Slider);
    mate("assembly.mate_cylindrical", MateType::Cylindrical);
    mate("assembly.mate_planar", MateType::Planar);
    mate("assembly.mate_ball", MateType::Ball);
    // Standard parts (docs/parts/README.md). Enabled on any assembly tab,
    // including an empty one: opening it with nothing to pick is not a
    // mistake to prevent, it is the tool asking for its one input, and the
    // window says so with Place disabled (U10).
    add(
        "assembly.fastener", [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) { OpenFastenerTool(s); });
    // docs/parts/README.md#bearings — the same shape as the fastener: a
    // tool session that opens asking for its pick.
    add(
        "assembly.bearing", [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) { OpenBearingTool(s); });

    // The two viewport modes (docs/ui/panels.md#assembly-tab-the-model-panel).
    // Each replaces the other rather than stacking: they both place the
    // occurrences, so the tool window's one-at-a-time slot is the behaviour,
    // not a limitation.
    add(
        "assembly.explode", [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) {
          s.explodeToolOpen = !s.explodeToolOpen;
          if (s.explodeToolOpen)
            s.motionToolOpen = false;
        });
    add(
        "assembly.simulation",
        [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) {
          // The third floating viewport mode (S1): a study on the assembly.
          if (s.simulationToolOpen)
            ExitSimulation(s);
          else
            EnterSimulation(s);
        });
    add(
        "assembly.bom", [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) { s.bomWindowOpen = !s.bomWindowOpen; });
    add(
        "assembly.motion", [](AppState& s) { return s.assembly != nullptr; },
        [](AppState& s) {
          s.motionToolOpen = !s.motionToolOpen;
          if (s.motionToolOpen)
            s.explodeToolOpen = false;
        });

    // Hide needs something picked — from the list or from the viewport, which
    // both land in the same selection. Show all is offered only when there is
    // something hidden to show, so the palette does not advertise a no-op.
    add(
        "assembly.hide",
        [](AppState& s) {
          return s.assembly != nullptr && !SelectedOccurrenceIds(s).empty();
        },
        [](AppState& s) { ToggleSelectedOccurrencesVisible(s); });
    add(
        "assembly.show_all",
        [](AppState& s) {
          if (s.assembly == nullptr)
            return false;
          for (const Occurrence& o : s.assembly->GetOccurrences())
            if (!o.visible)
              return true;
          return false;
        },
        [](AppState& s) { ShowAllOccurrences(s); });

    // ----------------------------------------------------------- Drawing
    add("drawing.add_view", nullptr,
        [](AppState& s) { s.drawingAddViewOpen = !s.drawingAddViewOpen; });
    add("drawing.update", nullptr,
        [](AppState& s) { RequestDrawingUpdate(s); });
    add(
        "drawing.export",
        [](AppState& s) {
          return s.drawing && !s.drawing->GetViews().empty() &&
                 FileDialogsAvailable();
        },
        [](AppState& s) { ExportDialog(s); });
    const auto dim = [&add](const char* id, DrawingDimKind kind) {
      add(
          id, [](AppState& s) { return s.drawing != nullptr; },
          [kind](AppState& s) { ArmDimTool(s, kind); });
    };
    dim("drawing.dim_distance", DrawingDimKind::Distance);
    dim("drawing.dim_distance_x", DrawingDimKind::DistanceX);
    dim("drawing.dim_distance_y", DrawingDimKind::DistanceY);
    dim("drawing.dim_radius", DrawingDimKind::Radius);
    dim("drawing.dim_diameter", DrawingDimKind::Diameter);
    dim("drawing.dim_angle", DrawingDimKind::Angle);
    dim("drawing.dim_hole_callout", DrawingDimKind::HoleCallout);
    dim("drawing.dim_pattern_callout", DrawingDimKind::PatternCallout);
    dim("drawing.dim_part_note", DrawingDimKind::FastenerCallout);
    // The annotations a dimension is not (docs/drawings/guide.md#in-the-gui).
    const auto hasViews = [](AppState& s) {
      return s.drawing != nullptr && !s.drawing->GetViews().empty();
    };
    add("drawing.fcf", hasViews, [](AppState& s) {
      ArmDrawingNoteTool(s, AppState::DrawingNoteTool::Kind::Fcf);
    });
    add("drawing.datum", hasViews, [](AppState& s) {
      ArmDrawingNoteTool(s, AppState::DrawingNoteTool::Kind::Datum);
    });
    add("drawing.detail", hasViews, [](AppState& s) { ArmDetailTool(s); });
    add(
        "drawing.balloons",
        [](AppState& s) {
          // Balloons stand on the landed geometry: not while a view they
          // go round is still projecting.
          const std::vector<uint64_t> views = BalloonViews(s);
          if (views.empty())
            return false;
          for (uint64_t id : views)
            if (DrawingViewProjecting(s, id))
              return false;
          return true;
        },
        [](AppState& s) { LayOutBalloons(s, BalloonViews(s)); });

    // -------------------------------------------------------- Simulation
    const auto bc = [&add](const char* id, SimulationState::Tool t) {
      add(id, HasStudy, [t](AppState& s) {
        // The toolbar button toggles: pressing the armed tool's key again
        // disarms it, the same as clicking it again.
        SimulationArmTool(
            s, s.simulation.tool == t ? SimulationState::Tool::None : t);
      });
    };
    bc("sim.fixed", SimulationState::Tool::Fixed);
    bc("sim.force", SimulationState::Tool::Force);
    bc("sim.pressure", SimulationState::Tool::Pressure);
    add("sim.run", HasStudy, RunActiveStudy);

    return c;
  }();
  return kCommands;
}

} // namespace

std::span<const Command> Commands() { return Table(); }

bool TakePopupRequest(AppState& state, const char* name) {
  if (state.popupRequest != name)
    return false;
  state.popupRequest.clear();
  return true;
}

const Command* FindCommand(std::string_view id) {
  for (const Command& c : Table())
    if (id == c.id)
      return &c;
  return nullptr;
}

keymap::Context ActiveContext(const AppState& state) {
  // Exactly DrawToolbar's branch, in the same order. If these two ever
  // disagree, the toolbar shows one mode's buttons while the keys run
  // another mode's commands — which is the class of bug this whole table
  // exists to make impossible.
  if (state.sketchEdit.IsEditing())
    return keymap::Context::Sketch;
  if (state.simulation.IsActive())
    return keymap::Context::Simulation;
  if (state.assembly)
    return keymap::Context::Assembly;
  if (state.drawing)
    return keymap::Context::Drawing;
  if (state.job)
    return keymap::Context::Job;
  return keymap::Context::Part;
}

bool CommandEnabled(AppState& state, std::string_view id) {
  const Command* c = FindCommand(id);
  return c != nullptr && (!c->enabled || c->enabled(state));
}

bool RunCommand(AppState& state, std::string_view id) {
  const Command* c = FindCommand(id);
  if (c == nullptr || (c->enabled && !c->enabled(state)))
    return false;
  c->run(state);
  return true;
}

std::string CommandChord(const AppState& state, std::string_view id) {
  return state.settings.keys.Text(id);
}

std::string WithChord(const AppState& state, std::string_view id,
                      const char* tooltip) {
  const std::string chord = CommandChord(state, id);
  if (chord.empty())
    return tooltip;
  return std::string(tooltip) + " (" + chord + ")";
}

keymap::Chord CaptureChord() {
  const ImGuiIO& io = ImGui::GetIO();
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
    const auto key = static_cast<ImGuiKey>(k);
    if (!ImGui::IsKeyPressed(key, /*repeat=*/false))
      continue;
    // A modifier alone is not a chord — the user is on their way to one — and
    // FromImGuiKey maps every modifier to None, so this drops them.
    const keymap::Key mapped = FromImGuiKey(key);
    if (mapped == keymap::Key::None)
      continue;
    {
      keymap::Chord c;
      c.key = mapped;
      if (io.KeyCtrl)
        c.mods |= keymap::ModCtrl;
      if (io.KeyShift)
        c.mods |= keymap::ModShift;
      if (io.KeyAlt)
        c.mods |= keymap::ModAlt;
      return c;
    }
  }
  return {};
}

void DispatchShortcuts(AppState& state) {
  // Preferences > Keyboard is waiting for a chord: every key means "bind me"
  // until it is done, so nothing may also RUN. Without this, capturing `S`
  // would arm the sketch tool behind the window you are typing into.
  if (!state.keyCaptureId.empty())
    return;

  // The terminal owns the keyboard while the pointer is over it
  // (docs/ui/terminal.md). Without this gate Ctrl+Z would mean two
  // things at once — undo a feature AND suspend the job — and which one you
  // got would depend on invisible focus state.
  if (TerminalCapturesKeys(state))
    return;

  const keymap::Keymap& keys = state.settings.keys;

  // Preferences works mid-preview: opening a window changes nothing about the
  // pending feature, and it is the one route to the keymap that must never be
  // unreachable (you may be here to fix a binding that is in your way).
  if (Pressed(keys.Get("edit.preferences")))
    RunCommand(state, "edit.preferences");

  // While a feature preview is pending the undo stack is frozen: undo's chord
  // cancels the preview instead of touching history, and nothing else fires.
  if (HasPendingFeature(state)) {
    if (Pressed(keys.Get("edit.undo")) || Pressed(keys.Alt("edit.undo")))
      CancelPendingFeature(state);
    return;
  }
  // The Align tool's preview is off the stack in the same way: undo's chord
  // puts the import back and closes it, and history is not touched.
  if (state.alignTool.open &&
      (Pressed(keys.Get("edit.undo")) || Pressed(keys.Alt("edit.undo")))) {
    CancelAlignTool(state);
    return;
  }
  if (state.fitTool.open &&
      (Pressed(keys.Get("edit.undo")) || Pressed(keys.Alt("edit.undo")))) {
    CancelFitTool(state);
    return;
  }

  const keymap::Context ctx = ActiveContext(state);

  // Context bindings first, and they SHADOW a global with the same chord —
  // `N` is Coincident while sketching and align-to-face everywhere else. The
  // shadow is by chord, not by id, so a rebind moves it with no other change.
  std::vector<keymap::Chord> claimed;
  for (const keymap::Binding& b : keymap::Defaults()) {
    if (b.ctx != ctx)
      continue;
    const keymap::Chord chord = keys.Get(b.id);
    if (!chord)
      continue;
    claimed.push_back(chord);
    if (Pressed(chord))
      RunCommand(state, b.id);
  }

  for (const keymap::Binding& b : keymap::Defaults()) {
    if (b.ctx != keymap::Context::Global)
      continue;
    if (std::string_view(b.id) == "edit.preferences")
      continue; // already handled above, before the pending-preview gate
    const keymap::Chord chord = keys.Get(b.id);
    const keymap::Chord alt = keys.Alt(b.id);
    const bool shadowed =
        std::find(claimed.begin(), claimed.end(), chord) != claimed.end();
    if (!shadowed && Pressed(chord))
      RunCommand(state, b.id);
    else if (alt && Pressed(alt))
      RunCommand(state, b.id);
  }
}

} // namespace ui
