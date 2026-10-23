// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/FramePacer.h" // kCooldownSeconds
#include "app/guitests/GuiTestsCommon.h"
#include "cad/FaceSelector.h"
#include "render/FieldRenderer.h"
#include "render/UnderlayRenderer.h"
#include "ui/DocumentSlots.h"
#include "ui/Icons.h"
#include "ui/ImportActions.h" // AddReferenceImageToSketch
#include "ui/RendererSync.h"  // InjectDeviceLoss
#include "ui/SimulationActions.h"

#include <functional>

// Shell GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.

// Every button on the model toolbar, in bar order, for
// "every_tool_is_reachable" (the row fits, nothing is culled).
// "no_duplicate_glyph" walks what is drawn instead, on every bar.
static const char* const kModelTools[] = {
    "###tb_new_sketch", "###tb_box",         "###tb_sphere",
    "###tb_cylinder",   "###tb_extrude",     "###tb_revolve",
    "###tb_curve3d",    "###tb_sweep",       "###tb_loft",
    "###tb_pipe",       "###tb_datum_plane", "###tb_datum_axis",
    "###tb_frame",      "###tb_boolean",     "###tb_mirror",
    "###tb_pattern",    "###tb_fillet",      "###tb_chamfer",
    "###tb_rib",        "###tb_hole",        "###tb_thread",
    "###tb_shell",      "###tb_draft",       "###tb_split",
    "###tb_refine",     "###tb_fit",         "###tb_planes",
    "###tb_simulation", "###tb_section",     "###tb_measure",
    "###tb_render",
};

namespace {

// The icon glyph a label starts with: its first code point, when that is in
// the Private Use Area both icon fonts live in. 0 = no icon.
uint32_t LeadingIcon(const char* label) {
  unsigned int cp = 0;
  ImTextCharFromUtf8(&cp, label, nullptr);
  return cp >= 0xE000 && cp <= 0xF8FF ? cp : 0;
}

// What a toolbar item IS, for telling a duplicate from the same command met
// twice: its "###" id where it has one, its visible text otherwise (a sketch
// flyout's rows carry no id).
std::string ItemIdentity(const char* label) {
  const char* hash = std::strstr(label, "###");
  if (hash != nullptr)
    return hash + 3;
  std::string s = label;
  const size_t text = s.find_first_not_of(' ', s.find(' '));
  return text == std::string::npos ? s : s.substr(text);
}

// Adds every icon-led item under `ref` to `seen` (glyph -> identity) and
// fails on a glyph two DIFFERENT items share. `skip` drops a flyout's face,
// which shows one of the rows it opens and would read as a second copy — and
// WHICH row depends on the last one used, so it lists every candidate.
void CollectToolbarIcons(ImGuiTestContext* ctx, const char* ref,
                         const char* bar,
                         std::unordered_map<uint32_t, std::string>& seen,
                         std::initializer_list<const char*> skip = {}) {
  ImGuiTestItemList items;
  ctx->GatherItems(&items, ref, -1);
  for (const ImGuiTestItemInfo& item : items) {
    const uint32_t glyph = LeadingIcon(item.DebugLabel);
    if (glyph == 0)
      continue;
    const std::string who = ItemIdentity(item.DebugLabel);
    if (std::any_of(skip.begin(), skip.end(), [&](const char* k) {
          return who.find(k) != std::string::npos;
        }))
      continue;
    if (glyph == LeadingIcon(ui::kUnknownIcon))
      IM_ERRORF("%s toolbar: \"%s\" has no icon in src/ui/Icons.cpp", bar,
                who.c_str());
    const auto [it, fresh] = seen.emplace(glyph, who);
    if (!fresh && it->second != who)
      IM_ERRORF("%s toolbar: \"%s\" and \"%s\" show the same icon U+%04X", bar,
                it->second.c_str(), who.c_str(), glyph);
  }
}

// Opens a flyout by its caret and collects its rows into the same bar.
void CollectFlyout(ImGuiTestContext* ctx, const char* caret, const char* bar,
                   std::unordered_map<uint32_t, std::string>& seen) {
  ctx->SetRef("//##DockHost");
  ctx->ItemClick((std::string("**/") + caret).c_str());
  ctx->Yield(2);
  CollectToolbarIcons(ctx, "//$FOCUSED", bar, seen);
  // Not Escape: in a sketch that is the session's own modal verb, and it
  // would end the sketch along with the popup.
  ctx->PopupCloseAll();
  ctx->Yield(2);
}

} // namespace

void RegisterShellGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "app", "boots_and_renders");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->Yield(3);
    IM_CHECK(ctx->UiContext->WithinFrameScope == false ||
             ctx->UiContext->FrameCount > 0);
  };

  t = IM_REGISTER_TEST(engine, "app", "core_windows_exist");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(ctx->GetWindowByRef("//###Model") != nullptr);
  };

  // A lost device is rebuilt in place
  // (docs/rendering/README.md#rebuilding-the-device): the document, its undo
  // history and the open tabs carry on,
  // and everything the renderers held is projected again -- a show-always
  // reference image, a FEM field, the model a click picks from and a quick
  // still renders. Then the same path by the GPU preference, onto another
  // device and back, where one can show the window.
  t = IM_REGISTER_TEST(engine, "app", "device_rebuild_keeps_the_session");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // The retry window is about real losses in a row; this test's own are
    // minutes apart as far as it is concerned.
    s_App->renderer.lastRebuildAt = -1.0;
    const int rebuildsBefore = s_App->renderer.rebuilds;

    // A sketch with a reference image shown always...
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);
    const uint32_t image = ui::AddReferenceImageToSketch(
        *s_App, SourceFixture("images/checker16x12.png"));
    IM_CHECK(image != 0);
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    sk->images[0].showAlways = true;
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield(4);
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->underlayRenderer != nullptr);
    IM_CHECK_EQ(s_App->underlayRenderer->DrawCount(), (size_t)1);

    // ...and a static study run, its field on the model.
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ctx->ItemClick("//###ToolWindow/###study_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    s_App->simulation.field = ui::SimulationState::Field::VonMises;
    IM_CHECK(WaitFor(ctx, "the field on the model", [] {
      return s_App->fieldRenderer != nullptr && s_App->fieldRenderer->HasMesh();
    }));
    // A click on the top face picks it, out of the study: before the
    // rebuild, so the setup is known to allow it, and after.
    const auto clickTop = [ctx]() -> bool {
      ui::ExitSimulation(*s_App);
      ctx->Yield(3);
      glm::dvec3 lo(0.0), hi(0.0);
      Doc()->GetBodies().back().shape.GetBoundingBox(lo, hi);
      const glm::vec3 mid = (lo + hi) * 0.5;
      const glm::vec3 size = hi - lo;
      const float radius = glm::length(size) * 0.5f;
      // As viewport/click_selects_facing_face frames it: a clean iso view,
      // and the centre of the biggest face that view sees.
      s_App->camera->StopAnimation();
      s_App->camera->SetTarget(mid);
      s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
      s_App->camera->SetPosition(
          mid + glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * radius * 2.5f);
      ctx->Yield(3);
      glm::vec3 face(hi.x, mid.y, mid.z);
      float area = size.y * size.z;
      if (size.x * size.z > area) {
        face = glm::vec3(mid.x, hi.y, mid.z);
        area = size.x * size.z;
      }
      if (size.x * size.y > area)
        face = glm::vec3(mid.x, mid.y, hi.z);
      ctx->MouseMoveToPos(WorldToWindow(face));
      ctx->Yield(2);
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      const bool picked = s_App->faceSelector != nullptr &&
                          s_App->faceSelector->GetSelectedFace() != nullptr;
      // Empty space clears it again.
      ctx->MouseMoveToPos(
          ImVec2(s_App->viewportPos.x + 20.0f, s_App->viewportPos.y + 20.0f));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      return picked;
    };
    IM_CHECK(clickTop());
    ui::EnterSimulation(*s_App);
    s_App->simulation.field = ui::SimulationState::Field::VonMises;
    IM_CHECK(WaitFor(ctx, "the field on the model again", [] {
      return s_App->fieldRenderer != nullptr && s_App->fieldRenderer->HasMesh();
    }));

    const size_t underlays = s_App->underlayRenderer->DrawCount();
    const size_t undoDepth = s_App->commands->UndoDepth();
    const size_t tabs = s_App->parts.size();
    const size_t activeTab = s_App->activePart;
    const MeshRenderer* oldMesh = s_App->meshRenderer;

    ui::InjectDeviceLoss(*s_App, "a GUI test");
    IM_CHECK(WaitFor(ctx, "the device rebuilt", [rebuildsBefore] {
      return s_App->renderer.rebuilds == rebuildsBefore + 1 &&
             !s_App->renderer.lost;
    }));
    IM_CHECK(!s_App->renderer.backstop);
    IM_CHECK_STR_EQ(s_App->renderer.lastRebuildReason.c_str(), "device_lost");
    IM_CHECK(s_App->meshRenderer != nullptr);
    // A new renderer, not the old one carried over. (The address may repeat;
    // what the old one held may not.)
    (void)oldMesh;
    IM_CHECK(WaitFor(ctx, "the model projected again", [] {
      return ui::RenderMeshesCurrent(*s_App) &&
             !s_App->gpu.documentMeshes.empty();
    }));
    IM_CHECK(WaitFor(ctx, "the field projected again", [] {
      return s_App->fieldRenderer != nullptr && s_App->fieldRenderer->HasMesh();
    }));
    IM_CHECK_EQ(s_App->underlayRenderer->DrawCount(), underlays);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoDepth);
    IM_CHECK_EQ(s_App->parts.size(), tabs);
    IM_CHECK_EQ(s_App->activePart, activeTab);
    // The menu bar says so, once.
    ctx->Yield(2);
    IM_CHECK(s_App->rendererRestartedNoticeUntil > ImGui::GetTime());

    // Out of the study, a click picks a face of the model as it is drawn
    // now -- the pick data was remade with the meshes.
    IM_CHECK(clickTop());

    // A quick still renders on the new device.
    {
      ui::AppState::RasterStill req;
      req.width = 64;
      req.height = 48;
      req.supersample = 1;
      ui::RasterFrame frame;
      std::string error, warning;
      IM_CHECK(s_App->captureRaster);
      IM_CHECK(s_App->captureRaster(req, &frame, &error, &warning));
      IM_CHECK(error.empty());
      IM_CHECK_EQ(frame.rgb.size(), (size_t)64 * 48 * 3);
    }

    // The GPU preference runs the same path: onto another device, and back.
    // Only a device that can show this window is a rung, and under Xvfb that
    // is often one alone -- then this half is skipped, by name.
    const std::string original = s_App->settings.gpuDevice;
    const std::string start = s_App->renderer.device;
    bool switched = false;
    for (const std::string& other : s_App->gpuDevices) {
      if (other == start)
        continue;
      const int before = s_App->renderer.rebuilds;
      s_App->settings.gpuDevice = other;
      IM_CHECK(WaitFor(ctx, "the preference acted on", [other] {
        return s_App->renderer.appliedPreference == other &&
               !s_App->renderer.preferenceWaiting;
      }));
      if (s_App->renderer.rebuilds == before)
        continue; // not a rung here: the ladder kept the device
      IM_CHECK_STR_EQ(s_App->renderer.device.c_str(), other.c_str());
      IM_CHECK_STR_EQ(s_App->renderer.lastRebuildReason.c_str(),
                      "gpu_preference");
      IM_CHECK(WaitFor(ctx, "the model on the other device", [] {
        return ui::RenderMeshesCurrent(*s_App) &&
               !s_App->gpu.documentMeshes.empty();
      }));
      switched = true;
      break;
    }
    s_App->settings.gpuDevice = original;
    IM_CHECK(WaitFor(ctx, "the preference put back", [original] {
      return s_App->renderer.appliedPreference == original &&
             !s_App->renderer.preferenceWaiting;
    }));
    if (switched)
      IM_CHECK_STR_EQ(s_App->renderer.device.c_str(), start.c_str());
    else
      ctx->LogInfo("skipped: the GPU-preference half -- only %s can show "
                   "this window here",
                   start.c_str());
    IM_CHECK(!s_App->renderer.lost);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoDepth);
    // Leave the menu bar as the next test expects it.
    s_App->rendererRestartedNoticeUntil = -1.0;
    s_App->renderer.lastRebuildAt = -1.0;
  };

  // Landing below the best device says so once, in a modal, and lastingly
  // in the menu bar and About (docs/rendering/README.md#the-device-ladder);
  // safe mode's banner offers its two ways out (docs/ui/settings.md#safe-mode).
  // Start-up decides both, so the test sets what it would have set.
  t = IM_REGISTER_TEST(engine, "app", "renderer_fallback_and_safe_mode_say_so");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const ui::AppState::RendererStatus renderer = s_App->renderer;
    const ui::AppState::SafeMode safeMode = s_App->safeMode;
    const bool noticeShown = s_App->rendererFallbackNoticeShown;
    const std::function<void()> restart = s_App->restartNormally;
    const std::function<void()> setAside = s_App->setAsideAndRestart;

    s_App->renderer.fallback = true;
    s_App->renderer.software = true;
    s_App->renderer.device = "llvmpipe (test)";
    s_App->renderer.reason =
        "Test GPU: VK_ERROR_INITIALIZATION_FAILED at device creation";
    s_App->rendererFallbackNoticeShown = false;
    ctx->Yield(3);
    ImGuiWindow* modal = ctx->GetWindowByRef("//###RendererFallback");
    IM_CHECK(modal != nullptr && modal->Active);
    ctx->SetRef("//###RendererFallback");
    ctx->ItemClick("Dismiss###startup_notice_dismiss");
    ctx->Yield(3);
    IM_CHECK(s_App->rendererFallbackNoticeShown); // said once
    modal = ctx->GetWindowByRef("//###RendererFallback");
    IM_CHECK(modal == nullptr || !modal->Active);
    IM_CHECK(s_App->renderer.fallback); // the menu bar's notice stays

    int restarts = 0, setAsides = 0;
    s_App->safeMode.active = true;
    s_App->safeMode.why = "a GUI test asked";
    s_App->safeMode.settingsPath = "/nowhere/settings.json";
    s_App->safeMode.layoutPath = "/nowhere/imgui.ini";
    s_App->restartNormally = [&restarts] { ++restarts; };
    s_App->setAsideAndRestart = [&setAsides] { ++setAsides; };
    ctx->Yield(2);
    ImGuiWindow* banner = ctx->GetWindowByRef("//###SafeModeBanner");
    IM_CHECK(banner != nullptr && banner->Active);
    ctx->SetRef("//###SafeModeBanner");
    ctx->ItemClick("Restart normally###safe_restart");
    ctx->ItemClick("Set them aside and restart###safe_set_aside");
    IM_CHECK_EQ(restarts, 1);
    IM_CHECK_EQ(setAsides, 1);

    s_App->renderer = renderer;
    s_App->safeMode = safeMode;
    s_App->rendererFallbackNoticeShown = noticeShown;
    s_App->restartNormally = restart;
    s_App->setAsideAndRestart = setAside;
    ctx->Yield(2);
  };

  t = IM_REGISTER_TEST(engine, "shortcuts", "undo_redo_keyboard");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
  };

