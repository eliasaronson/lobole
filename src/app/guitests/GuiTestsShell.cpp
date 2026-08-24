#include "app/guitests/GuiTestsCommon.h"

// Shell GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.

// Every button on the model toolbar, in bar order. File scope because two
// tests read it — "every_tool_is_reachable" (the row fits, nothing is culled)
// and "no_duplicate_glyph" (no two of them wear the same icon). A tool added
// to the bar and not to this list is caught by the first of those.
static const char* const kModelTools[] = {
    "###tb_new_sketch", "###tb_box",         "###tb_sphere",
    "###tb_cylinder",   "###tb_extrude",     "###tb_revolve",
    "###tb_curve3d",    "###tb_sweep",       "###tb_loft",
    "###tb_pipe",       "###tb_datum_plane", "###tb_datum_axis",
    "###tb_boolean",    "###tb_mirror",      "###tb_pattern",
    "###tb_fillet",     "###tb_chamfer",     "###tb_rib",
    "###tb_hole",       "###tb_thread",      "###tb_shell",
    "###tb_draft",      "###tb_split",       "###tb_refine",
    "###tb_fit",        "###tb_planes",      "###tb_simulation",
    "###tb_section",    "###tb_measure",     "###tb_render",
};

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

  t = IM_REGISTER_TEST(engine, "shortcuts", "undo_redo_keyboard");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
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
    s_App->mateTool.active = true;
    ctx->Yield(2);

    ctx->MenuClick("File/New");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->editFeatureId, (uint64_t)0);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(!s_App->measure.active);
    IM_CHECK(!s_App->dimTool.active);
    IM_CHECK(!s_App->mateTool.active);
  };

  // A standard view is an ORIENTATION, not a place. Every one of them —
  // the menu, the view cube, the Shift+1..7 shortcuts, the command palette —
  // passed the world origin as the point to look at, so fitting the view on a
  // part modelled away from the origin and then pressing Front left you
  // looking at empty space beside it (docs/cam/dogfood-round.md).
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
    ctx->MenuClick("Help/About Designer");
    ctx->Yield();
    IM_CHECK(s_App->showAbout);
    // It is a real window with the build in it, not just a flag flip.
    ctx->SetRef("//###AboutDesigner");
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

  // A new sketch defaults to the Select tool; letter keys switch tools (Onshape
  // keymap: g = rectangle, m = trim).
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
#ifdef HAVE_CAM
    IM_CHECK(ctx->ItemExists("**/###tb_cam"));
#endif
  };

  // No two commands in ONE bar may wear the same glyph. Split and Section
  // both shipped as SCISSORS, one row apart, and Datum Plane and Toggle
  // Planes as two sizes of the same grid — a bar of 30 unlabelled icons is
  // already a memory test, and a duplicate makes it a guess.
  //
  // Reuse ACROSS bars is fine and deliberate: scissors is Section here and
  // Trim in the sketch bar, the eye is plane visibility here and glyph
  // visibility there. One glyph, one MEANING, is the rule; this checks the
  // stronger property that holds within a single row.
  //
  // Reading the glyph back from the live item, not from the source: the
  // button's label is "<glyph>###tb_x", and DebugLabel carries it whole (the
  // engine's buffer is 32 bytes and the longest of these is 20).
  t = IM_REGISTER_TEST(engine, "toolbar", "no_duplicate_glyph");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(4);
    ctx->SetRef("//##DockHost");

    std::unordered_map<std::string, std::string> byGlyph; // glyph -> first id
    for (const char* id : kModelTools) {
      const ImGuiTestItemInfo info =
          ctx->ItemInfo((std::string("**/") + id).c_str());
      IM_CHECK_SILENT(info.ID != 0);
      const std::string label = info.DebugLabel;
      const size_t hash = label.find("###");
      // A button with no "###" has no stable id either, which the reachability
      // test would already have caught — but say which one, not just "0 != 0".
      IM_CHECK_NO_RET(hash != std::string::npos && hash > 0);
      if (hash == std::string::npos || hash == 0)
        continue;
      const std::string glyph = label.substr(0, hash);
      const auto [it, fresh] = byGlyph.emplace(glyph, id);
      // NO_RET so one run names EVERY collision, not just the first.
      if (!fresh)
        ctx->LogError("%s and %s share one glyph", it->second.c_str(), id);
      IM_CHECK_NO_RET(fresh);
    }
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
    IM_CHECK(s_App->pendingFeatureId != 0); // Box preview began
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
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

    // Shadows are on by default and the renderer agrees.
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows);

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

  // A restart-tier row must SAY so rather than pretending to apply: the
  // swapchain and every renderer are built once, at startup.
  t = IM_REGISTER_TEST(engine, "settings", "a_restart_row_says_restart");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->settingsWindowOpen = true;
    ctx->Yield(2);
    bool sawGpuRow = false;
    bool saidRestart = false;
    for (const settings::Field& f : settings::Fields()) {
      if (std::string(f.key) != "gpu_device")
        continue;
      sawGpuRow = true;
      // The TIER is the contract: the window turns it into the "(needs a
      // restart)" note. The blurb deliberately does NOT repeat the word —
      // that read as "...the viewport needs a restart. (needs a restart)".
      saidRestart = f.tier == settings::Tier::Restart;
      // It does carry the asymmetry the note cannot: simulation builds its
      // device per run, so the same setting lands immediately there.
      IM_CHECK(strstr(f.blurb, "next run") != nullptr);
    }
    IM_CHECK(sawGpuRow);
    IM_CHECK(saidRestart);
    IM_CHECK(ctx->ItemExists("//###SettingsWindow/###set_gpu_device"));
  };

  // Viewport handles (docs/ui/viewport-handles.md): the three owners publish
  // into one list, so hover can say WHICH arrow is under the cursor — before
  // this, the hit test ran only on click and nothing knew until you pressed.
}
