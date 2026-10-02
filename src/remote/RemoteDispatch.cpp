#include "remote/InspectionContracts.h"
#include "remote/RemoteCatalog.h"
#include "remote/RemoteServer.h"

#include "remote/InstanceRegistry.h"

#include <Standard_Failure.hxx>

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "cad/MeshExport.h"
#include "common/Logger.h"
#include "core/Angles.h"
#include "core/Conventions.h"
#include "core/Expression.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingExport.h"
#include "core/features/DerivedFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/io/GitVault.h"
#include "core/io/Keymap.h"
#include "core/io/Serialization.h"
#include "core/io/Settings.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteAssembly.h"
#include "remote/RemoteCam.h"
#include "remote/RemoteConfigurations.h"
#include "remote/RemoteDrawings.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteInspection.h"
#include "remote/RemoteModeling.h"
#include "remote/RemoteMotion.h"
#include "remote/RemoteProject.h"
#include "remote/RemoteRequirements.h"
#include "remote/RemoteSheetMetal.h"
#include "remote/RemoteSimulation.h"
#include "remote/RemoteTube.h"
#include "remote/RpcParams.h"

#include "render/Camera.h"
#include "render/FrameTimer.h"
#include "render/MeshRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/RenderSheet.h"
#include "render/Screenshot.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/Commands.h" // ActiveContext (get_keymap)
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/MotionActions.h" // FfmpegPath, for ping's features
#include "ui/PreviewFeature.h"
#include "ui/ProjectActions.h"
#include "ui/RendererSync.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (U1b)
#include "ui/SettingsWindow.h"    // ApplySettings / the save debounce
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // FaceElementRef
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

#include "imgui.h"
#ifdef HAVE_TEST_ENGINE
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#endif

#include <cstring>

using nlohmann::json;

#include "remote/RemoteServerInternal.h"

// Deciding what an RPC MEANS: the server's own methods, the part-tab gate, and
// the routing to the domain modules that own everything else.
//
// What gates a method is the `flags` column of its MethodDoc row, next to its
// handler -- docs/architecture.md#the-remote-json-rpc. These are members of
// RemoteServer, defined across several translation units.

// RemoteHelpers.h already has this one. Defining a second, non-inline copy at
// global scope made a STRONG symbol that collides with the inline one in
// SerializationInternal.h -- which ELF resolves silently in favour of the
// strong definition and PE/COFF refuses outright, so it only ever failed on
// the Windows cross build.
using remote::Vec3ToJson;

// render_still's `outlines`: a bool, or refused -- nlohmann's own message
// for a string here is a type error naming no parameter.
[[nodiscard]] static bool StillOutlinesParam(const remote::RpcParams& p) {
  if (!p.contains("outlines"))
    return true;
  const json& v = p["outlines"];
  if (!v.is_boolean())
    throw std::runtime_error("outlines is true or false; got " + v.dump());
  return v.get<bool>();
}
// A quick still's line weight, px of the output image: from a hairline to a
// heavy pen, both well inside every device's line range at 4x.
static constexpr double kMinLineWeight = 0.25, kMaxLineWeight = 4.0;

