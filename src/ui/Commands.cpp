#include "ui/Commands.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/features/SketchFeature.h"
#include "render/Camera.h"
#include "render/RayTracingRenderer.h"
#include "ui/AppActions.h"
#include "ui/FeatureEditor.h" // kCamToolSlot (the CAM session's pick slot)
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/TerminalPanel.h"

#include "imgui.h"

#include <glm/glm.hpp>

#include <algorithm>
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

void StandardView(AppState& s, glm::vec3 normal) {
  // The camera's OWN pivot, not the world origin: a standard view is an
  // orientation, not a place (docs/cam/dogfood-round.md).
  if (s.camera)
    s.camera->AlignToFace(s.camera->GetTarget(), normal);
}

void ToggleMeasure(AppState& s) {
  const bool wasActive = s.measure.active;
  s.measure = MeasureState{};
  s.measure.active = !wasActive;
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
  s.dimTool = AppState::DimTool{};
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
    add("view.front", nullptr, [](AppState& s) { StandardView(s, {0, 0, 1}); });
    add("view.back", nullptr, [](AppState& s) { StandardView(s, {0, 0, -1}); });
    add("view.left", nullptr, [](AppState& s) { StandardView(s, {-1, 0, 0}); });
    add("view.right", nullptr, [](AppState& s) { StandardView(s, {1, 0, 0}); });
    add("view.top", nullptr, [](AppState& s) { StandardView(s, {0, 1, 0}); });
    add("view.bottom", nullptr,
        [](AppState& s) { StandardView(s, {0, -1, 0}); });
    add("view.iso", nullptr, [](AppState& s) {
      StandardView(s, glm::normalize(glm::vec3(1, 1, 1)));
    });
    add("view.reset_layout", nullptr,
        [](AppState& s) { s.buildDefaultDockLayout = true; });
    add("view.controls_help", nullptr,
        [](AppState& s) { s.showControlsHelp = !s.showControlsHelp; });
    add(
        "view.measure",
        [](AppState& s) {
          return HasBody(s) || (s.assembly && s.assembly->OccurrenceCount());
        },
        ToggleMeasure);
    add("view.section", nullptr, ToggleSection);
    add(
        "view.render",
        [](AppState& s) {
          const bool ready = s.rayTracer && s.rayTracer->IsInitialized();
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

    add("part.fillet", IdleBody, [](AppState& s) { CreateFillet(s); });
    add("part.chamfer", IdleBody, [](AppState& s) { CreateChamfer(s); });
    add("part.hole", IdleBody, [](AppState& s) { StartHoleTool(s); });
    add("part.thread", IdleBody, [](AppState& s) { CreateThread(s); });
    add("part.draft", IdleBody, [](AppState& s) { CreateDraft(s); });
    add("part.shell", IdleBody, [](AppState& s) { CreateShell(s); });
    add("part.rib", IdleBody, [](AppState& s) { CreateRib(s); });
    add(
        "part.boolean",
        [](AppState& s) { return Idle(s) && Producers(s) >= 2; },
        [](AppState& s) { CreateBoolean(s); });
    add(
        "part.mirror", [](AppState& s) { return Idle(s) && Producers(s) >= 1; },
        [](AppState& s) { CreateMirror(s); });
    add(
        "part.pattern",
        [](AppState& s) { return Idle(s) && Producers(s) >= 1; },
        [](AppState& s) { CreatePattern(s); });
    add("part.split", IdleBody, [](AppState& s) { CreateSplit(s); });
    add("part.refine", IdleBody, [](AppState& s) { CreateRefine(s); });
    add("part.simulation", IdleBody, [](AppState& s) { EnterSimulation(s); });
    add("part.cam", HasBody, [](AppState& s) {
#ifdef HAVE_CAM
      s.camToolOpen = !s.camToolOpen;
      if (!s.camToolOpen) {
        s.camGhostOp = 0;
        if (s.slotPick.featureId == kCamToolSlot)
          s.slotPick = {};
      }
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

    // ----------------------------------------------------------- Drawing
    add("drawing.add_view", nullptr,
        [](AppState& s) { RequestPopup(s, "add_view"); });
    add("drawing.update", nullptr,
        [](AppState& s) { UpdateDrawingRenders(s); });
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
  // (docs/ui/terminal-panel.md). Without this gate Ctrl+Z would mean two
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