#ifdef HAVE_CAM
  t = IM_REGISTER_TEST(engine, "app", "new_document_closes_tool_sessions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);

    // Every transient session at once, so this test fails if a NEW one is
    // added without being cleared alongside the rest.
    s_App->editFeatureId = Doc()->GetFeature(0)->GetId();
    s_App->camToolOpen = true;
    s_App->measure.active = true;
    s_App->dimTool.active = true;
    s_App->drawingNoteTool.kind = ui::AppState::DrawingNoteTool::Kind::Fcf;
    s_App->detailTool.active = true;
    s_App->mateTool.active = true;
    ctx->Yield(2);

    ctx->MenuClick("File/New");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->editFeatureId, (uint64_t)0);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(!s_App->measure.active);
    IM_CHECK(!s_App->dimTool.active);
    IM_CHECK(s_App->drawingNoteTool.kind ==
             ui::AppState::DrawingNoteTool::Kind::None);
    IM_CHECK(!s_App->detailTool.active);
    IM_CHECK(!s_App->mateTool.active);
  };

  // A standard view is an ORIENTATION, not a place. Every one of them —
  // the menu, the view cube, the Shift+1..7 shortcuts, the command palette —
  // passed the world origin as the point to look at, so fitting the view on a
  // part modelled away from the origin and then pressing Front left you
  // looking at empty space beside it (docs/ui/README.md#viewport).