// Levenshtein distance, for the "did you mean" hint on a rejected param key.
size_t EditDistance(const std::string& a, const std::string& b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); i++) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); j++) {
      const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

// The names in `pool` a caller who typed `name` probably meant: a near miss
// by edit distance, or one containing the other ("mate" -> add_mate,
// list_mates). Best first, at most five.
static std::vector<std::string>
CloseNames(const std::string& name, const std::vector<std::string>& pool) {
  std::vector<std::pair<size_t, std::string>> scored;
  const size_t within = std::max<size_t>(2, name.size() / 3);
  for (const std::string& k : pool) {
    const size_t d = EditDistance(name, k);
    const bool contains =
        name.size() >= 3 && (k.find(name) != std::string::npos ||
                             name.find(k) != std::string::npos);
    if (d <= within || contains)
      scored.emplace_back(contains ? std::min(d, within) : d, k);
  }
  std::sort(scored.begin(), scored.end());
  std::vector<std::string> out;
  for (const auto& [d, k] : scored) {
    if (out.size() == 5)
      break;
    if (std::find(out.begin(), out.end(), k) == out.end())
      out.push_back(k);
  }
  return out;
}

// The JSON type name a caller would recognise in their own request.
const char* JsonTypeName(const json& v) {
  switch (v.type()) {
  case json::value_t::null:
    return "null";
  case json::value_t::boolean:
    return "a boolean";
  case json::value_t::string:
    return "a string";
  case json::value_t::array:
    return "an array";
  case json::value_t::object:
    return "an object";
  case json::value_t::number_integer:
  case json::value_t::number_unsigned:
  case json::value_t::number_float:
    return "a number";
  default:
    return "a value";
  }
}

// Message for a request whose params carried keys the handler never read —
// a silently-ignored typo under the old path. Names the offending key(s),
// suggests the closest accepted key, and lists the method's vocabulary.
std::string UnknownParamError(const std::string& method,
                              const std::vector<std::string>& unknown,
                              const std::set<std::string>& accepted) {
  std::string msg = "unknown parameter";
  msg += unknown.size() > 1 ? "s " : " ";
  for (size_t i = 0; i < unknown.size(); i++) {
    msg += (i ? ", '" : "'") + unknown[i] + "'";
    // Closest accepted key within a small edit distance = probable typo.
    std::string best;
    size_t bestDist = 3; // only suggest a genuinely near miss
    for (const std::string& k : accepted) {
      const size_t d = EditDistance(unknown[i], k);
      if (d < bestDist) {
        bestDist = d;
        best = k;
      }
    }
    if (!best.empty())
      msg += " (did you mean '" + best + "'?)";
  }
  msg += " for '" + method + "'.";
  if (!accepted.empty()) {
    msg += " Accepted: ";
    bool first = true;
    for (const std::string& k : accepted) {
      msg += (first ? "" : ", ") + k;
      first = false;
    }
    msg += ".";
  }
  return msg;
}

std::string TypeErrorMessage(const std::string& method, const json& params,
                             const std::set<std::string>& seen,
                             const std::string& what) {
  std::string msg =
      "'" + method + "': a parameter has the wrong type — " +
      what.substr(what.find(']') == std::string::npos ? 0 : what.find(']') + 2);
  if (params.is_object() && !params.empty()) {
    msg += ". Given: ";
    bool first = true;
    for (auto it = params.begin(); it != params.end(); ++it) {
      msg += (first ? "" : ", ") + it.key() + " = " + JsonTypeName(it.value());
      first = false;
    }
  }
  if (!seen.empty()) {
    msg += ". This method reads: ";
    bool first = true;
    for (const std::string& k : seen) {
      msg += (first ? "" : ", ") + k;
      first = false;
    }
  }
  msg += ". `describe {method: \"" + method +
         "\"}` says what each parameter takes";
  return msg;
}

// Snap the camera to a named standard view (optionally fitting first). Shared
// by the `view` RPC and the raw `screenshot` handler so a raw screenshot(view=)
// orients the same way the MCP wrapper does. Returns false for an unknown name.
// Directions come from conv::kNamedViews — the same table as the view.*
// commands, the view cube and the drawings' OrientationAxes.
// boxMin/boxMax (both or neither) fit the camera to that WORLD box instead
// of the whole document — the render_sheet focus path.
bool ApplyStandardView(ui::AppState& state, const std::string& name, bool fit,
                       const glm::vec3* boxMin, const glm::vec3* boxMax) {
  const conv::NamedView* v = conv::FindView(name);
  if (v == nullptr)
    return false;
  // Fit runs first: it re-aims at the target (SetTarget), so the snap must
  // set the orientation last.
  if (fit) {
    if (boxMin != nullptr && boxMax != nullptr)
      ui::FitCameraToWorldBox(state, *boxMin, *boxMax);
    else
      ui::FitCameraToDocument(state);
  }
  state.camera->SnapToView(glm::vec3(v->toCamera), glm::vec3(v->up));
  return true;
}

// The methods RemoteServer dispatches itself (viewport, camera, selection,
// project tabs, UI events). describe: one row per `m ==` branch — extend
// together. `screenshot` and the UI actions are dispatched outside
// HandleSemantic but are part of the same surface.
static constexpr remote::MethodDoc kServerMethods[] = {
    {"batch",
     "Run {calls: [{method, params}]} as ONE transaction: one regeneration "
     "at the end instead of one per call, and ONE undo step. A param value "
     "{\"$ref\": n} anywhere is the result of the n-th call in this batch "
     "(1-based, BACKWARD only) — its `id` by default, or another field with "
     "{\"$ref\": n, \"key\": \"...\"} — so a sketch and the extrude "
     "that names it are one batch. A call that "
     "throws rolls the WHOLE batch back and names which call failed and why "
     "(stop_on_error, default true; false runs on and reports each error in "
     "place). Returns {results: [...], regen, calls, failed?, "
     "regenerations}: the per-call `regen`/`digest` are STRIPPED — geometry "
     "is answered once, at the end, by the batch's own `regen` — so a "
     "result carries the identities a call created. Refuses to nest, and "
     "refuses anything whose effect a rollback could not take back "
     "(screenshots, exports, project and git verbs, UI actions), naming it. "
     "A feature that fails to BUILD is not a failed call: it is reported in "
     "the final regen, exactly as it would be on its own.",
     "calls, stop_on_error"},
    {"describe",
     "This endpoint: methods (each with the parameter keys it accepts), "
     "feature/entity/constraint types, enums, and the document format "
     "version. {method} narrows the answer to that method's row alone "
     "(no type tables); an unknown method is refused naming close matches.",
     "method"},
    {"ping",
     "Liveness check; returns {ok, app, port, pid, mode, ui_actions, "
     "project, dirty, gpu, features, binary_replaced}. `pid` is this "
     "process's id, so "
     "a launcher can tell its own instance from one already on the port. `gpu` "
     "is the device that answers "
     "renders — empty under --headless, which has no renderer. `features` "
     "is what this BUILD contains (cam, cam_adaptive, fem, mesher) and "
     "whether ffmpeg is on PATH (ffmpeg: export_motion_video's mp4, webm "
     "and gif). "
     "`binary_replaced` is true when this process is running an executable "
     "that has since been rebuilt on disk: it will keep serving the OLD "
     "code, which is indistinguishable from a missing feature or a "
     "regression unless you ask.",
     ""},
    {"background_jobs",
     "Every long operation running off the frame, as {jobs: [{id, kind, label, "
     "fraction, cancellable, elapsed_seconds}]}. `fraction` is -1 when the "
     "job cannot say. Kinds: `cam` (an adaptive clearing pass; see "
     "cam_toolpath's `async`, and what a waiting cam_toolpath, cam_post, "
     "cam_verify or cam_simulate is waiting for), `cam-verify` (the panel's "
     "verification, or a waiting cam_verify's simulation), `cam-simulate` "
     "(reading what a setup's earlier setups left before its playback opens, "
     "for the panel's Simulate or a waiting cam_simulate), `sync` (git "
     "fetch/push/clone), `topo`, `study` (a FEM solve), `mesh`, `import`, "
     "`motion`, `motion-video` (export_motion_video), `open` (a "
     "project opening). Empty when nothing is running. Not the CAM "
     "job document -- that is list_job_parts.",
     ""},
    {"cancel_background_job",
     "Ask every job of {kind} to stop; returns {asked}. A job with no "
     "interruption points (a `cam` pass) finishes the piece it is on and "
     "starts nothing further; the answer says `cancellable: false` for it. "
     "Refuses a kind that is not running, naming what is.",
     "kind"},
    {"get_state",
     "Full snapshot: features (with errors), bodies (volume, bbox), "
     "selection, camera, undo/redo, rollback, regen summary. `part` (a slot "
     "name, stable id or index) reads ANOTHER part without switching to it — "
     "the tab-wide fields (selection, camera, undo/redo) then describe the "
     "active tab still, since that is where they live. Prefer it to "
     "set_active_part for a read: switching tabs clears the selection, the "
     "hidden bodies, the measure session and the explode state, which on "
     "somebody's running instance is their work.",
     "part", remote::NoFlags,
     "features[]{type,name,index,id?,material?,appearance?,suppressed?,error?,"
     "stale?,warning?,after_earlier_failure?, "
     "plus per-type keys}, "
     "bodies[]{name,volume,material,created_by,bbox_min,bbox_max,reference?,"
     "appearance?,mesh?}, "
     "selection{mesh,face,normal,center}, "
     "selection_items[]{kind,id?,element?}, selected_feature, "
     "camera{position,target,distance,fov_deg,aspect}, can_undo, can_redo, "
     "rollback, view_rollback, regen{ok,failed,warnings,executed,cached}, "
     "pending_feature_id"},
    {"undo", "Undo one step (assembly snapshot history on an assembly tab).",
     "", remote::NotInBatch},
    {"redo", "Redo one step.", "", remote::NotInBatch},
    {"view",
     "Orient the camera to a named view {name: front|back|left|right|top|"
     "bottom|iso}.",
     "fit, name"},
    {"fit_view",
     "Frame the geometry in the viewport. `width` and `height` (PIXELS, both "
     "together) frame it for a picture of that size instead — which is what "
     "`render_still` renders at, and a fit solved for the viewport crops a "
     "still whose aspect is narrower. Offscreen there is no window, so the "
     "viewport's own aspect is whatever the framebuffer happens to be: pass "
     "the size you are about to render.",
     "height, width"},
    {"set_camera",
     "Direct camera control {target?, eye?, direction?, up?, distance?}. "
     "NOTE: direction points from target TOWARD the camera (a view-from "
     "vector); passing eye [x,y,z] derives direction+distance instead. "
     "`distance` and every point here are model MILLIMETRES. `up` sets "
     "the roll, so it is refused within 1 degree of the view direction "
     "(eye to target), where it would fix none.",
     "direction, distance, eye, target, up"},
    {"orient_to_face", "Look straight at the selected face.", ""},
    {"select_face",
     "Select a face by {mesh, face} or stable {element}/{feature_id, "
     "element}; returns normal/center.",
     "element, face, mesh", remote::NeedsPart},
    {"screenshot",
     "Write the rendered viewport to {path} (needs a renderer "
     "— not available under --headless).",
     "fit, path, view", remote::NotInBatch},
    {"render_sheet",
     "One PNG: tiled named views (default iso/front/right/top, up to 6 via "
     "{views}) with a numeric footer — bbox, mass, volume, and on an "
     "assembly the contact_report summary. {focus: occurrence name|id} "
     "frames that one occurrence large in every view (close-ups without "
     "camera math). Needs a renderer.",
     "focus, path, views", remote::NotInBatch},
    {"render_still",
     "Path-traced still to {path} (ray tracing; needs GPU support). Defaults "
     "to 1920x1080 at 512 samples, the same as the GUI's Render dialog. "
     "`backdrop`: studio (a neutral sweep, the default) or environment (the "
     "HDRI itself). `outlines`: edges over the image, as the viewport draws "
     "them (default true). `fit` frames the model for THIS picture's aspect "
     "first: the still projects at width/height, while a plain `fit_view` "
     "solves for the viewport, so fit-then-render crops whenever the two "
     "differ — and offscreen the viewport is whatever the framebuffer "
     "happens to be. `mode`: path (default, the path tracer) or quick — the "
     "model as the VIEWPORT draws it (materials, lights, shadow map, SSAO, "
     "outlines; no sketches, gizmos or selection tints) at this size in "
     "about a second, `supersample` 1-4 (default 2) times wider and taller "
     "and averaged down. `line_weight` (quick only): the edges' width in "
     "PIXELS of the output image, 0.25-4, default the viewport's (1) -- "
     "drawn k times wider at k times the size, so lines stay as dark as on "
     "screen at any supersample. The path tracer's samples/seed/ground/"
     "backdrop are refused in quick mode.",
     "backdrop, fit, ground, height, line_weight, mode, outlines, path, "
     "samples, seed, supersample, width",
     remote::NotInBatch},
    {"rt_build", "Build ray-tracing acceleration structures.", ""},
    {"set_render_options", "Toggle shadows/SSAO/quality options.",
     "shadow_map, shadows, ssao"},
    {"frame_stats",
     "Frame-time statistics over the last {window} frames (default 240): "
     "fps, avg/p50/p95/max ms and the update|ui|wait|record|present split. "
     "{hud: true|false} toggles the overlay (off by default; also "
     "Ctrl+Shift+F). cpu_ms vs display_ms is the read: which of "
     "wait/present holds the vsync idle is driver-dependent. In --offscreen "
     "there is no frame loop, so it reports the PER-PASS GPU time of the "
     "renders taken so far (one sample per screenshot) and no frame numbers "
     "— which is where to measure a shader, since a windowed frame's `main` "
     "includes the wait for the swapchain image. --headless has no renderer "
     "and refuses. {reset:true} clears the GPU pass samples so the next "
     "renders are measured on their own; in --offscreen {renders:N} draws N "
     "frames back to back (saving nothing) and reports THOSE — which is how "
     "to measure a pass, since the encode between saved captures leaves the "
     "GPU idle enough to drop its clocks.",
     "hud, renders, reset, window"},
    {"get_settings", "Read the user preferences (docs/ui/settings.md).", "",
     remote::NoFlags,
     "settings_format, ui_font_scale, lock_lights_to_model, gpu_device, "
     "shadows, ssao, "
     "idle_throttle, shadow_map, git_user_name, git_user_email, "
     "terminal_panel, autosave_interval, keys{per-command chord}, "
     "window{x,y,w,h,maximized}, path, devices"},
    {"set_settings", "Change user preferences {key: value, ...}; persists.",
     "*", remote::NotInBatch},
    {"get_keymap",
     "The keyboard bindings (docs/ui/keybindings.md): every command with "
     "its context, label, current chord and default, plus any conflicts. "
     "{context: \"sketch\"} narrows it. A context binding SHADOWS a global "
     "one with the same chord — that is how N is Coincident while sketching "
     "and align-to-face everywhere else.",
     "context", remote::NoFlags,
     "contexts, "
     "bindings[]{id,context,section,label,chord,default,custom,alt?,hidden?}, "
     "active, conflicts[]{context,chord,commands}"},
    {"set_keybinding",
     "Rebind one command: {command: \"part.fillet\", chord: \"Shift+B\"}. "
     "An empty chord unbinds it; {reset: true} restores the default. "
     "Persists. Escape/Enter/Delete/arrows are fixed and have no command.",
     "chord, command, reset", remote::NotInBatch},
    {"reset_layout", "Reset the dock layout.", "", remote::NotInBatch},
    {"add_part", "Add a new empty part tab.", "name"},
    {"add_assembly", "Add an assembly tab.", "name"},
    {"list_parts",
     "List project TABS — every one, with `type`: part | assembly | "
     "drawing | job. Not parts alone, whatever the name says: a sweep that "
     "filed 'everything list_parts returned and nothing else claimed' into a "
     "Fasteners folder put the engine and the carburettor there. Filter on "
     "`type` unless you mean all four.",
     "", remote::NoFlags,
     "parts[]{name,index,id,type,folder?}, active, active_id, folders?"},
    {"rename_part", "Rename a part.", "new_name, part"},
    {"menu_click", "Drive a menu item by path, e.g. \"Edit/Undo\".", "path",
     remote::UiAction},
    {"menu_items",
     "List the menu bar's contents: no {path} = the top-level "
     "menus, {path: \"File\"} = that menu's items (submenu: "
     "true = walk deeper). The menu closes again.",
     "path", remote::UiAction},
    {"key_press", "Send a key chord, e.g. \"Ctrl+Z\".", "chord",
     remote::UiAction},
    {"item_click",
     "UI test tier: click a widget by {ref} or by the {id} ui_items reports "
     "(the only handle on hidden-label widgets like a combo). Ref example: "
     "\"//##DockHost/**/###tb_box\" (//window, ** = any depth, ### = the "
     "widget's stable id); {button: 1} right-clicks.",
     "button, id, ref", remote::UiAction},
    {"item_double_click",
     "UI test tier: double-click a widget (same ref "
     "grammar as item_click).",
     "id, ref", remote::UiAction},
    {"item_input", "UI test tier: type into a widget {ref, value}.",
     "id, ref, value", remote::UiAction},
    {"item_hover",
     "UI test tier: hover a widget {ref|id} — shows its "
     "tooltip and hover state without clicking.",
     "id, ref", remote::UiAction},
    {"popup_click",
     "UI test tier: open the popup of {ref|id} ({button}: 1 "
     "= context menu) and click {item} in it — the only way "
     "to reach a menu that is not the main menu bar (the tab "
     "bar's '+', context menus). {item} may be a submenu path, "
     "\"Part 1/Front\".",
     "button, id, item, ref, value", remote::UiAction},
    {"popup_items",
     "UI test tier: list what the popup of {ref|id} holds "
     "(submenu: true = address its children as "
     "\"Parent/Child\"), then close it.",
     "button, id, ref", remote::UiAction},
    {"item_combo",
     "UI test tier: choose {value} in the combo {ref|id} "
     "(opens and picks in one action — the popup does not "
     "survive between RPCs).",
     "button, id, item, ref, value", remote::UiAction},
    {"ui_items",
     "UI test tier: list the widgets under {ref} (default the "
     "focused window, {depth}): label, id, window, disabled, "
     "screen centre. The GUI, self-described. Labels come from a "
     "32-byte buffer, so a long one is CUT — `label_truncated` "
     "says so, and the numeric `id` is the handle item_click "
     "always accepts.",
     "depth, ref", remote::UiAction},
    {"scroll_panel",
     "UI test tier: scroll a docked panel {window, to: top|bottom|<y>}; "
     "returns the widget list afterwards. This is for SEEING — a screenshot "
     "shows only what is on screen, so the bottom of a long panel needs "
     "scrolling to appear in one. It is not needed to reach a widget: no "
     "panel here uses a list clipper, so ui_items lists items below the fold "
     "and item_click scrolls them into view by itself. (`scroll` is a "
     "different thing: it zooms the 3D viewport.)",
     "to, window", remote::UiAction},
    {"focus_panel",
     "UI test tier: bring a docked panel's TAB forward {name}, e.g. \"CAM\", "
     "\"Model\", \"History\". A panel tabbed behind another is invisible to "
     "item_click, so without this the only panels a script could drive were "
     "whichever the saved layout happened to leave in front.",
     "name", remote::UiAction},
    {"viewport_click",
     "Click the 3D viewport at (x, y) as 0-1 fractions OF THE VIEWPORT "
     "rect (0.5,0.5 = its centre, whatever is docked around it).",
     "button, ctrl, x, y", remote::UiAction},
    {"viewport_drag",
     "Drag in the viewport (button 0/1/2) — orbit/pan. From (x0, y0) to "
     "(x1, y1), the same 0-1 fractions of the viewport rect; the defaults "
     "drag from its centre a tenth of the way out.",
     "button, ctrl, shift, x0, x1, y0, y1", remote::UiAction},
    {"scroll", "Scroll (zoom) the viewport.", "amount, dy, window",
     remote::UiAction},
};

// Every method's row, by name.
//
// This replaced four separate std::set<std::string> -- kNoPartDoc (166 names),
// kNotInBatch (31), kDeferrable (10) and IsUiAction -- none of which sat next
// to a handler, so adding a method meant remembering four places and no test
// compared them. Building it found three names that were in those lists for
// methods that DO NOT EXIST (export_mesh, move_feature, rename_feature): dead
// entries a list apart from its handlers accumulates in silence.
// docs/architecture.md#the-remote-json-rpc.
const remote::MethodDoc* FindMethod(const std::string& m) {
  static const std::map<std::string, const remote::MethodDoc*> byName = [] {
    std::map<std::string, const remote::MethodDoc*> out;
    const auto add = [&out](std::span<const remote::MethodDoc> docs) {
      for (const remote::MethodDoc& d : docs)
        out.emplace(d.name, &d);
    };
    add(kServerMethods);
    add(remote::ModelingMethodDocs());
    add(remote::ProjectMethodDocs());
    add(remote::AssemblyMethodDocs());
    add(remote::MotionMethodDocs());
    add(remote::InspectionMethodDocs());
    add(remote::CatalogMethodDocs());
    add(remote::CamMethodDocs());
    add(remote::DrawingMethodDocs());
    add(remote::ConfigurationMethodDocs());
    add(remote::SheetMetalMethodDocs());
    add(remote::TubeMethodDocs());
    add(remote::SimulationMethodDocs());
    add(remote::RequirementMethodDocs());
    return out;
  }();
  const auto it = byName.find(m);
  return it == byName.end() ? nullptr : it->second;
}

// The flags of `m`, or 0 for a method we do not know. An unknown method having
// no flags is what keeps the part gate below quiet about it -- a typo must
// fall through to "unknown method", not be answered with "switch to a part
// tab", which sends the caller hunting a problem that does not exist.
uint8_t MethodFlags(const std::string& m) {
  const remote::MethodDoc* d = FindMethod(m);
  return d == nullptr ? uint8_t{0} : d->flags;
}

// Every method any module documents. The describe tables are the registry
// (a branch without a row is caught by its own test), so this needs no
// second list to drift out of step.
static bool IsKnownMethod(const std::string& m) {
  static const std::set<std::string> known = [] {
    std::set<std::string> s;
    const auto add = [&s](std::span<const remote::MethodDoc> docs) {
      for (const auto& d : docs)
        s.insert(d.name);
    };
    add(kServerMethods);
    add(remote::ModelingMethodDocs());
    add(remote::ProjectMethodDocs());
    add(remote::AssemblyMethodDocs());
    add(remote::MotionMethodDocs());
    add(remote::InspectionMethodDocs());
    add(remote::CatalogMethodDocs());
    add(remote::CamMethodDocs());
    add(remote::DrawingMethodDocs());
    add(remote::ConfigurationMethodDocs());
    add(remote::SheetMetalMethodDocs());
    add(remote::TubeMethodDocs());
    add(remote::SimulationMethodDocs());
    add(remote::RequirementMethodDocs());
    return s;
  }();
  return known.count(m) != 0;
}

json RemoteServer::HandleSemantic(const Request& request, ui::AppState& state,
                                  bool& handled, remote::RpcParams& p) {
  const std::string& m = request.method;
  Document* doc = state.document.get();
  handled = true;
  remote::ValidateInspectionRequest(m, request.params);

  // UI actions (item_click, viewport_click, …) fall through to QueueUiAction,
  // not HandleSemantic, so don't guard them here — they drive the assembly tab.
  //
  // An UNKNOWN method falls through too, or the gate answers a typo with
  // "switch to a part tab" and sends the caller hunting a tab problem that
  // does not exist. Whether a method needs a part is only a sensible thing
  // to say about a method that exists.
  if (!doc && (MethodFlags(m) & remote::NeedsPart)) {
    // Named, every kind: the fall-through used to read "the active document
    // is not a part, not a part" on a CAM job's tab.
    const char* kind = state.assembly  ? "an assembly"
                       : state.drawing ? "a drawing"
                       : state.job     ? "a CAM job"
                                       : "empty";
    throw std::runtime_error("the active document is " + std::string(kind) +
                             ", not a part; switch to a part tab");
  }

  if (m == "batch") {
    // One transaction, one regeneration, one undo step
    // (docs/scripting/rpc.md#batch). The gripper dogfood spent 86 calls on
    // two parts and an assembly, each paying a full regeneration of a
    // 200-body part; adding the features and regenerating once is the same
    // document for a fraction of the work (Document::BulkEdit's own
    // rationale, which had no way in from here).
    if (m_inBatch)
      throw std::runtime_error(
          "batch does not nest — put the inner calls in the outer list");
    const json& calls = p.at("calls");
    const bool stopOnError = p.value("stop_on_error", true);
    if (!calls.is_array())
      throw std::runtime_error(
          "batch 'calls' is an array of {method, params} objects");
    if (calls.empty())
      throw std::runtime_error("batch 'calls' is empty — nothing to run");

    for (size_t i = 0; i < calls.size(); i++) {
      if (!calls[i].is_object() || !calls[i].contains("method"))
        throw std::runtime_error("batch call " + std::to_string(i + 1) +
                                 " is not {method, params}");
      const std::string sub = calls[i].at("method").get<std::string>();
      if (sub == "batch")
        throw std::runtime_error(
            "batch does not nest — put the inner calls in the outer list. "
            "The document is unchanged.");
      if (MethodFlags(sub) & remote::NotInBatch)
        throw std::runtime_error(
            "'" + sub +
            "' cannot run inside a batch: a batch rolls the DOCUMENT back on "
            "failure and this reaches outside it (a file, the git tree, the "
            "window, or the active tab). Call it before or after the batch. "
            "The document is unchanged.");
      if (IsUiAction(sub))
        throw std::runtime_error(
            "'" + sub +
            "' is a UI action, which drives the real window through the test "
            "engine and cannot be rolled back: call it outside the batch. The "
            "document is unchanged.");
      if (!IsKnownMethod(sub))
        throw std::runtime_error("batch call " + std::to_string(i + 1) +
                                 ": no method '" + sub +
                                 "' (see describe). The document is "
                                 "unchanged.");
      // Every call's keys against its row before ANY call runs: a $ref
      // replaces a value, never a key, so this is already the final shape.
      if (calls[i].contains("params"))
        if (auto refusal = UndeclaredParamsError(sub, calls[i].at("params")))
          throw std::runtime_error("batch call " + std::to_string(i + 1) +
                                   ": " + *refusal +
                                   " The document is unchanged.");
    }

    // The marks a rollback rewinds to. Both are taken before anything runs:
    // a part batch unwinds its commands, an assembly batch restores the
    // snapshot its first mutation pushed.
    const EditMarks marks = MarkEdits(state);
    const size_t undoMark = marks.commandDepth;
    const size_t asmMark = marks.assemblyUndo;

    json results = json::array();
    json failures = json::array();
    // Each call's result by index, for {"$ref": n}. Null where a call failed
    // or has not run; a reference into either is refused rather than
    // resolving to nothing.
    std::vector<json> byIndex(calls.size(), json());
    size_t regenerations = 0;
    std::optional<Document::BulkEdit> bulk;
    Document* bulkDoc = nullptr;
    const auto flush = [&] {
      if (bulk) {
        bulk.reset(); // the guard's release IS the regeneration
        bulkDoc = nullptr;
        regenerations++;
      }
    };

    m_inBatch = true;
    struct Guard {
      bool& flag;
      ~Guard() { flag = false; }
    } guard{m_inBatch};

    std::string fatal;
    for (size_t i = 0; i < calls.size(); i++) {
      const std::string sub = calls[i].at("method").get<std::string>();
      json subParams =
          calls[i].contains("params") ? calls[i].at("params") : json::object();
      // {"$ref": n} anywhere in the params is the result of call n — by
      // default its `id`, or {"$ref": n, "key": "..."} for another field.
      // Without this a batch could not express the API's commonest pair, a
      // sketch and the extrude that names it, and the whole verb would be
      // limited to calls that happen not to depend on each other.
      std::function<json(const json&)> resolveRefs =
          [&](const json& v) -> json {
        if (v.is_array()) {
          json out = json::array();
          for (const json& e : v)
            out.push_back(resolveRefs(e));
          return out;
        }
        if (!v.is_object())
          return v;
        if (v.contains("$ref")) {
          const json& which = v.at("$ref");
          if (!which.is_number_unsigned() || which.get<size_t>() == 0)
            throw std::runtime_error(
                "$ref takes the 1-based number of an EARLIER call in this "
                "batch, e.g. {\"$ref\": 1}");
          const size_t n = which.get<size_t>();
          if (n > i)
            throw std::runtime_error(
                "$ref names call " + std::to_string(n) +
                ", which has not run yet — a $ref points BACKWARD (this is "
                "call " +
                std::to_string(i + 1) + ")");
          if (byIndex[n - 1].is_null())
            throw std::runtime_error("$ref names call " + std::to_string(n) +
                                     ", which failed, so it returned nothing");
          const std::string key = v.value("key", std::string("id"));
          if (!byIndex[n - 1].is_object() || !byIndex[n - 1].contains(key)) {
            std::vector<std::string> had;
            if (byIndex[n - 1].is_object())
              for (auto it = byIndex[n - 1].begin(); it != byIndex[n - 1].end();
                   ++it)
                had.push_back(it.key());
            throw std::runtime_error(
                "call " + std::to_string(n) + " returned no '" + key +
                "'; it returned: " + remote::KnownList(had));
          }
          return byIndex[n - 1].at(key);
        }
        json out = json::object();
        for (auto it = v.begin(); it != v.end(); ++it)
          out[it.key()] = resolveRefs(it.value());
        return out;
      };
      if ((MethodFlags(sub) & remote::Deferrable) && state.document) {
        if (bulkDoc != state.document.get()) {
          flush();
          bulk.emplace(*state.document);
          bulkDoc = state.document.get();
        }
      } else {
        flush();
      }
      try {
        subParams = resolveRefs(subParams);
        bool subHandled = false;
        remote::RpcParams subP{subParams};
        Request subRequest;
        subRequest.method = sub;
        subRequest.params = subParams;
        json r = HandleSemantic(subRequest, state, subHandled, subP);
        if (!subHandled)
          throw std::runtime_error(
              "'" + sub +
              "' is not a semantic method (it needs the GUI or a renderer) — "
              "call it outside the batch");
        // The same strict check the single-call path runs. Inside a
        // transaction it is not "applied, then rejected": the batch rolls
        // back, so a typo'd key leaves nothing behind.
        if (auto unknown = subP.UnknownKeys(); !unknown.empty())
          throw std::runtime_error(
              UnknownParamError(sub, unknown, subP.SeenKeys()));
        // Geometry is answered ONCE, by the batch's own regen. A per-call
        // summary here would be read from a document that has not
        // regenerated yet — the honest thing is not to send one.
        if (r.is_object()) {
          r.erase("regen");
          r.erase("digest");
        }
        byIndex[i] = r;
        results.push_back({{"call", i + 1}, {"method", sub}, {"result", r}});
      } catch (const std::exception& e) {
        json failure{{"call", i + 1}, {"method", sub}, {"error", e.what()}};
        if (const auto* contract =
                dynamic_cast<const remote::ContractError*>(&e))
          failure["diagnostic"] = contract->diagnostic;
        failures.push_back(failure);
        if (stopOnError) {
          fatal = "batch call " + std::to_string(i + 1) + " (" + sub +
                  ") failed: " + e.what();
          break;
        }
        results.push_back(std::move(failure));
      }
    }
    flush();

    if (!fatal.empty()) {
      // Roll the whole thing back: the commands this batch pushed, and the
      // assembly state its first mutation snapshotted.
      // ...and the drawing's and the job's: every snapshot history the
      // calls pushed onto.
      const std::string undone = RollBackEdits(state, marks);
      const std::string message =
          fatal + ". The batch was rolled back: " +
          (undone.empty() ? std::string("nothing had changed") : undone) +
          ", so the document is as it was before the batch. " +
          std::to_string(calls.size() - results.size() - 1) +
          " later call(s) did not run.";
      if (std::any_of(failures.begin(), failures.end(),
                      [](const json& failure) {
                        return failure.contains("diagnostic");
                      })) {
        remote::ContractError error("batch", "batch_failed", "/params/calls",
                                    "all calls valid", failures, message);
        error.diagnostic["failures"] = failures;
        throw error;
      }
      throw std::runtime_error(message);
    }

    // One undo step for the whole batch, and one assembly snapshot.
    if (state.commands)
      state.commands->CoalesceSince(
          undoMark, "Batch of " + std::to_string(calls.size()) + " calls");
    if (state.assemblyHistory.undo.size() > asmMark + 1) {
      state.assemblyHistory.undo.resize(asmMark + 1);
      state.assemblyHistory.undoStamps.resize(asmMark + 1);
    }

    json out{{"calls", calls.size()},
             {"results", std::move(results)},
             {"regenerations", regenerations}};
    if (!failures.empty())
      out["failed"] = std::move(failures);
    if (state.document)
      out["regen"] = remote::RegenSummaryJson(*state.document);
    return out;
  }

  if (m == "describe") {
    // API introspection: every method with a one-line doc, the feature/entity/
    // constraint dialects, and enum values — so clients (agents, the MCP
    // wrapper) discover the surface without grepping the source. Static
    // metadata only; safe with no document.
    // `method` narrows to one entry. Three separate dogfooding agents
    // reached for describe{method: ...} and found describe took no arguments
    // at all, so the whole 195-method surface came back every time — and
    // still without saying what any of them takes.
    const std::string only = p.value("method", std::string{});
    json methods = json::array();
    const auto addDocs = [&](std::span<const remote::MethodDoc> docs) {
      for (const auto& d : docs) {
        if (!only.empty() && only != d.name)
          continue;
        json entry = {
            {"name", d.name},
            {"doc", d.doc},
            {"requires_part_doc", (d.flags & remote::NeedsPart) != 0}};
        // "" = takes nothing; "*" = a free-form spec read opaquely, where
        // the strict unknown-key check is off and prose is the only guide.
        const std::string ps = d.params ? d.params : "";
        if (ps == "*") {
          entry["params_free_form"] = true;
        } else {
          json keys = json::array();
          for (size_t i = 0, j = 0; i <= ps.size(); i++) {
            if (i == ps.size() || ps[i] == ',') {
              std::string k = ps.substr(j, i - j);
              const size_t b = k.find_first_not_of(" ");
              const size_t e = k.find_last_not_of(" ");
              if (b != std::string::npos)
                keys.push_back(k.substr(b, e - b + 1));
              j = i + 1;
            }
          }
          entry["params"] = std::move(keys);
        }
        // What comes BACK. Kept as the row wrote it — `bodies[]{name,
        // volume}`, `total{mass_g}`, `optional?` — because the nesting is
        // the half a caller gets wrong, and flattening it to a key list
        // would throw away exactly that. Required of every list_*, get_*
        // and check_* method and gated by tests/rpc_returns_gate.py; absent
        // elsewhere, where the answer is the `regen` summary the prose
        // already describes.
        if (d.returns != nullptr && *d.returns != '\0')
          entry["returns"] = d.returns;
        const auto& contracts = remote::InspectionContracts().at("methods");
        if (contracts.contains(d.name)) {
          entry["request_schema"] = contracts[d.name]["request"];
          entry["result_schema"] = contracts[d.name]["result"];
        }
        methods.push_back(std::move(entry));
      }
    };
    const std::span<const remote::MethodDoc> tables[] = {
        kServerMethods,
        remote::ModelingMethodDocs(),
        remote::ProjectMethodDocs(),
        remote::AssemblyMethodDocs(),
        remote::MotionMethodDocs(),
        remote::InspectionMethodDocs(),
        remote::CatalogMethodDocs(),
        remote::CamMethodDocs(),
        remote::DrawingMethodDocs(),
        remote::ConfigurationMethodDocs(),
        remote::SheetMetalMethodDocs(),
        remote::TubeMethodDocs(),
        remote::SimulationMethodDocs(),
        remote::RequirementMethodDocs()};
    for (const auto& t : tables)
      addDocs(t);
    if (!only.empty() && methods.empty()) {
      std::vector<std::string> names;
      for (const auto& t : tables)
        for (const auto& d : t)
          names.emplace_back(d.name);
      const std::vector<std::string> close = CloseNames(only, names);
      std::string msg = "unknown method '" + only + "'";
      if (!close.empty()) {
        msg += "; close: ";
        for (size_t i = 0; i < close.size(); i++)
          msg += (i ? ", " : "") + close[i];
      }
      msg += ". describe with no arguments lists every method";
      throw std::runtime_error(msg);
    }
    // Narrowed: that method's row, and not the catalogue around it. The
    // type tables below are several times the size of any one row, and a
    // caller asking about add_mate wants add_mate — the dogfood's
    // describe(method=...) read as "the filter is ignored" because the
    // one row came back buried under every feature type and enum.
    if (!only.empty())
      return json{
          {"app", "designer"},
          {"document_format_version", DocumentIO::DocumentFormatVersion()},
          {"methods", std::move(methods)}};
    json featureTypes = json::array();
    for (const auto& ft : DocumentIO::FeatureTypes())
      featureTypes.push_back({{"type", ft.type},
                              {"since_version", ft.sinceVersion},
                              {"params", ft.params}});
    // Same shape as feature_types: an entity type that names no fields is a
    // type an agent can see and cannot use.
    json entityTypes = json::array();
    for (const auto& et : DocumentIO::SketchEntityTypes())
      entityTypes.push_back({{"type", et.type},
                             {"since_version", et.sinceVersion},
                             {"params", et.params}});
    return json{
        {"app", "designer"},
        {"inspection_contract",
         {{"version", remote::InspectionContracts()["version"]},
          {"hash", std::string(remote::kInspectionContractHash)},
          {"dialect", remote::InspectionContracts()["dialect"]}}},
        {"document_format_version", DocumentIO::DocumentFormatVersion()},
        {"methods", std::move(methods)},
        {"feature_types", std::move(featureTypes)},
        {"sketch_entity_types", std::move(entityTypes)},
        {"constraint_kinds", remote::ConstraintKindNames()},
        // The assembly vocabulary, from the parser's own tables. Without
        // `relation_kinds` here, a caller reading this endpoint to find out
        // what the app can do sees six mate types and concludes that coupled
        // DOFs -- gears, rack and pinion, a screw -- are not supported.
        {"mate_types", DocumentIO::MateTypeNames()},
        {"relation_kinds", DocumentIO::RelationKindNames()},
        {"constraint_value_units",
         [] {
           json u = json::object();
           for (const auto& [kind, unit] : remote::ConstraintValueUnits())
             u[kind] = unit;
           return u;
         }()},
        {"extrude_modes", {"new_body", "add", "cut"}},
        {"extrude_end_modes",
         {"blind", "symmetric", "two_sided", "up_to_face", "through_all"}},
        {"drawing_dim_kinds", DrawingDimKindNames()},
        {"axis_kinds", {"x", "y", "z", "sketch_line", "datum_axis", "custom"}},
        {"material_slugs", CADMaterials::MaterialLibrary::AllSlugs()}};
  }

  // Doc-level-object RPCs carved into their own module (decision 7). The
  // config RPCs target whichever tab is active — part or assembly.
  // Sheet metal is a PART-only object (the rules describe one blank), so
  // unlike the config RPCs it has no assembly half — but its READ-ONLY half
  // names the part it is asking about, and a question that names its part has
  // no business caring which tab is active. It is consulted from any tab and
  // refuses for itself when there is neither a name nor an active part.
  if (json tubeOut; remote::HandleTubeRpc(state, doc, m, p, tubeOut))
    return tubeOut;
  if (json smOut; remote::HandleSheetMetalRpc(state, doc, m, p, smOut))
    return smOut;
  // A read that names its part answers from any tab, like get_state.
  if (m == "list_configurations" && p.contains("part")) {
    json cfgOut;
    remote::HandleConfigurationRpc(
        state, *remote::ResolvePartParam(state, p.at("part")), m, p, cfgOut);
    return cfgOut;
  }
  if (doc) {
    if (json cfgOut; remote::HandleConfigurationRpc(state, *doc, m, p, cfgOut))
      return cfgOut;
  } else if (state.assembly) {
    if (json cfgOut; remote::HandleAssemblyConfigurationRpc(
            state, *state.assembly, m, p, cfgOut))
      return cfgOut;
  } else {
    // Neither tab kind (a drawing is active): recognize config methods so
    // they fail with guidance instead of falling through to
    // "unknown method".
    for (const auto& d : remote::ConfigurationMethodDocs()) {
      if (m == d.name) {
        throw std::runtime_error(
            m + " needs a part or assembly tab (the active tab is a "
                "drawing) — switch with set_active_part");
      }
    }
  }

  if (m == "ping") {
    // Enough for a client to say WHICH instance it just attached to, live
    // rather than from a file: the port, whether this build can screenshot,
    // the open project and whether it has unsaved changes. A client that
    // silently attaches to the wrong window is the failure this prevents.
    const std::string project =
        state.projectPath.empty() ? std::string{} : state.projectPath;
    return {{"ok", true},
            {"app", "designer"},
            {"port", m_port},
            // Which PROCESS: two launches handed the same port cannot tell
            // their own instance from one already listening there by the
            // port alone (tools/headless-preview.sh --start checks this).
            {"pid", remote::CurrentPid()},
            {"mode", ModeName(m_mode)},
            {"ui_actions", UiActionsAvailable()},
            {"project", project},
            {"dirty", ui::ProjectDirty(state)},
            // Which GPU answers is the first thing worth knowing about a
            // rendering bug, and until now it existed only in the log and in
            // Help > About — both out of reach of the caller writing the
            // report. It is also what lets a pixel baseline be keyed to the
            // device that produced it (tests/render_pixel_smoke.py).
            {"gpu", state.gpuName},
            // What this BUILD contains. Optional subsystems are compile-time
            // (`cmake -DENABLE_CAM=OFF`), so a method can be absent for a
            // reason that has nothing to do with the document or the tab, and
            // "unknown method: list_machines" gives a caller no way to tell
            // the two apart. A test or an agent gates on this rather than
            // guessing from a failure.
            {"features",
             {{"cam",
#ifdef HAVE_CAM
               true
#else
               false
#endif
              },
              {"cam_adaptive",
#ifdef HAVE_CAM_ADAPTIVE
               true
#else
               false
#endif
              },
              {"fem",
#ifdef HAVE_FEM_SOLVER
               true
#else
               false
#endif
              },
              {"mesher",
#ifdef HAVE_NETGEN
               true
#else
               false
#endif
              },
              // Not compiled in: an external program on PATH, looked for
              // now, which export_motion_video needs for mp4/webm/gif.
              {"ffmpeg", !ui::FfmpegPath().empty()}}},
            // Whether this instance is still running the binary that is on
            // disk. A rebuild REPLACES the file; a process already running
            // keeps the old inode, serves happily, and answers questions
            // about a build nobody can look at any more. That cost this
            // project two diagnosis detours in one day — once concluding a
            // feature that had just been added did not work, because the
            // instance answering had been started before it was.
            //
            // Linux makes it a one-liner: /proc/self/exe still resolves, and
            // the kernel appends " (deleted)" when the file it named is gone.
            // No build stamp to keep in step with anything.
            {"binary_replaced", BinaryReplaced()}};
  }
  if (m == "background_jobs") {
    json jobs = json::array();
    for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
      jobs.push_back({{"id", j.id},
                      {"kind", j.kind},
                      {"label", j.label},
                      {"fraction", j.fraction},
                      {"cancellable", j.cancellable},
                      {"elapsed_seconds", j.elapsedSeconds}});
    return json{{"jobs", jobs}};
  }
  if (m == "cancel_background_job") {
    const std::string kind = p.at("kind").get<std::string>();
    if (!ui::BackgroundJobRunning(state, kind)) {
      std::string running;
      for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
        running += (running.empty() ? "" : ", ") + j.kind;
      throw std::runtime_error(
          "no background job of kind '" + kind + "' is running" +
          (running.empty() ? std::string(" (nothing is)")
                           : " (running: " + running + ")"));
    }
    // The domain gets to say what cancelling means for what was queued
    // behind the running piece.
    if (kind == "cam")
      ui::CancelCamPasses(state);
    const size_t asked = ui::CancelBackgroundJobs(state, kind);
    bool cancellable = false;
    for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
      if (j.kind == kind)
        cancellable |= j.cancellable;
    return json{{"asked", asked}, {"cancellable", cancellable}};
  }
  if (m == "get_state") {
    // Another part, read without activating it. `doc` is the active tab's;
    // the fields below that are about the DOCUMENT follow `stateDoc`, and the
    // ones about the tab (selection, camera, history) stay with the tab.
    Document* stateDoc = doc;
    if (p.contains("part"))
      stateDoc = remote::ResolvePartParam(state, p.at("part"));
    // Off the part gate now (it has to be, to answer for another part), so it
    // owns the refusal: on an assembly tab with no `part` there is no feature
    // list to describe, and saying so beats a null dereference.
    if (stateDoc == nullptr)
      throw std::runtime_error(
          std::string("the active document is ") +
          (state.assembly  ? "an assembly"
           : state.drawing ? "a drawing"
           : state.job     ? "a CAM job"
                           : "not a part") +
          ", which has no feature list — pass `part` to read one (see "
          "list_parts), or switch to a part tab");
    json features = json::array();
    for (size_t i = 0; i < stateDoc->FeatureCount(); i++) {
      json f = DocumentIO::FeatureToJson(*stateDoc->GetFeature(i));
      f["index"] = i;
      // Regeneration errors, so remote callers see WHY a feature contributed
      // nothing instead of silently reading stale bodies.
      if (!stateDoc->GetFeature(i)->GetError().empty()) {
        f["error"] = stateDoc->GetFeature(i)->GetError();
        // Failure downstream of another failure — usually collateral; fix
        // the first red feature first.
        if (stateDoc->GetFeature(i)->ErrorAfterEarlierFailure()) {
          f["after_earlier_failure"] = true;
        }
      }
      // Non-fatal advisory (e.g. a cut that intersected nothing): the feature
      // built a valid result, but likely not the intended one.
      if (!stateDoc->GetFeature(i)->GetWarning().empty()) {
        f["warning"] = stateDoc->GetFeature(i)->GetWarning();
      }
      // A Derived copy says whether its source has moved (v84): true/false,
      // or null when the source is not loaded — unknown is not fresh.
      if (auto* d =
              dynamic_cast<const DerivedFeature*>(stateDoc->GetFeature(i))) {
        const std::optional<bool> stale = ui::DerivedStale(state, *d);
        f["stale"] = stale.has_value() ? json(*stale) : json(nullptr);
      }
      // A worm or a wheel says what it built: the sizes that follow from
      // its blank, the lead angle and whether the pair self-locks.
      if (auto* w = dynamic_cast<const WormFeature*>(stateDoc->GetFeature(i)))
        if (json rep = w->ReportJson(); !rep.is_null())
          f["report"] = rep;
      if (auto* w =
              dynamic_cast<const WormWheelFeature*>(stateDoc->GetFeature(i)))
        if (json rep = w->ReportJson(); !rep.is_null())
          f["report"] = rep;
      features.push_back(f);
    }
    json bodies = json::array();
    // A windowed instance projects every frame regardless, so pulling it
    // here is version-gated to nothing — and it closes the narrow window
    // where an RPC edit is answered before the frame that would have
    // projected it, which would make `mesh` blink out of a GUI session's
    // get_state. --offscreen must NOT be pulled here: this is the hot poll,
    // and forcing the projection there is exactly what made an edit on a
    // 200-body part cost seconds.
    if (m_mode == RemoteMode::Gui)
      ui::SyncRenderersToDocument(state);
    // Asked once, not per body: do the render ids below describe THIS version
    // of the document?
    const bool renderMeshesCurrent = ui::RenderMeshesCurrent(state);
    for (size_t i = 0; i < stateDoc->GetBodies().size(); i++) {
      const auto& body = stateDoc->GetBodies()[i];
      glm::dvec3 min, max;
      // The CHEAP box, which is conservative rather than exact: a swept
      // surface is bounded by its control points, so a helical spring reads
      // about sixty per cent bigger than it is. get_state is called after
      // every edit — asking for the exact one here took an engine's build
      // from 1.6 s to 5.8 s — and a bound that cannot be too SMALL is what
      // most callers of this field want. `mass_properties` returns the
      // measured box for the times you want a size.
      body.shape.GetBoundingBox(min, max);
      // `created_by` is here because it is FREE here and expensive
      // anywhere else: it was only ever readable from list_elements, which
      // enumerates every face and edge of the body to hand back one
      // integer. The SDK reads it after each extrude to return the body it
      // made, so a cylinder head paid 341 ms a time, 161 times, for 55 s of
      // full surface analysis it never looked at.
      json entry = {
          {"name", body.name},           {"volume", body.shape.Volume()},
          {"material", body.material},   {"created_by", body.createdBy},
          {"bbox_min", Vec3ToJson(min)}, {"bbox_max", Vec3ToJson(max)}};
      if (body.reference) // a Derived ghost: drawn on, weighed as nothing
        entry["reference"] = true;
      if (body.appearance.Any()) { // v54 visual override, set_appearance
        json a = json::object();
        if (body.appearance.color)
          a["color"] = Vec3ToJson(*body.appearance.color);
        if (body.appearance.roughness)
          a["roughness"] = *body.appearance.roughness;
        if (body.appearance.metallic)
          a["metallic"] = *body.appearance.metallic;
        entry["appearance"] = std::move(a);
      }
      // Render/picking mesh id (used by select_face); ids change on
      // regeneration, so always read them fresh from here. OMITTED rather
      // than guessed when the projection is behind the document (--offscreen
      // projects lazily): the list is still the right LENGTH after an edit,
      // so an index would read back a plausible id for superseded geometry —
      // select_face would then pick a real face that is not this one. An
      // absent key is a loud failure; a wrong id is a silent one.
      if (renderMeshesCurrent && i < state.documentMeshes.size()) {
        entry["mesh"] = state.documentMeshes[i].meshId;
      }
      bodies.push_back(entry);
    }
    // Legacy single-face block, now derived from the Selection (not the
    // FaceSelector mirror, which only updates when a frame runs — RPC
    // batches would read stale state).
    json selection = nullptr;
    if (const ui::SelectionItem* it =
            state.selection.First(ui::SelectionItem::Kind::Face)) {
      uint32_t mesh = 0, face = 0;
      if (state.faceSelector && ui::ResolveFaceItem(state, *it, mesh, face)) {
        selection = {
            {"mesh", mesh},
            {"face", face},
            {"normal",
             Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
            {"center",
             Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
      }
    }
    // The typed Selection (U1): every item, stable refs only.
    json selectionItems = json::array();
    for (const ui::SelectionItem& it : state.selection.items) {
      const char* kind =
          it.kind == ui::SelectionItem::Kind::Face          ? "face"
          : it.kind == ui::SelectionItem::Kind::Edge        ? "edge"
          : it.kind == ui::SelectionItem::Kind::Body        ? "body"
          : it.kind == ui::SelectionItem::Kind::Feature     ? "feature"
          : it.kind == ui::SelectionItem::Kind::Occurrence  ? "occurrence"
          : it.kind == ui::SelectionItem::Kind::Mate        ? "mate"
          : it.kind == ui::SelectionItem::Kind::Relation    ? "relation"
          : it.kind == ui::SelectionItem::Kind::Connector   ? "connector"
          : it.kind == ui::SelectionItem::Kind::Operation   ? "operation"
          : it.kind == ui::SelectionItem::Kind::SketchCurve ? "sketch_curve"
          : it.kind == ui::SelectionItem::Kind::CamSetup    ? "cam_setup"
          : it.kind == ui::SelectionItem::Kind::JobPart     ? "job_part"
          : it.kind == ui::SelectionItem::Kind::Fixture     ? "fixture"
          : it.kind == ui::SelectionItem::Kind::CamTool     ? "cam_tool"
                                                            : "sketch_region";
      json entry = {{"kind", kind}};
      if (it.id != 0)
        entry["id"] = it.id;
      // A moving part of a flexible placement: id is the placement, path
      // the part inside it (docs/assemblies/README.md#flexible-sub-assemblies).
      if (it.kind == ui::SelectionItem::Kind::Occurrence &&
          !it.innerPath.empty())
        entry["path"] = it.innerPath;
      if (!it.element.name.empty())
        entry["element"] = {{"feature_id", it.element.featureId},
                            {"name", it.element.name}};
      selectionItems.push_back(entry);
    }
    return {{"features", features},
            {"bodies", bodies},
            {"selection", selection},
            {"selection_items", selectionItems},
            {"selected_feature", ui::SelectedFeatureIndexOrMinus1(state)},
            {"camera",
             // `fov` (VERTICAL, degrees) and `aspect` are here so a caller
             // can reproduce the projection and check its own framing.
             // Without them the block said where the camera is and not what
             // it sees, and a script could not tell a fitted view from a
             // cropped one (docs/rendering/still-render.md#framing).
             {{"position", Vec3ToJson(state.camera->GetPosition())},
              {"target", Vec3ToJson(state.camera->GetTarget())},
              {"distance", state.camera->GetDistance()},
              {"fov_deg", state.camera->GetFOV()},
              {"aspect", state.camera->GetAspectRatio()}}},
            {"can_undo", ui::CanGlobalUndo(state)},
            {"can_redo", ui::CanGlobalRedo(state)},
            {"rollback", stateDoc->Rollback()},
            // A GUI sketch edit hides everything built from the open sketch.
            // Reported alongside the bar, and always (-1 = off, like
            // `rollback`), because the alternative is an agent reading a body
            // list that is short by a feature with no field saying why — the
            // same honesty rule the regen summary answers to.
            {"view_rollback", stateDoc->ViewRollback()},
            {"regen",
             [&] {
               json r = remote::RegenSummaryJson(*stateDoc);
               r["executed"] = stateDoc->LastRegenStats().executed;
               r["cached"] = stateDoc->LastRegenStats().cached;
               return r;
             }()},
            {"pending_feature_id", state.pendingFeatureId}};
  }
  if (m == "rt_build") {
    // SyncRayTracingScene reads the raster sync's tessellation cache when it
    // has one, which is the difference between a copy and re-meshing every
    // body; under a lazy projection there is nothing there until we ask.
    ui::SyncRenderersToDocument(state);
    // Build the ray-tracing acceleration structures from the current document.
    // Verification hook for the offline-render work (R4).
    bool ok = ui::SyncRayTracingScene(state);
    return {{"ok", ok},
            {"geometry_count",
             state.rayTracer ? state.rayTracer->GeometryCount() : 0},
            {"has_tlas", state.rayTracer && state.rayTracer->HasTLAS()}};
  }
  if (m == "render_still") {
    // Assemblies render too: SyncRayTracingScene walks every visible
    // occurrence at its solved placement. What must still fail loudly is a
    // tab with nothing in it, which used to "succeed" with a picture of the
    // environment alone — the dock round's silently-wrong papercut.
    if (state.assembly && state.assembly->OccurrenceCount() == 0)
      throw std::runtime_error(
          "this assembly has no occurrences — there is nothing to render");
    const std::string mode = p.value("mode", std::string("path"));
    if (mode != "path" && mode != "quick")
      throw std::runtime_error("unknown mode: " + mode +
                               "; modes: path (the path tracer), quick (what "
                               "the viewport draws)");
    if (mode == "quick") {
      // What the viewport rasterizes, at this size
      // (docs/rendering/still-render.md#quick-mode). The path tracer's
      // parameters mean nothing here, and are refused rather than dropped: a
      // caller asking for 4096 samples of a raster image has the wrong idea
      // about what came back.
      for (const char* k : {"samples", "seed", "ground", "backdrop"})
        if (p.contains(k))
          throw std::runtime_error(
              std::string("`") + k +
              "` is a path-tracer setting; a quick render draws what the "
              "viewport draws (supersample sets its smoothness, outlines and "
              "line_weight its edges). Drop it, or use mode: path");
      if (!state.captureRaster)
        throw std::runtime_error(
            "a quick render needs a renderer: this instance is --headless");
      ui::AppState::RasterStill req;
      req.width = p.value("width", 1920u);
      req.height = p.value("height", 1080u);
      req.supersample = p.value("supersample", 2u);
      if (req.supersample < 1 || req.supersample > 4)
        throw std::runtime_error("supersample is 1 to 4 (the image is drawn "
                                 "that many times wider and taller)");
      req.outlines = StillOutlinesParam(p);
      // The lines' width in pixels of THIS picture, whatever the
      // supersample: the capture draws them k times wider
      // (docs/rendering/still-render.md#lines-in-a-quick-still).
      if (p.contains("line_weight")) {
        const json& lw = p["line_weight"];
        const double v = lw.is_number() ? lw.get<double>() : -1.0;
        if (!lw.is_number() || !std::isfinite(v) || v < kMinLineWeight ||
            v > kMaxLineWeight)
          throw std::runtime_error(
              "line_weight is a width in pixels of the output image, " +
              std::format("{} to {}", kMinLineWeight, kMaxLineWeight) +
              " (the viewport's is 1); got " + lw.dump());
        req.lineWeight = static_cast<float>(v);
      }
      req.path = p.value("path", std::string("render.png"));
      if (p.value("fit", false))
        ui::FitCameraToDocument(state, static_cast<float>(req.width) /
                                           static_cast<float>(req.height));
      ui::SyncRenderersToDocument(state);
      std::string error, warning;
      const auto t0 = std::chrono::steady_clock::now();
      // Captured on this (the main) thread, then averaged down and written
      // in line: the RPC answers with the file on disk.
      ui::RasterFrame frame;
      const bool ok = state.captureRaster(req, &frame, &error, &warning) &&
                      DownsampleToPng(frame.rgb, frame.width, frame.height,
                                      frame.factor, req.path, &error);
      json result = {{"ok", ok},
                     {"mode", "quick"},
                     {"path", req.path},
                     {"width", req.width},
                     {"height", req.height},
                     {"supersample", req.supersample},
                     {"outlines", req.outlines},
                     {"error", error},
                     {"seconds", std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()}};
      if (req.outlines && frame.lineWeight > 0.0f)
        result["line_weight"] = frame.lineWeight;
      if (!warning.empty())
        result["warnings"] = json::array({warning});
      return result;
    }
    if (p.contains("line_weight"))
      throw std::runtime_error(
          "`line_weight` is a quick-mode setting (mode: quick); a traced "
          "still's edges are drawn at the viewport's outline width");
    // Offline path-traced still to a PNG. Runs synchronously on the main thread
    // (ProcessPending) using the same idle-queue one-shot submits as the AS
    // build, so it needs no swapchain interaction.
    //
    // The defaults used to be 640x480 at 64 samples — a size chosen for smoke
    // tests, which every scripted and agent-driven render then inherited while
    // the GUI dialog defaulted to 1920x1080. A picture that small reads as a
    // low-resolution render of a high-resolution model, and that was most of
    // what "the exported render looks worse than the app" meant. The GUI's
    // default is the default here too now. Samples are close to free: at 720p
    // the sample count does not move the wall clock at all, and at 4K eight
    // times the samples costs 1.7x, because the fixed costs (scene sync, AS
    // build, readback, PNG encode) dominate. Tests that want the old cheap
    // render pass width/height/samples explicitly.
    uint32_t width = p.value("width", 1920u);
    uint32_t height = p.value("height", 1080u);
    uint32_t samples = p.value("samples", 512u);
    uint32_t seed = p.value("seed", 1u);
    std::string path = p.value("path", std::string("render.png"));
    bool ground = p.value("ground", true); // shadow-catcher plane
    // Frame the model for THIS picture before taking it. Off by default —
    // the camera the caller set is the camera they get — but asking for it
    // is the only way a script can frame correctly, because the still
    // projects at width/height while `fit_view` alone solves for the
    // viewport, which offscreen is whatever the framebuffer happens to be
    // (docs/rendering/still-render.md#framing). Read here, applied once the
    // keys below have all passed: a refused render leaves the camera alone.
    const bool fit = p.value("fit", false);
    // Edges over the traced image. On by default because the viewport draws
    // them and their absence is what made butted boards render as one slab;
    // false renders the bare path-traced image.
    const bool outlines = StillOutlinesParam(p);
    // "studio" (default) shows a neutral sweep; "environment" shows the HDRI
    // itself, which is what it was before there was a choice.
    const std::string backdrop = p.value("backdrop", std::string("studio"));
    if (backdrop != "studio" && backdrop != "environment") {
      throw std::runtime_error("unknown backdrop: " + backdrop +
                               "; backdrops: studio, environment");
    }
    if (fit)
      ui::FitCameraToDocument(state, height > 0 ? static_cast<float>(width) /
                                                      static_cast<float>(height)
                                                : 0.0f);
    // As rt_build: the still is built from the raster tessellation cache when
    // there is one, and a lazy projection has not filled it yet.
    ui::SyncRenderersToDocument(state);
    std::string error, warning;
    bool ok = ui::RenderStillBlocking(state, width, height, samples, path, seed,
                                      ground, &error, &warning,
                                      backdrop == "studio", outlines);
    json result = {{"ok", ok},         {"path", path},       {"width", width},
                   {"height", height}, {"samples", samples}, {"error", error}};
    // The postcondition a screenshot already carries: saying "ok" about a
    // picture of nothing is the confident wrong answer this codebase keeps
    // paying for.
    if (ok && !warning.empty())
      result["warnings"] = json::array({warning});
    return result;
  }
  // Part-document domain modules (disjoint method sets; order doesn't matter).
  if (doc) {
    if (auto r = remote::HandleModelingRpc(state, *doc, m, p))
      return *r;
  }
  // Studies live on a part OR an assembly (S1): the handler takes a nullable
  // part and finds the assembly itself.
  if (auto r = remote::HandleSimulationRpc(state, doc, m, p))
    return *r;
  // Inspection takes a nullable doc: interference works on assembly
  // occurrence pairs, list_elements/measure on any part via `part`.
  if (auto r = remote::HandleCamRpc(state, doc, m, p))
    return *r;
  if (auto r = remote::HandleCatalogRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleInspectionRpc(state, doc, m, p))
    return *r;
  if (m == "undo") {
    // Global undo (Phase V): the active tab's stack OR the cross-tab
    // project-variable stack, whichever was most recently mutated.
    //
    // Undo regenerates, so it can leave a feature broken — stepping back past
    // the edit that gave a fillet an edge to bite on, say. It used to report
    // only `can_undo`, so a script learned nothing about the state it had just
    // restored. `state.document` is re-read AFTER the undo: a tab switch is
    // undoable, so the active document may not be the one captured above.
    ui::GlobalUndo(state);
    json out = {{"can_undo", ui::CanGlobalUndo(state)}};
    remote::AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "redo") {
    ui::GlobalRedo(state);
    json out = {{"can_redo", ui::CanGlobalRedo(state)}};
    remote::AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "list_parts") {
    // Multi-document project (assemblies A1/A2): the tabs and which is active.
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      // A CAM job is its own kind of document and says so. It used to fall
      // through to "part", so a client filtering for parts was handed a job
      // and every part call against it was then refused ("the active
      // document is a CAM job") -- with nothing in the listing to explain
      // why.
      const char* type =
          state.parts[i].kind == ui::DocKind::Assembly  ? "assembly"
          : state.parts[i].kind == ui::DocKind::Drawing ? "drawing"
          : state.parts[i].kind == ui::DocKind::Job     ? "job"
                                                        : "part";
      json entry = {{"name", state.parts[i].name},
                    {"index", i},
                    {"id", state.parts[i].id},
                    {"type", type}};
      if (!state.parts[i].folder.empty())
        entry["folder"] = state.parts[i].folder;
      arr.push_back(std::move(entry));
    }
    // `active` is the INDEX (what it has always been); `active_id` is the
    // same tab's stable id, which is what every other call and every client
    // handle uses. Without it "which tab is in front?" needed the index and
    // the list to be cross-referenced by hand, and a client caching the
    // active tab had no way to check itself
    // (generators/README.md#single-cylinder-and-air-cooled-engines).
    json out = {{"parts", arr},
                {"active", state.activePart},
                {"active_id",
                 state.activePart >= 0 &&
                         state.activePart < static_cast<int>(state.parts.size())
                     ? state.parts[state.activePart].id
                     : 0}};
    if (!state.tabFolders.empty()) // tab-bar folder registry (may hold empty)
      out["folders"] = state.tabFolders;
    return out;
  }
  if (m == "add_part") {
    // A duplicate name is refused (names address tabs); the DEFAULT skips
    // taken names, since "Part parts.size()+1" collided after a deletion.
    std::string name;
    if (p.contains("name")) {
      name = p.at("name").get<std::string>();
      remote::RequireFreshTabName(state, name);
    } else {
      name = remote::FreshDefaultTabName(state, "Part");
    }
    const size_t idx = ui::AddPart(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "add_assembly") {
    std::string name;
    if (p.contains("name")) {
      name = p.at("name").get<std::string>();
      remote::RequireFreshTabName(state, name);
    } else {
      name = remote::FreshDefaultTabName(state, "Assembly");
    }
    const size_t idx = ui::AddAssembly(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "rename_part") {
    // Rename an existing tab (part/assembly/drawing) so the default "Part 1"
    // isn't permanent — it feeds BOM + add_occurrence. Occurrences reference
    // parts by stable id, so a name change never breaks a reference. `part`
    // resolves by name, stable id, or slot index. params: {part, new_name}
    const size_t slot = remote::ResolveSlotIndex(state, p.at("part"));
    const std::string newName = p.at("new_name").get<std::string>();
    if (newName.empty())
      throw std::runtime_error("new_name must be non-empty");
    if (state.parts[slot].name != newName) // renaming to itself is a no-op
      remote::RequireFreshTabName(state, newName);
    state.parts[slot].name = newName;
    state.projectMetaDirty = true; // tab names live in project.json only
    return {{"index", slot}, {"name", newName}, {"id", state.parts[slot].id}};
  }
  if (auto r = remote::HandleDrawingRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleMotionRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleAssemblyRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleProjectRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleRequirementsRpc(state, m, p))
    return *r;
  if (m == "fit_view") {
    // The aspect of the PICTURE being framed, when that is not the viewport.
    // A `render_still` projects at the size it was asked for, so a fit solved
    // for the viewport crops it (docs/rendering/still-render.md#framing).
    // Both or neither: a width without a height is an aspect nobody stated,
    // and applying half of it silently frames for the wrong picture.
    const bool hasW = p.contains("width"), hasH = p.contains("height");
    if (hasW != hasH)
      throw std::runtime_error(
          "fit_view takes width AND height together — they are the size of "
          "the picture being framed, and one alone is not an aspect");
    float aspect = 0.0f;
    if (hasW) {
      const double w = p.at("width").get<double>();
      const double h = p.at("height").get<double>();
      if (!(w > 0.0) || !(h > 0.0))
        throw std::runtime_error("fit_view width and height are pixels, both "
                                 "greater than zero");
      aspect = static_cast<float>(w / h);
    }
    ui::FitCameraToDocument(state, aspect);
    return json::object();
  }
  if (m == "view") {
    // Standard view, snapped instantly (no animation) so the very next
    // screenshot captures the final framing.
    // params: {name: front|back|left|right|top|bottom|iso, fit?: true}
    const std::string name = p.at("name").get<std::string>();
    if (!ApplyStandardView(state, name, p.value("fit", true)))
      throw std::runtime_error(
          "unknown view: " + name +
          "; views: front, back, left, right, top, bottom, iso");
    return {{"position", Vec3ToJson(state.camera->GetPosition())},
            {"target", Vec3ToJson(state.camera->GetTarget())},
            {"distance", state.camera->GetDistance()},
            {"fov_deg", state.camera->GetFOV()},
            {"aspect", state.camera->GetAspectRatio()}};
  }
  if (m == "frame_stats") {
    // The door that matters for agents and CI: a HUD nobody is looking at
    // proves nothing
    // (docs/rendering/frame-pacing.md#the-frame-time-instrument).
    if (p.contains("hud"))
      state.perfHudOpen = p.at("hud").get<bool>();
    // Start a fresh measurement window and say so, rather than returning an
    // average of the configuration you just changed and the one before it.
    if (p.value("reset", false)) {
      state.gpuStatsReset = true;
      // Clear what a reader sees NOW, not when the loop next runs: the flag
      // reaches the timer's ring a tick later, and a caller that asks again
      // in the same batch of requests would otherwise be handed the very
      // samples it just discarded.
      state.gpuStats = {};
      return {{"reset", true},
              {"note", "GPU pass samples cleared; render again, then ask"}};
    }
    const size_t window = p.value("window", render::FrameTimer::kHistory);
    const render::FrameTimer::Stats fs = state.frameTimer.Snapshot(window);
    if (m_mode == RemoteMode::Offscreen) {
      // No frame loop here — this mode draws only when asked — so there is no
      // fps, no phase split and none of them are reported as zero, which
      // would read as "infinitely fast" to anything that plots it. What IS
      // measured is the part nothing else can measure honestly: with no
      // swapchain there is no image to wait for, so `main` is shading and not
      // the display stalling inside it
      // (docs/rendering/frame-pacing.md#measuring-a-pass-offscreen). Each
      // capture is one sample. {renders: N} measures N renders taken back to
      // back, saving nothing: the PNG encode between captures is what leaves
      // the GPU idle long enough for a laptop to drop its clocks, so a burst
      // measures the card as it runs in a real frame loop rather than at its
      // idle floor.
      if (p.contains("renders")) {
        const long want = p.at("renders").get<long>();
        if (want <= 0 || want > 10000)
          throw std::runtime_error(
              "renders must be between 1 and 10000 (it is a count of frames "
              "to draw, back to back, for the measurement)");
        if (!state.renderBurst)
          throw std::runtime_error("this build cannot render a burst here");
        state.renderBurst(static_cast<size_t>(want));
      }
      if (state.gpuStats.samples == 0)
        throw std::runtime_error(
            "nothing rendered yet in --offscreen: pass {renders: N} to draw "
            "N frames and measure them, or take a screenshot (each one is a "
            "sample), then ask again");
      return {{"mode", "offscreen"},
              {"renders", state.gpuStats.samples},
              {"gpu",
               {{"supported", state.gpuStats.supported},
                {"samples", state.gpuStats.samples},
                {"shadow_ms", state.gpuStats.shadowMs},
                {"ssao_ms", state.gpuStats.ssaoMs},
                {"main_ms", state.gpuStats.mainMs},
                {"total_ms", state.gpuStats.totalMs},
                // Read THESE when measuring: the first render after a state
                // change carries first-use work, and one outlier in six
                // renders moved a 0.05 ms pass to 2.47.
                {"shadow_p50_ms", state.gpuStats.shadowP50Ms},
                {"ssao_p50_ms", state.gpuStats.ssaoP50Ms},
                {"main_p50_ms", state.gpuStats.mainP50Ms},
                {"total_p50_ms", state.gpuStats.totalP50Ms}}},
              {"note",
               "per-pass GPU time over the last renders; no frame loop, so "
               "no fps or CPU phase split. `main` is the scene alone (no "
               "interface is drawn) and carries no swapchain wait, which a "
               "windowed frame's does."}};
    }
    if (fs.frames == 0)
      throw std::runtime_error(
          m_mode == RemoteMode::Gui
              ? "no frames measured yet (the window may be minimised)"
              : "no frame loop under --headless: run the GUI (or "
                "tools/headless-preview.sh --gui) to measure frame time");
    json phases = json::object();
    json worstPhases = json::object();
    for (size_t i = 0; i < render::kPhaseCount; i++) {
      const char* name = render::PhaseName(static_cast<render::FramePhase>(i));
      phases[name] = fs.phaseMs[i];
      worstPhases[name] = fs.worstPhaseMs[i];
    }
    // The conclusion, not just the ingredients: which of wait/present holds
    // the vsync idle is a driver detail, so a caller reading one of them
    // alone would draw the wrong one.
    const double display =
        fs.phaseMs[static_cast<size_t>(render::FramePhase::Wait)] +
        fs.phaseMs[static_cast<size_t>(render::FramePhase::Present)];
    return {{"frames", fs.frames},
            {"total_frames", state.frameTimer.TotalFrames()},
            {"fps", fs.fps},
            {"avg_ms", fs.avgMs},
            {"p50_ms", fs.p50Ms},
            {"p95_ms", fs.p95Ms},
            {"max_ms", fs.maxMs},
            {"phases_ms", phases},
            {"cpu_ms", fs.avgMs - display},
            {"display_ms", display},
            // Iterations the idle throttle paced
            // (docs/rendering/frame-pacing.md#idle-throttle): a low fps with a
            // high idle share is the app resting, not the app slow.
            {"idle_frames", fs.idleFrames},
            // Who last held/restored full rate, and how long ago — "idle 0%
            // while hands-off" is undiagnosable without the waker's name.
            {"wake", state.pacerWake},
            {"wake_age_s", state.pacerWakeAgeS},
            // The worst frame's own phase split (its total is max_ms): which
            // phase held the hitch the averages dilute away.
            {"worst_phases_ms", worstPhases},
            {"worst_idle", fs.worstIdle},
            // Which PASS the GPU spends its time on — the number the CPU
            // split cannot see. supported:false names the reason (a queue
            // with no timestamp support) instead of omitting the key.
            {"gpu",
             {{"supported", state.gpuStats.supported},
              {"samples", state.gpuStats.samples},
              {"shadow_ms", state.gpuStats.shadowMs},
              {"ssao_ms", state.gpuStats.ssaoMs},
              {"main_ms", state.gpuStats.mainMs},
              {"total_ms", state.gpuStats.totalMs}}},
            // The display half's own shape. A present time that is a MULTIPLE
            // of the refresh period with `images` at 2 is the FIFO cliff, not
            // a slow GPU — and nothing else in the app can be asked about it.
            {"swapchain",
             {{"images", state.swapchainImages},
              {"present_mode", state.presentMode}}},
            {"hud", state.perfHudOpen}};
  }
  if (m == "get_settings") {
    // The whole struct, not one method per setting: the assistant and the
    // human stay on the same interface, and every setting is testable
    // headlessly.
    // rpc-returns-from: src/core/io/Settings.cpp
    json j = settings::ToJson(state.settings);
    j["path"] = settings::ConfigPath();
    j["devices"] = state.gpuDevices; // what gpu_device may be set to
    return j;
  }
  if (m == "set_settings") {
    // Validates first: a rejected value leaves nothing half-applied.
    const settings::Settings before = state.settings;
    std::string error;
    if (!settings::ApplyJson(state.settings, p.raw(), &error))
      throw std::runtime_error(error);
    // Persist NOW rather than arming the window's debounce: that debounce is
    // pumped by the GUI main loop, which a --headless instance does not run,
    // so a scripted change would apply and then never reach the file. And
    // BEFORE applying: a save that fails is refused, and a refused call
    // leaves the preferences as they were, not changed for this session only.
    if (!settings::Save(state.settings, state.settingsSaved, &error)) {
      state.settings = before;
      throw std::runtime_error("could not save settings: " + error);
    }
    ui::ApplySettings(state);
    state.settingsSaved = state.settings;
    state.settingsDirtyAt = -1.0;
    return settings::ToJson(state.settings);
  }
  if (m == "get_keymap") {
    const keymap::Keymap& keys = state.settings.keys;
    const bool filtered = p.contains("context");
    std::string want;
    if (filtered) {
      want = p.at("context").get<std::string>();
      bool known = false;
      for (keymap::Context c : keymap::AllContexts())
        known = known || want == keymap::ContextName(c);
      if (!known)
        throw std::runtime_error("unknown context: " + want);
    }
    json bindings = json::array();
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (filtered && want != keymap::ContextName(b.ctx))
        continue;
      json row{{"id", b.id},
               {"context", keymap::ContextName(b.ctx)},
               {"section", b.section},
               {"label", b.label},
               {"chord", keys.Text(b.id)},
               {"default", b.chord},
               {"custom", !keys.IsDefault(b.id)}};
      if (const keymap::Chord alt = keys.Alt(b.id))
        row["alt"] = keymap::FormatChord(alt);
      if (b.hidden)
        row["hidden"] = true;
      bindings.push_back(row);
    }
    json conflicts = json::array();
    for (const keymap::Conflict& c : keys.Conflicts())
      conflicts.push_back({{"context", keymap::ContextName(c.ctx)},
                           {"chord", keymap::FormatChord(c.chord)},
                           {"commands", json::array({c.a, c.b})}});
    json contexts = json::array();
    for (keymap::Context c : keymap::AllContexts())
      contexts.push_back(keymap::ContextName(c));
    return json{{"contexts", contexts},
                {"bindings", bindings},
                {"active", keymap::ContextName(ui::ActiveContext(state))},
                {"conflicts", conflicts}};
  }
  if (m == "set_keybinding") {
    const std::string command = p.at("command").get<std::string>();
    if (keymap::Find(command) == nullptr)
      throw std::runtime_error(
          "unknown command: " + command +
          " (get_keymap lists them; Escape/Enter/Delete are fixed verbs, not "
          "commands)");
    keymap::Keymap& keys = state.settings.keys;
    const keymap::Keymap before = keys;
    const bool reset = p.contains("reset") && p.at("reset").get<bool>();
    // Two answers to one question, refused before either is applied -- the
    // chord used to go unread, and was refused as unknown after the reset
    // had been saved.
    if (reset && p.contains("chord"))
      throw std::runtime_error(
          "set_keybinding: {reset: true} restores the default chord; a "
          "`chord` as well says something else -- pass one of them");
    if (reset) {
      keys.Reset(command);
    } else {
      // An absent `chord` is not the same as an empty one: empty UNBINDS on
      // purpose, absent is a caller who forgot which they meant.
      if (!p.contains("chord"))
        throw std::runtime_error(
            "set_keybinding needs a 'chord' (\"\" unbinds) or {reset: true}");
      std::string error;
      if (!keys.Set(command, p.at("chord").get<std::string>(), &error))
        throw std::runtime_error(error);
    }
    // Persist NOW rather than arming the window's debounce, for the same
    // reason set_settings does: a --headless instance runs no GUI main loop.
    // A save that fails takes the binding back with it.
    std::string error;
    if (!settings::Save(state.settings, state.settingsSaved, &error)) {
      keys = before;
      throw std::runtime_error("could not save settings: " + error);
    }
    state.settingsSaved = state.settings;
    return json{{"command", command},
                {"chord", keys.Text(command)},
                {"custom", !keys.IsDefault(command)}};
  }
  if (m == "set_render_options") {
    // Viewport render quality (S3): shadows on/off, SSAO off|half|full,
    // shadow-map size. Session state; the main loop applies target
    // recreation. With no params this just reads the current state.
    if (!state.meshRenderer)
      throw std::runtime_error(
          "render options need a renderer — unavailable in --headless");
    if (p.contains("shadows"))
      state.showShadows = p.at("shadows").get<bool>();
    if (p.contains("shadow_map")) {
      const int size = p.at("shadow_map").get<int>();
      if (size != 1024 && size != 2048)
        throw std::runtime_error("shadow_map must be 1024 or 2048");
      state.shadowMapSize = size;
    }
    if (p.contains("ssao")) {
      const std::string q = p.at("ssao").get<std::string>();
      if (q == "off") {
        state.showAmbientOcclusion = false;
      } else if (q == "half" || q == "full") {
        state.showAmbientOcclusion = true;
        state.ssaoFullRes = (q == "full");
      } else {
        throw std::runtime_error("ssao must be off, half or full");
      }
    }
    return {{"shadows", state.showShadows},
            {"ssao", !state.showAmbientOcclusion
                         ? "off"
                         : (state.ssaoFullRes ? "full" : "half")},
            {"shadow_map", state.shadowMapSize}};
  }
  if (m == "select_face") {
    if (!state.faceSelector || !state.meshRenderer)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
    // --offscreen projects the document into the renderers only when
    // something reads it (ui::SyncRenderersToDocument). This is one of those
    // readers: without it, every id below belongs to the geometry as it was
    // before the last edit — a face that resolves, and is the wrong face.
    ui::SyncRenderersToDocument(state);
    uint32_t mesh = 0, face = 0;
    if (p.contains("element")) {
      // Stable element name (list_elements), e.g. {"element": "2/g4"} — no
      // volatile mesh ids needed. Names are unique per document (prefixed by
      // the creating feature), so the body scan is unambiguous.
      if (!state.document)
        throw std::runtime_error("no document");
      const std::string el = p.at("element").get<std::string>();
      const auto& bodies = state.document->GetBodies();
      bool found = false;
      for (size_t i = 0; i < bodies.size() && !found; ++i) {
        for (const auto& [idx, name] : bodies[i].elementMap.names) {
          if (name != el)
            continue;
          if (i >= state.documentMeshes.size())
            throw std::runtime_error("body has no render mesh yet");
          mesh = state.documentMeshes[i].meshId;
          face = static_cast<uint32_t>(idx);
          found = true;
          break;
        }
      }
      if (!found)
        throw std::runtime_error("no face named '" + el +
                                 "' (see list_elements)");
    } else {
      mesh = p.at("mesh").get<uint32_t>();
      face = p.at("face").get<uint32_t>();
    }
    uint32_t first = 0, count = 0;
    if (!state.faceSelector->GetFaceVertexRange(mesh, face, first, count))
      throw std::runtime_error("no such face");
    // Drive the Selection (U1b) — renderer highlights are its projection.
    // The mirror into FaceSelector happens in SyncSelectionHighlights; a
    // direct write here would be cleared next frame.
    {
      const auto ref = ui::FaceElementRef(state, mesh, face);
      if (!ref)
        throw std::runtime_error("face has no stable name");
      state.selection.Replace({ui::SelectionItem::Kind::Face, 0, *ref});
    }
    return {
        {"normal", Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
        {"center", Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
  }
  if (m == "set_camera") {
    // Direct camera control (the view/fit RPCs only snap to standard views):
    // params: {target?: [x,y,z], direction?: [x,y,z] (unit offset from target
    // toward the camera), up?: [x,y,z], distance?: mm}. Applied in that
    // order; each field optional. Works headless (camera state only).
    if (!state.camera)
      throw std::runtime_error("no camera");
    Camera& cam = *state.camera;
    auto vec3 = [](const json& a) {
      return glm::vec3(a.at(0).get<float>(), a.at(1).get<float>(),
                       a.at(2).get<float>());
    };
    // An `up` within 1 degree of the line of sight fixes no roll: the view
    // comes back at whatever roll the cross product's rounding picks. Checked
    // against the direction this call ends up looking along, before anything
    // moves, so a refused call leaves the camera where it was.
    if (p.contains("up")) {
      const glm::vec3 up = vec3(p["up"]);
      if (glm::length(up) < 1e-9f)
        throw std::runtime_error("up must be non-zero");
      const glm::vec3 target =
          p.contains("target") ? vec3(p["target"]) : cam.GetTarget();
      // Toward the camera, as `direction` is: the last of eye/direction wins.
      glm::vec3 back = -cam.GetForward();
      if (p.contains("eye"))
        back = vec3(p["eye"]) - target;
      if (p.contains("direction"))
        back = vec3(p["direction"]);
      if (glm::length(back) > 1e-9f) {
        const double c = std::abs(glm::dot(glm::normalize(glm::dvec3(up)),
                                           glm::normalize(glm::dvec3(back))));
        const double deg = angles::ToDegrees(std::acos(std::min(1.0, c)));
        if (deg < 1.0) {
          char buf[32];
          std::snprintf(buf, sizeof buf, "%.3g", deg);
          throw std::runtime_error(
              std::string("up is ") + buf +
              " degrees from the view direction (eye to target); it must be "
              "at least 1 degree away to set the roll. Pass an up "
              "perpendicular to the line of sight: world Y when looking "
              "along Z, world Z otherwise");
        }
      }
    }
    if (p.contains("target"))
      cam.SetTarget(vec3(p["target"]));
    if (p.contains("eye")) {
      // V5: an explicit camera position — the intuitive form. Derives the
      // (target -> camera) direction and the distance, so it composes with
      // an earlier `target` in the same call.
      const glm::vec3 eye = vec3(p["eye"]);
      const glm::vec3 offset = eye - cam.GetTarget();
      if (glm::length(offset) < 1e-6f)
        throw std::runtime_error("eye must not equal the target");
      const glm::vec3 up = p.contains("up") ? vec3(p["up"]) : conv::kWorldUpF;
      cam.SnapToView(glm::normalize(offset), up);
      cam.SetDistance(glm::length(offset));
    }
    if (p.contains("direction")) {
      const glm::vec3 dir = vec3(p["direction"]);
      if (glm::length(dir) < 1e-9f)
        throw std::runtime_error("direction must be non-zero");
      const glm::vec3 up = p.contains("up") ? vec3(p["up"]) : conv::kWorldUpF;
      cam.SnapToView(glm::normalize(dir), up);
    }
    // `up` on its own re-rolls the current view. It used to be read only
    // alongside eye/direction and dropped silently otherwise.
    if (p.contains("up") && !p.contains("eye") && !p.contains("direction"))
      cam.SnapToView(-cam.GetForward(), vec3(p["up"]));
    if (p.contains("distance")) {
      const float d = p.at("distance").get<float>();
      if (d <= 0.0f)
        throw std::runtime_error("distance must be positive");
      cam.SetMaxDistance(std::max(20.0f * d, 1000.0f));
      cam.SetDistance(d);
    }
    return {{"position", Vec3ToJson(cam.GetPosition())},
            {"target", Vec3ToJson(cam.GetTarget())},
            {"distance", cam.GetDistance()}};
  }
  if (m == "reset_layout") {
    // Rebuild the default dock layout (panels re-docked to their edges) — the
    // recovery for a wedged imgui.ini state, without restarting the app.
    state.buildDefaultDockLayout = true;
    return json::object();
  }
  if (m == "orient_to_face") {
    if (!state.faceSelector)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
    ui::SyncRenderersToDocument(state); // resolves through documentMeshes
    const ui::SelectionItem* it =
        state.selection.First(ui::SelectionItem::Kind::Face);
    uint32_t mesh = 0, face = 0;
    if (it == nullptr || !ui::ResolveFaceItem(state, *it, mesh, face))
      throw std::runtime_error("no face selected");
    state.camera->AlignToFace(state.faceSelector->GetFaceCenter(mesh, face),
                              state.faceSelector->GetFaceNormal(mesh, face));
    return json::object();
  }

  handled = false;
  return json();
}

namespace remote {
uint8_t MethodFlagsOf(const std::string& method) { return MethodFlags(method); }
} // namespace remote

// The refusal for a request whose params the method's row does not take,
// BEFORE any handler runs (docs/scripting/rpc.md#parameters). The row is the
// vocabulary: rpc_params_gate holds it equal to the keys the handler reads,
// in both directions, so a key outside it is one no path of the handler
// reads. Learning that by running the handler -- which is how this used to
// work -- refused `cam_post {_timeout}` after it had written the G-code.
std::optional<std::string> UndeclaredParamsError(const std::string& method,
                                                 const json& params) {
  const remote::MethodDoc* d = FindMethod(method);
  if (d == nullptr)
    return std::nullopt; // "unknown method" is the dispatcher's to say
  if (params.is_null())
    return std::nullopt;
  if (!params.is_object())
    return "'" + method + "' takes its params as an object {key: value}; got " +
           std::string(JsonTypeName(params));
  const std::string ps = d->params ? d->params : "";
  if (ps == "*")
    return std::nullopt; // a free-form spec, read opaquely
  std::set<std::string> accepted;
  for (size_t i = 0, j = 0; i <= ps.size(); i++) {
    if (i == ps.size() || ps[i] == ',') {
      const std::string k = ps.substr(j, i - j);
      const size_t b = k.find_first_not_of(' ');
      const size_t e = k.find_last_not_of(' ');
      if (b != std::string::npos)
        accepted.insert(k.substr(b, e - b + 1));
      j = i + 1;
    }
  }
  std::vector<std::string> unknown;
  for (auto it = params.begin(); it != params.end(); ++it)
    if (accepted.count(it.key()) == 0)
      unknown.push_back(it.key());
  if (unknown.empty())
    return std::nullopt;
  return UnknownParamError(method, unknown, accepted);
}

// The active tab's save stamp, whichever kind of document it holds (the
// value ProjectDirty compares against state.lastSavedVersion).
static uint64_t ActiveSaveStamp(const ui::AppState& state) {
  if (state.document)
    return state.document->SaveStamp();
  if (state.assembly)
    return state.assembly->SaveStamp();
  if (state.drawing)
    return state.drawing->Version();
  if (state.job)
    return state.job->SaveStamp();
  return 0;
}

EditMarks MarkEdits(const ui::AppState& state) {
  EditMarks marks;
  marks.activePart = state.activePart;
  marks.activeStamp = ActiveSaveStamp(state);
  marks.lastSaved = state.lastSavedVersion;
  marks.document = state.document.get();
  marks.commands = state.commands.get();
  marks.commandDepth = state.commands ? state.commands->UndoDepth() : 0;
  marks.assembly = state.assembly.get();
  marks.assemblyUndo = state.assemblyHistory.undo.size();
  marks.drawing = state.drawing.get();
  marks.drawingUndo = state.drawingHistory.undo.size();
  marks.job = state.job.get();
  marks.jobUndo = state.jobHistory.undo.size();
  return marks;
}

std::string RollBackEdits(ui::AppState& state, const EditMarks& marks) {
  std::vector<std::string> what;
  // Each history only where the tab is still the one marked: a call that
  // switched tabs moved the histories with them, and the marks say nothing
  // about the tab that is active now.
  if (state.document && state.commands &&
      state.document.get() == marks.document &&
      state.commands.get() == marks.commands) {
    const size_t undone =
        state.commands->RollbackSince(marks.commandDepth, *state.document);
    if (undone > 0)
      what.push_back(std::to_string(undone) + " edit(s) undone");
  }
  // The snapshot histories: every edit pushed its pre-edit copy, so undoing
  // back to the mark and dropping the redo entries that made restores
  // exactly the state before the call.
  if (state.assembly && state.assembly.get() == marks.assembly &&
      state.assemblyHistory.undo.size() > marks.assemblyUndo) {
    size_t n = 0;
    while (state.assemblyHistory.undo.size() > marks.assemblyUndo &&
           ui::UndoAssembly(state))
      n++;
    auto& h = state.assemblyHistory;
    h.redo.resize(h.redo.size() - std::min(n, h.redo.size()));
    h.redoStamps.resize(h.redoStamps.size() - std::min(n, h.redoStamps.size()));
    ui::SolveActiveAssembly(state);
    what.push_back("the assembly restored");
  }
  if (state.drawing && state.drawing.get() == marks.drawing &&
      state.drawingHistory.undo.size() > marks.drawingUndo) {
    size_t n = 0;
    while (state.drawingHistory.undo.size() > marks.drawingUndo &&
           ui::UndoDrawing(state))
      n++;
    auto& h = state.drawingHistory;
    h.redo.resize(h.redo.size() - std::min(n, h.redo.size()));
    h.redoStamps.resize(h.redoStamps.size() - std::min(n, h.redoStamps.size()));
    what.push_back("the drawing restored");
  }
  if (state.job && state.job.get() == marks.job &&
      state.jobHistory.undo.size() > marks.jobUndo) {
    size_t n = 0;
    while (state.jobHistory.undo.size() > marks.jobUndo && ui::UndoJob(state))
      n++;
    auto& h = state.jobHistory;
    h.redo.resize(h.redo.size() - std::min(n, h.redo.size()));
    h.redoStamps.resize(h.redoStamps.size() - std::min(n, h.redoStamps.size()));
    what.push_back("the job restored");
  }
  // Saved before the call and restored now: saved still. The restore moved
  // the stamp on, and without this a refused call would leave the project
  // reading as edited.
  if (!what.empty() && state.activePart == marks.activePart &&
      marks.activeStamp == marks.lastSaved &&
      state.lastSavedVersion == marks.lastSaved)
    state.lastSavedVersion = ActiveSaveStamp(state);
  std::string out;
  for (size_t i = 0; i < what.size(); i++)
    out += (i == 0 ? "" : i + 1 == what.size() ? " and " : ", ") + what[i];
  return out;
}