#endif
  t = IM_REGISTER_TEST(engine, "frametime",
                       "frame_stats_measure_hud_stays_hidden");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(5); // some frames to measure

    // Measured: the app has run frames, and the phases account for all of
    // each one (an unattributed sliver would read as "fast phases, slow
    // frame").
    const render::FrameTimer::Stats fs = s_App->frameTimer.Snapshot();
    IM_CHECK(fs.frames > 0);
    IM_CHECK(s_App->frameTimer.TotalFrames() >= fs.frames);
    IM_CHECK(fs.avgMs > 0.0 && fs.fps > 0.0);
    IM_CHECK(fs.maxMs >= fs.p95Ms && fs.p95Ms >= fs.p50Ms);
    double phaseSum = 0.0;
    for (size_t i = 0; i < render::kPhaseCount; i++)
      phaseSum += fs.phaseMs[i];
    IM_CHECK(std::abs(phaseSum - fs.avgMs) < 0.05 * fs.avgMs + 0.05);

    // GPU per-pass timestamps ride along on every frame. This rig's GPU
    // supports them (a queue without timestampValidBits would make the rest
    // of this vacuous, so assert the support rather than skip silently).
    IM_CHECK(s_App->gpuStats.supported);
    IM_CHECK(s_App->gpuStats.samples > 0);
    // The main pass draws the UI every frame, so it cannot be zero; and the
    // three deltas are contiguous, so they must SUM to the total exactly —
    // a gap would mean a write point drifted off its pass boundary.
    IM_CHECK(s_App->gpuStats.mainMs > 0.0);
    IM_CHECK(std::abs(s_App->gpuStats.totalMs -
                      (s_App->gpuStats.shadowMs + s_App->gpuStats.ssaoMs +
                       s_App->gpuStats.mainMs)) < 1e-9);
    IM_CHECK(s_App->gpuStats.totalMs < 1000.0); // ms, not a unit mix-up

    // Hidden: nothing in the UI opened it, and the window does not exist.
    IM_CHECK(!s_App->perfHudOpen);
    // NoError: asking whether a window is absent must not log an error, or
    // every later lookup in this test returns null. Latent here — it only
    // bites once something else in the process has opened the HUD first.
    IM_CHECK(ctx->WindowInfo("//Frame time###PerfHud", ImGuiTestOpFlags_NoError)
                 .Window == nullptr);

    // The undocumented chord is the door.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    ctx->Yield(2);
    IM_CHECK(s_App->perfHudOpen);
    ImGuiWindow* hud = ctx->WindowInfo("//Frame time###PerfHud").Window;
    IM_CHECK(hud != nullptr);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    ctx->Yield(2);
    IM_CHECK(!s_App->perfHudOpen); // and it closes again
  };

  // Simulation is a mode, and its studies belong to the document. File > New
  // used to leave the simulation toolbar up over a brand-new part.

  // Nothing had ever RUN a study by clicking — the four tests above all stop
  // at setup — and nothing had ever touched topology optimization at all. A
  // 2026-08-23 dogfooding round did both by hand and found the GPU solver
  // returning a ZERO displacement field on a beam-shaped part, which reads
  // downstream as a structure carrying no load: zero compliance, zero
  // sensitivities, and a "converged" report on the third iteration with the
  // volume constraint abandoned (0.107 against a 0.30 target). These two run
  // the real thing end to end and check the answer.

  t = IM_REGISTER_TEST(engine, "help", "shortcuts_window_opens");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Help/Keyboard Shortcuts");
    ctx->Yield();
    IM_CHECK(ctx->ItemExists("//###ShortcutsWindow"));
    IM_CHECK(s_App->showShortcuts);
    ctx->WindowClose("//###ShortcutsWindow");
    ctx->Yield();
    IM_CHECK(!s_App->showShortcuts);
  };

  // U9: Shell/Draft/Split/Refine had parameters (and mostly slots) but no
  // creation path outside the script API — a human could not reach them.

  // Shell defaults to the newest body, so the only pick needed is which face
  // to open.
  t = IM_REGISTER_TEST(engine, "help", "about_opens_and_names_the_build");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(!s_App->showAbout);
    ctx->MenuClick("Help/About Loböle");
    ctx->Yield();
    IM_CHECK(s_App->showAbout);
    // It is a real window with the build in it, not just a flag flip.
    ctx->SetRef("//###AboutLobole");
    IM_CHECK(ctx->ItemExists("###about_copy"));
    // The attributions are a tree node, collapsed by default; open it so the
    // test covers the table actually building.
    ctx->ItemClick("Third-party components");
    ctx->Yield();
    ctx->ItemClick("###about_close");
    ctx->Yield();
    IM_CHECK(!s_App->showAbout);
  };

  // Section view on an ASSEMBLY tab, through the tool. The bugs this pins:
  // the cut had no filled caps (you saw the inside of hollow shells), the
  // plane defaulted to the world origin (a model built elsewhere lost
  // everything or nothing), and a second cut was impossible.
  t = IM_REGISTER_TEST(engine, "keys", "registry_is_one_to_one");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    for (const keymap::Binding& b : keymap::Defaults()) {
      ctx->LogDebug("binding %s", b.id);
      IM_CHECK_SILENT(ui::FindCommand(b.id) != nullptr);
    }
    for (const ui::Command& c : ui::Commands()) {
      ctx->LogDebug("command %s", c.id);
      IM_CHECK_SILENT(keymap::Find(c.id) != nullptr);
    }
    IM_CHECK_EQ(ui::Commands().size(), keymap::Defaults().size());
  };

  // ActiveContext must stay DrawToolbar's own branch: if the two disagree you
  // see one mode's buttons while the keys run another mode's commands.
  t = IM_REGISTER_TEST(engine, "keys", "context_follows_the_toolbar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK_EQ(ui::ActiveContext(*s_App), keymap::Context::Part);
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK_EQ(ui::ActiveContext(*s_App), keymap::Context::Sketch);
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
    IM_CHECK_EQ(ui::ActiveContext(*s_App), keymap::Context::Part);
  };

  // Rebinding actually moves the key: the new chord runs the command and the
  // old one stops. Without this the whole keymap could be settings-shaped
  // decoration over a still-hard-coded dispatcher.
  t = IM_REGISTER_TEST(engine, "keys", "rebinding_moves_the_shortcut");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    // Default: L is Line.
    ctx->KeyPress(ImGuiKey_L);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);

    // Move Line to Shift+J. ResetAppForTest restores the defaults, because
    // the keymap lives inside `settings` (docs/ui/settings.md).
    IM_CHECK(s_App->settings.keys.Set("sketch.line", "Shift+J"));
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    ctx->Yield();

    ctx->KeyPress(ImGuiKey_L); // the OLD chord now does nothing
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_J);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);

    // And the toolbar's tooltip follows the binding rather than a literal.
    IM_CHECK_STR_EQ(ui::WithChord(*s_App, "sketch.line", "Line").c_str(),
                    "Line (Shift+J)");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // A sketch binding SHADOWS the global with the same chord. Shift+F is
  // zoom-to-selection everywhere and Fillet in a sketch — and before the
  // contexts existed BOTH fired on one press.
  t = IM_REGISTER_TEST(engine, "keys", "context_shadows_the_global");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    const glm::vec3 before = s_App->camera->GetPosition();
    ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_F);
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Fillet);
    // The camera did not ALSO reframe: one press, one consumer.
    IM_CHECK(!s_App->camera->IsAnimating());
    IM_CHECK_EQ(before, s_App->camera->GetPosition());
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // A new sketch defaults to the Select tool; letter keys switch tools (the
  // sketch keymap: g = rectangle, m = trim).
  t = IM_REGISTER_TEST(engine, "keys", "tool_switch");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select); // default
    ctx->KeyPress(ImGuiKey_L);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);
    ctx->KeyPress(ImGuiKey_G);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Rectangle);
    ctx->KeyPress(ImGuiKey_M);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Trim);
  };

  // `h` applies Horizontal to the current selection (constraint keymap).
  t = IM_REGISTER_TEST(engine, "keys", "constraint_on_selection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l =
        sk->AddEntity(sketch::Line{{0, 0}, {4, 0.5f}}); // slightly tilted
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{l, 0}};
    ctx->KeyPress(ImGuiKey_H);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    int nh = 0;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Horizontal)
        nh++;
    IM_CHECK_EQ(nh, 1);
    const auto& ln = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l)]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f); // solved flat
  };

  // Dragging a box over empty space selects the entities it encloses (window).
  t = IM_REGISTER_TEST(engine, "prompt", "staged_tool_steps");
  t->TestFunc = [](ImGuiTestContext*) {
    ui::SketchEditState e;
    e.activeSketchId = 1; // in sketch edit

    e.tool = ui::SketchTool::Select;
    IM_CHECK(ui::SketchToolPrompt(e).text.empty()); // Select is quiet

    e.tool = ui::SketchTool::Line;
    e.hasPendingPoint = false;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 1);
    e.hasPendingPoint = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 2);

    ui::SketchEditState m;
    m.tool = ui::SketchTool::Mirror;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 1); // collecting geometry
    IM_CHECK_EQ(ui::SketchToolPrompt(m).stepCount, 2);
    m.mirrorAxisPhase = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 2); // axis-pick step

    ui::SketchEditState el;
    el.tool = ui::SketchTool::Ellipse;
    el.hasPendingPoint = true;
    el.hasPendingPoint2 = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(el).step, 3); // third click
    IM_CHECK_EQ(ui::SketchToolPrompt(el).stepCount, 3);

    ui::SketchEditState sp;
    sp.tool = ui::SketchTool::Spline;
    IM_CHECK(!ui::SketchToolPrompt(sp).text.empty()); // prompts before any pole
    sp.splinePoles = {{0, 0}, {1, 1}};
    IM_CHECK(ui::SketchToolPrompt(sp).text.find("finish") != std::string::npos);
  };

  // Guided mirror (N2): select geometry in-tool, Enter, then click the axis.
  t = IM_REGISTER_TEST(engine, "toolbar", "every_tool_is_reachable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // A body, so the tools that need one are enabled (a disabled button is
    // still submitted, but this keeps the test honest about real conditions).
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(4);

    // THE load-bearing assertion, and it is structural on purpose. Probing
    // each button with ItemExists does NOT work: the test engine SCROLLS to
    // bring an item into view, so every id resolves whether or not it was
    // reachable, and the test passes with the bug present (verified by
    // removing the wrap and watching it stay green). What actually matters is
    // that the row FITS — content no wider than the window means nothing is
    // scrolled out, for a human or for a script.
    // By substring: a child window's real name carries the parent's and a
    // hashed suffix ("##DockHost/##ToolbarRow_1A2B3C4D"), so an exact lookup
    // finds nothing.
    ImGuiWindow* bar = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
      if (std::string(w->Name).find("##ToolbarRow") != std::string::npos) {
        bar = w;
        break;
      }
    IM_CHECK(bar != nullptr);
    ctx->LogInfo("toolbar content %.0f px in %.0f px", bar->ContentSize.x,
                 bar->InnerRect.GetWidth());
    IM_CHECK(bar->ContentSize.x <= bar->InnerRect.GetWidth() + 1.0f);

    // And the ids are all present, which catches a tool dropped entirely.
    ctx->SetRef("//##DockHost");
    for (const char* id : kModelTools)
      IM_CHECK_SILENT(ctx->ItemExists((std::string("**/") + id).c_str()));
  };

  // No two commands in ONE bar may wear the same glyph. Split and Section
  // both shipped as SCISSORS, one row apart; Datum Plane and Toggle Planes as
  // two sizes of the same grid; later Revolve and Hem as one arrow mirrored,
  // Boolean and Corner relief as Font Awesome's group/ungroup pair, and three
  // arc tools under one glyph. A bar of 30 unlabelled icons is already a
  // memory test, and a duplicate makes it a guess.
  //
  // Reuse ACROSS bars is fine when it is one idea (the eye is plane
  // visibility here and glyph visibility there); the convention test
  // "a toolbar glyph means one thing" holds that rule over the icon table.
  // This checks what is DRAWN, on every bar and in every flyout, and that no
  // button fell through to the unknown-command glyph (docs/ui/README.md#icons).
  t = IM_REGISTER_TEST(engine, "toolbar", "no_duplicate_glyph");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    using Seen = std::unordered_map<uint32_t, std::string>;
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield(2);
    {
      Seen part;
      CollectToolbarIcons(ctx, "//##DockHost", "part", part,
                          {"tb_base_flange", "tb_edge_flange", "tb_hem",
                           "tb_jog", "tb_corner_relief", "tb_normal_cut",
                           "tb_flat_pattern", "tb_unfold", "tb_refold"});
      CollectFlyout(ctx, "sheet/###more", "part", part);
      IM_CHECK_GT((int)part.size(), 30);
    }
    {
      EnterSketchOnPlane(ctx, 0);
      Seen sketch;
      CollectToolbarIcons(ctx, "//##DockHost", "sketch", sketch, {"grp"});
      for (const char* g : {"circlegrp", "rectgrp", "arcgrp"})
        CollectFlyout(ctx, (std::string(g) + "/###more").c_str(), "sketch",
                      sketch);
      IM_CHECK_GT((int)sketch.size(), 25);
      ctx->SetRef("//##DockHost");
      ctx->ItemClick("**/###tb_sk_finish");
      ctx->Yield(2);
    }
    {
      ui::EnterSimulation(*s_App);
      ctx->Yield(3);
      Seen sim;
      CollectToolbarIcons(ctx, "//##DockHost", "simulation", sim);
      IM_CHECK_GE((int)sim.size(), 4);
      ui::ExitSimulation(*s_App);
      ctx->Yield(2);
    }
    const auto check = [&](size_t slot, const char* bar, int atLeast) {
      ui::SetActivePart(*s_App, slot);
      ctx->Yield(3);
      Seen seen;
      CollectToolbarIcons(ctx, "//##DockHost", bar, seen);
      IM_CHECK_GE((int)seen.size(), atLeast);
    };
    check(ui::AddAssembly(*s_App, "Unit"), "assembly", 12);
    check(ui::AddDrawing(*s_App, "Sheet"), "drawing", 12);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    MakeJob();
    ctx->Yield(3);
    Seen job;
    CollectToolbarIcons(ctx, "//##DockHost", "job", job);
    IM_CHECK_GE((int)job.size(), 15);
  };

  t = IM_REGISTER_TEST(engine, "palette", "ctrl_k_creates_box");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_K);
    ctx->Yield();
    IM_CHECK(s_App->paletteOpen);
    ctx->KeyChars("box");
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(!s_App->paletteOpen);
    // The Box tool began: it asks what to stand on first.
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Primitive);
    IM_CHECK(s_App->primitive.kind == primitive::Kind::Box);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
  };

  // Dimension visuals: a new distance dim is born with a non-zero label
  // offset (legible placement), its pill drags to a new spot (writing the
  // serialized labelOffset), and the drag neither moves nor draws geometry
  // underneath (the dimHot gate).
  t = IM_REGISTER_TEST(engine, "settings", "a_change_reaches_the_renderer");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/###edit_preferences");
    ctx->Yield();
    IM_CHECK(s_App->settingsWindowOpen);

    // Shadows are on by default and the renderer agrees. They are on the
    // Performance page: one page shows at a time.
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows);
    ctx->ItemClick("//###SettingsWindow/###cat_Performance");
    ctx->Yield();

    ctx->ItemClick("//###SettingsWindow/###set_shadows");
    ctx->Yield(2);
    IM_CHECK(!s_App->settings.shadows);
    IM_CHECK(!s_App->showShadows); // ApplySettings pushed it through

    // Back on, from the same control: the View menu's duplicate is gone —
    // render quality was session state there, resetting every launch, and two
    // controls over one preference is how the ends drift apart.
    ctx->ItemClick("//###SettingsWindow/###set_shadows");
    ctx->Yield(2);
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows);

    // Lock lights to model: off by default (the rig follows the camera), and
    // the checkbox reaches the flag RecordScenePrepasses hands the renderers
    // (docs/rendering/README.md#the-light-rig).
    IM_CHECK(!s_App->settings.lockLightsToModel);
    IM_CHECK(!s_App->lightsLockedToModel);
    ctx->ItemClick("//###SettingsWindow/###cat_Appearance");
    ctx->Yield();
    ctx->ItemClick("//###SettingsWindow/###set_lock_lights_to_model");
    ctx->Yield(2);
    IM_CHECK(s_App->settings.lockLightsToModel);
    IM_CHECK(s_App->lightsLockedToModel);
    ctx->ItemClick("//###SettingsWindow/###set_lock_lights_to_model");
    ctx->Yield(2);
    IM_CHECK(!s_App->lightsLockedToModel);
    // And the View menu really has no duplicate left.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuAction(ImGuiTestAction_Open, "View");
    ctx->Yield();
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Shadows"));
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Render Quality"));
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Font Size"));
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();

    // An enum row is a combo, and its id is stable.
    ctx->ItemClick("//###SettingsWindow/###cat_Performance");
    ctx->Yield();
    ctx->ItemClick("//###SettingsWindow/###set_ssao");
    ctx->ItemClick("//$FOCUSED/full");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settings.ssao, std::string("full"));
    IM_CHECK(s_App->showAmbientOcclusion);
    IM_CHECK(s_App->ssaoFullRes);

    // Reset puts everything back, so a broken config never needs the JSON.
    ctx->ItemClick("//###SettingsWindow/###set_reset_all");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settings.ssao, std::string("half"));
    IM_CHECK(s_App->settings.shadows);
  };

  // The GPU row is Live (docs/ui/settings.md#the-gpu-choice): a change
  // rebuilds the device in place, so the row says what is in use rather
  // than promising a restart.
  t = IM_REGISTER_TEST(engine, "settings", "the_gpu_row_switches_live");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->settingsWindowOpen = true;
    ctx->Yield(2);
    bool sawGpuRow = false;
    for (const settings::Field& f : settings::Fields()) {
      if (std::string(f.key) != "gpu_device")
        continue;
      sawGpuRow = true;
      IM_CHECK(f.tier == settings::Tier::Live);
      // It carries the asymmetry the tier cannot: simulation builds its
      // device per run, so the same setting lands there on the next one.
      IM_CHECK(strstr(f.blurb, "next run") != nullptr);
    }
    IM_CHECK(sawGpuRow);
    ui::OpenPreferences(*s_App, "Performance");
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists("//###SettingsWindow/###set_gpu_device"));
  };

  // Preferences shows one page at a time (docs/ui/settings.md#the-window):
  // each category's rows and nothing else, and no page wider than the
  // window, at 100 % and at the largest font -- nothing scrolls sideways.
  t = IM_REGISTER_TEST(engine, "settings", "each_page_shows_its_own_rows");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->settingsWindowOpen = true;
    ctx->Yield(2);
    const auto categories = ui::PreferenceCategories();
    const auto noSideways = [&](const std::string& page) {
      ImGuiWindow* root =
          ctx->GetWindowByRef(ctx->GetID("//###SettingsWindow"));
      IM_CHECK_RETV(root != nullptr, false);
      for (ImGuiWindow* w : ctx->UiContext->Windows)
        if (w->RootWindow == root && w->Active) {
          ctx->LogInfo("%s: '%s' scroll max x %.1f", page.c_str(), w->Name,
                       w->ScrollMax.x);
          IM_CHECK_RETV(w->ScrollMax.x <= 0.5f, false);
        }
      return true;
    };
    for (float scale : {1.0f, 1.5f}) {
      s_App->settings.uiFontScale = scale;
      ui::MarkSettingsChanged(*s_App);
      ctx->Yield(4);
      // The window resizes to the font on its next appearing.
      s_App->settingsWindowOpen = false;
      ctx->Yield(2);
      s_App->settingsWindowOpen = true;
      ctx->Yield(3);
      for (const ui::PreferenceCategory& c : categories) {
        ctx->ItemClick(("//###SettingsWindow/###cat_" + c.name).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(s_App->settingsCategory, c.name);
        for (const settings::Field& f : settings::Fields()) {
          const bool mine = c.name == f.section;
          const std::string row =
              std::string("//###SettingsWindow/###set_") + f.key;
          IM_CHECK_EQ(ctx->ItemExists(row.c_str()), mine);
        }
        if (c.name == "Keyboard")
          IM_CHECK(ctx->ItemExists("//###SettingsWindow/###key_filter"));
        if (c.name == "Machines")
          IM_CHECK(ctx->ItemExists("//###SettingsWindow/###mach_new"));
        if (c.name == "Print profiles")
          IM_CHECK(ctx->ItemExists("//###SettingsWindow/###prof_new"));
        // A table page resets alone; a library has no reset.
        IM_CHECK_EQ(
            ctx->ItemExists("//###SettingsWindow/###set_reset_category"),
            !c.rows.empty());
        IM_CHECK(noSideways(c.name));
      }
    }
    s_App->settings.uiFontScale = 1.0f;
    ui::MarkSettingsChanged(*s_App);
    ctx->Yield(3);
  };

  // The search field finds rows by label and blurb across every page, each
  // under its page's name, and the row found is the real control.
  t = IM_REGISTER_TEST(engine, "settings", "search_finds_rows_on_any_page");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ui::OpenPreferences(*s_App, "Appearance");
    ctx->Yield(2);
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemInputValue("###set_search", "shadow");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settingsSearch, std::string("shadow"));
    IM_CHECK(ctx->ItemExists("###set_shadows"));
    IM_CHECK(ctx->ItemExists("###set_shadow_map"));
    IM_CHECK(!ctx->ItemExists("###set_ui_font_scale"));
    IM_CHECK(!ctx->ItemExists("###set_autosave_interval"));
    // The row is live: a click here is the preference.
    IM_CHECK(s_App->settings.shadows);
    ctx->ItemClick("###set_shadows");
    ctx->Yield(2);
    IM_CHECK(!s_App->settings.shadows);
    IM_CHECK(!s_App->showShadows);
    // A page's name finds the page, and choosing it ends the search.
    ctx->ItemInputValue("###set_search", "keyb");
    ctx->Yield(2);
    ctx->ItemClick("###find_page_Keyboard");
    ctx->Yield(2);
    IM_CHECK(s_App->settingsSearch.empty());
    IM_CHECK_EQ(s_App->settingsCategory, std::string("Keyboard"));
    ctx->ItemInputValue("###set_search", "no such preference");
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists("###set_shadows"));
    s_App->settingsSearch.clear();
    s_App->settingsWindowOpen = false;
  };

  // Reset <category> puts that page back and leaves the others.
  t = IM_REGISTER_TEST(engine, "settings", "a_page_resets_alone");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->settings.ssao = "full";
    s_App->settings.shadows = false;
    s_App->settings.lockLightsToModel = true; // Appearance
    s_App->settings.autosaveInterval = 120.0; // General
    ui::MarkSettingsChanged(*s_App);
    ui::OpenPreferences(*s_App, "Performance");
    ctx->Yield(2);
    ctx->ItemClick("//###SettingsWindow/###set_reset_category");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settings.ssao, std::string("half"));
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows); // applied, not only stored
    IM_CHECK(s_App->settings.lockLightsToModel);
    IM_CHECK(s_App->lightsLockedToModel);
    IM_CHECK_EQ(s_App->settings.autosaveInterval, 120.0);
    s_App->settingsWindowOpen = false;
  };

  // Idle throttle (docs/rendering/frame-pacing.md#idle-throttle). Three
  // contracts in one sitting: the suite itself never idles (the engine drives
  // frames, so the pacer is bypassed under --gui-tests), a force-enabled pacer
  // rests after the cooldown, and input restores full rate.
  t = IM_REGISTER_TEST(engine, "shell", "idle_throttle_rests_and_input_wakes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // Bypassed in test mode: frames must never slow under the engine.
    const uint64_t before = s_App->pacerIdleFrames;
    ctx->Yield(5);
    IM_CHECK_EQ(s_App->pacerIdleFrames, before);

    // Opt back in through the test seam and hold still past the cooldown.
    // The cooldown is wall time, so this loop watches the clock, not a frame
    // count — test-mode frames are fast until the pacer slows them.
    s_App->pacerForceEnable = true;
    // (Not ImGui::GetTime(): a suite run counts that in frames.)
    IM_CHECK(WaitFor(
        ctx, "the pacer to rest",
        [&] { return s_App->pacerIdleFrames > before; },
        app::FramePacer::kCooldownSeconds + 5.0));

    // Mouse motion is activity: the pacer returns to full rate and the idle
    // counter freezes for at least the cooldown window.
    ctx->MouseMoveToPos(ImVec2(300.0f, 300.0f));
    ctx->MouseMoveToPos(ImVec2(420.0f, 340.0f));
    const auto woke = std::chrono::steady_clock::now();
    ctx->Yield(2);
    const uint64_t afterWake = s_App->pacerIdleFrames;
    // Frames INSIDE the cooldown, which is wall time: five frames of 160 ms
    // on a loaded machine outlast it, and the pacer is right to rest again.
    // Asked with 0.3 s to spare, since the pacer saw the wake a frame or two
    // before this thread could note the time.
    const auto inside =
        std::chrono::duration<double>(app::FramePacer::kCooldownSeconds - 0.3);
    for (int i = 0; i < 5 && std::chrono::steady_clock::now() - woke < inside;
         i++)
      ctx->Yield();
    if (std::chrono::steady_clock::now() - woke < inside)
      IM_CHECK_EQ(s_App->pacerIdleFrames, afterWake);
    else
      ctx->LogInfo("frames outlasted the cooldown; nothing to hold");

    s_App->pacerForceEnable = false;
    ctx->Yield();
  };

  // Viewport handles (docs/ui/viewport-handles.md): the three owners publish
  // into one list, so hover can say WHICH arrow is under the cursor — before
  // this, the hit test ran only on click and nothing knew until you pressed.
}
