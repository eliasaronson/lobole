// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/InspectionContracts.h"
#include "remote/RemoteServer.h"

#include "remote/InstanceRegistry.h"

#include <Standard_Failure.hxx>

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "cad/MeshExport.h"
#include "common/Logger.h"
#include "common/Watchdog.h"
#include "core/Conventions.h"
#include "core/Expression.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingExport.h"
#include "core/features/DerivedFeature.h"
#include "core/features/SketchFeature.h"
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
#include "remote/RemoteSimulation.h"
#include "remote/RpcErrors.h"
#include "remote/RpcParams.h"

#include "render/Camera.h"
#include "render/FrameTimer.h"
#include "render/MeshRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/RenderSheet.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/Commands.h" // ActiveContext (get_keymap)
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
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
#include <filesystem>
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

// Driving the GUI from an RPC: item_click, viewport_drag, key_press and the
// rest. They cannot run where the other RPCs run -- ImGui state is only
// coherent inside the frame -- so they are queued here and drained by the
// frame loop through ProcessPending.
//
// Only in a build with Dear ImGui Test Engine; `ping` reports whether this
// build has it.
// Split out of RemoteServer.cpp (docs/architecture.md#the-remote-json-rpc).
// RemoteServer.h is unchanged: these are the same class's members,
// defined across three translation units instead of one.

// The test-engine hook lives with the actions it runs: SetTestEngine
// registers ONE reusable test, and ProcessPending queues it per action.
// It was in the transport half only because everything was one file.

#ifdef HAVE_TEST_ENGINE
static ImGuiTest* s_UiActionTest = nullptr;
#endif
// TestFunc of the reusable "remote action" test: runs whatever UI action is
// currently queued. Runs inside the test engine's coroutine, so it can span
// frames (menu navigation, drags) exactly like a scripted human.
#ifdef HAVE_TEST_ENGINE
void RemoteServerRunUiAction(ImGuiTestContext* ctx) {
  if (g_RemoteServerInstance && g_RemoteServerInstance->m_uiAction) {
    g_RemoteServerInstance->m_uiAction(ctx);
  }
}
#endif
void RemoteServer::SetTestEngine(ImGuiTestEngine* engine) {
#ifdef HAVE_TEST_ENGINE
  m_engine = engine;
  if (engine == nullptr)
    return;
  // Each RPC is a separate test-engine run. Restoring the pre-run focus
  // would undo focus_panel (and dock-tab clicks) before the next RPC could
  // inspect or use that panel. Remote actions must retain their UI effects.
  ImGuiTestEngine_GetIO(engine).ConfigRestoreFocusAfterTests = false;
  // One reusable test carries every queued UI action (ProcessPending queues
  // it; the engine runs it between frames, where clicks are legal).
  s_UiActionTest = IM_REGISTER_TEST(engine, "remote", "action");
  s_UiActionTest->TestFunc = [](ImGuiTestContext* ctx) {
    RemoteServerRunUiAction(ctx);
  };
#else
  // Built with -DENABLE_TEST_ENGINE=OFF: the UI-parity tier does not exist,
  // and every ui_action refuses with that reason rather than a silent no-op.
  (void)engine;
#endif
}

ImGuiKeyChord ParseChord(const std::string& text, std::string* error) {
  ImGuiKeyChord chord = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('+', start);
    std::string token = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (token == "Ctrl")
      chord |= ImGuiMod_Ctrl;
    else if (token == "Shift")
      chord |= ImGuiMod_Shift;
    else if (token == "Alt")
      chord |= ImGuiMod_Alt;
    else if (token.size() == 1 && token[0] >= 'A' && token[0] <= 'Z')
      chord |= ImGuiKey_A + (token[0] - 'A');
    else if (token.size() == 1 && token[0] >= '0' && token[0] <= '9')
      chord |= ImGuiKey_0 + (token[0] - '0');
    else if (token == "Enter")
      chord |= ImGuiKey_Enter;
    else if (token == "Escape")
      chord |= ImGuiKey_Escape;
    else if (token == "Delete")
      chord |= ImGuiKey_Delete;
    else if (token == "Tab")
      chord |= ImGuiKey_Tab;
    else if (token == "Space")
      chord |= ImGuiKey_Space;
    else if (token == "Left")
      chord |= ImGuiKey_LeftArrow;
    else if (token == "Right")
      chord |= ImGuiKey_RightArrow;
    else if (token == "Up")
      chord |= ImGuiKey_UpArrow;
    else if (token == "Down")
      chord |= ImGuiKey_DownArrow;
    else if (token == "PageUp")
      chord |= ImGuiKey_PageUp;
    else if (token == "PageDown")
      chord |= ImGuiKey_PageDown;
    else if (token == "Home")
      chord |= ImGuiKey_Home;
    else if (token == "End")
      chord |= ImGuiKey_End;
    else {
      if (error)
        *error = "unknown key token: " + token;
      return 0;
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return chord;
}

// The 3D viewport's screen rect, snapshotted every tick in ProcessPending.
// Zero size = no renderer (--headless) or not drawn yet; callers fall back to
// the whole window.
ImVec2 s_ViewportPos(0.0f, 0.0f);
ImVec2 s_ViewportSize(0.0f, 0.0f);

// (x, y) in 0..1 of the 3D VIEWPORT, which is what `describe` and every
// caller means by "viewport_click" — mapping them over the whole OS window
// aimed every click at the wrong pixel whenever a panel was docked (0.5,0.5
// landed in the feature tree, not the middle of the model).
ImVec2 FractionToPos(const json& params, const char* xKey = "x",
                     const char* yKey = "y", float xDef = 0.5f,
                     float yDef = 0.5f) {
  ImVec2 pos = s_ViewportPos;
  ImVec2 size = s_ViewportSize;
  if (size.x <= 1.0f || size.y <= 1.0f) {
    pos = ImVec2(0.0f, 0.0f);
    size = ImGui::GetMainViewport()->Size;
  }
  return ImVec2(pos.x + params.value(xKey, xDef) * size.x,
                pos.y + params.value(yKey, yDef) * size.y);
}

// Everything from here to BuildSheetFooter speaks the test engine's types
// (ImGuiTestRef, ImGuiTestItemInfo, ImGuiTestContext) or reaches into
// imgui_internal.h, which only the engine's headers pull in — so none of it
// compiles in the -DENABLE_TEST_ENGINE=OFF build, and all of it is used only
// from QueueUiAction's engine branch below. It was outside the guard, which
// broke that build; the job that compiles it never got that far, because the
// step before it was failing.
#ifdef HAVE_TEST_ENGINE

// A widget addressed either by test-engine path ({ref}) or by the numeric id
// {id} that `ui_items` reports. Ids are the only handle on widgets whose
// label is hidden ("##Type" — every LabeledCombo), which no path can reach.
struct ItemTarget {
  std::string path;
  ImGuiID id = 0;

  // A panel's DISPLAY title is not its address. Titles carry an icon and a
  // "###id" ("<glyph> Model###Model") so they can be reworded without
  // breaking anything, and ImGui hashes a window from the "###" onward — so
  // the addressable form is "//###Model/...". Callers should not have to
  // know that, and every script written before the icons landed says
  // "//Model/...", so accept the friendly form and rewrite it.
  static std::string ResolveWindowAlias(const std::string& ref) {
    if (!ref.starts_with("//") || ref.starts_with("//$") ||
        ref.starts_with("//###") || ref.starts_with("//**"))
      return ref;
    const size_t slash = ref.find('/', 2);
    const std::string window = ref.substr(2, slash - 2);
    if (window.empty() || window.find('#') != std::string::npos)
      return ref;
    // Only when the plain name names no window but the "###" form does —
    // never override a window that really is called that.
    if (ImGui::FindWindowByName(window.c_str()) != nullptr)
      return ref;
    const std::string hashed = "###" + window;
    if (ImGui::FindWindowByID(ImHashStr(hashed.c_str())) == nullptr)
      return ref;
    return "//" + hashed +
           (slash == std::string::npos ? "" : ref.substr(slash));
  }

  ItemTarget(const json& params, const std::string& method) {
    path = ResolveWindowAlias(params.value("ref", std::string()));
    id = params.value("id", ImGuiID(0));
    if (path.empty() && id == 0) {
      throw std::runtime_error(method + " needs {ref} or {id} (ids come from "
                                        "ui_items)");
    }
  }
  // Valid while this object lives — the actions hold it by value.
  ImGuiTestRef Ref() const {
    return path.empty() ? ImGuiTestRef(id) : ImGuiTestRef(path.c_str());
  }

  // A disabled widget swallows clicks silently, so a script "pressing
  // Accept" on a tool that is not ready got a success and no feature. Say so
  // instead — LogError fails the action, which becomes the RPC error.
  bool RefuseIfDisabled(ImGuiTestContext* ctx) const {
    const ImGuiTestItemInfo info = ctx->ItemInfo(Ref());
    if (info.ID != 0 && (info.ItemFlags & ImGuiItemFlags_Disabled) != 0) {
      // IM_ERRORF (not LogError) — only an engine error marks the action
      // failed, which is what turns into the client's error response.
      IM_ERRORF_NOHDR("item is disabled: %s",
                      path.empty() ? info.DebugLabel : path.c_str());
      return true;
    }
    return false;
  }
};

// One widget list as JSON: what a scripted human can see of a window.
json ItemsToJson(const ImGuiTestItemList& items) {
  json out = json::array();
  for (const ImGuiTestItemInfo& it : items) {
    // The test engine keeps a 32-byte DebugLabel, so a longer one arrives
    // CUT — including its "###id" suffix, if the suffix is what falls off.
    // A caller who pastes that back into item_click gets "unable to locate
    // item"; the numeric id below always works. Say which labels are cut
    // rather than let them look whole.
    const bool cut =
        std::strlen(it.DebugLabel) >= IM_ARRAYSIZE(it.DebugLabel) - 1;
    json e{{"label", it.DebugLabel},
           {"label_truncated", cut},
           {"id", it.ID},
           {"depth", it.Depth},
           {"disabled", (it.ItemFlags & ImGuiItemFlags_Disabled) != 0},
           // Openable = a submenu header: clicking it only opens more items,
           // so callers must address the leaf as "Parent/Leaf".
           {"submenu", (it.StatusFlags & ImGuiItemStatusFlags_Openable) != 0},
           {"x", it.RectFull.GetCenter().x},
           {"y", it.RectFull.GetCenter().y}};
    if (it.Window != nullptr)
      e["window"] = it.Window->Name;
    out.push_back(std::move(e));
  }
  return out;
}

// Clicks `target` and returns the popup it opened (a combo list, the tab
// bar's "+" menu, a right-click context menu). Fails the action when nothing
// opened, so callers can just check for null.
ImGuiWindow* OpenPopupOf(ImGuiTestContext* ctx, const ItemTarget& target,
                         int button) {
  ctx->ItemClick(target.Ref(), button);
  ImGuiWindow* popup = ctx->GetWindowByRef("//$FOCUSED");
  if (popup == nullptr)
    IM_ERRORF_NOHDR("%s", "that widget opened no popup");
  return popup;
}

// Why the last UI action failed, from the test engine's own log ("Unable to
// locate item: '###tb_box'"). Without this the client got "see app log" and
// nothing was ever written there.
std::string UiActionFailureReason(ImGuiTest* test) {
  if (test == nullptr)
    return {};
  ImGuiTextBuffer buf;
  test->Output.Log.ExtractLinesForVerboseLevels(
      ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Warning, &buf);
  // The raw log is the engine's own transcript ("[0007] Error 'Unable to
  // locate item: ...'"). Keep the reasons, drop the frame stamps and the
  // bookkeeping lines, and join them into one JSON-RPC error string.
  std::string out;
  std::string line;
  std::istringstream in(buf.c_str());
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t close = line.find("] ");
    if (line.rfind("[", 0) == 0 && close != std::string::npos)
      line = line.substr(close + 2); // strip "[0007] "
    if (line.rfind("Test: ", 0) == 0 ||
        line.find("test failed.") != std::string::npos || line.empty())
      continue;
    if (line.rfind("Error ", 0) == 0)
      line = line.substr(6); // the level is already implied
    if (!out.empty())
      out += "; ";
    out += line;
  }
  return out;
}

#endif // HAVE_TEST_ENGINE

// V3 render_sheet footer: the kernel digest burned under the view grid, so
// the eyeball pass and the numbers live in the same image. Reuses the
// mass_properties and contact_report handlers rather than re-deriving.
std::vector<std::string> BuildSheetFooter(ui::AppState& state, Document* doc) {
  std::vector<std::string> out;
  char buf[192];
  const nlohmann::json empty = nlohmann::json::object();
  try {
    double volume = 0.0;
    bool massKnown = false;
    double massG = 0.0;
    {
      remote::RpcParams p{empty};
      if (auto r =
              remote::HandleInspectionRpc(state, doc, "mass_properties", p)) {
        const nlohmann::json& total = r->at("total");
        volume = total.value("volume_mm3", 0.0);
        if (total.contains("mass_g") && !total.at("mass_g").is_null()) {
          massKnown = true;
          massG = total.at("mass_g").get<double>();
        }
      }
    }
    glm::dvec3 mn(0.0), mx(0.0);
    bool haveBox = false;
    int count = 0;
    if (doc != nullptr) {
      for (const Body& b : doc->GetBodies()) {
        glm::dvec3 bmn(0.0), bmx(0.0);
        b.shape.GetBoundingBox(bmn, bmx);
        mn = haveBox ? glm::min(mn, bmn) : bmn;
        mx = haveBox ? glm::max(mx, bmx) : bmx;
        haveBox = true;
        count++;
      }
      snprintf(buf, sizeof buf, "%d BODIES", count);
    } else if (state.assembly) {
      for (const Occurrence& o : state.assembly->GetOccurrences()) {
        if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
          continue;
        const Document* pd = ui::OccurrencePartDoc(state, o);
        glm::dvec3 bmn(0.0), bmx(0.0);
        if (pd == nullptr ||
            !Measure::WorldBounds(pd->GetBodies(), ui::OccurrenceMatrix(o), bmn,
                                  bmx))
          continue;
        mn = haveBox ? glm::min(mn, bmn) : bmn;
        mx = haveBox ? glm::max(mx, bmx) : bmx;
        haveBox = true;
        count++;
      }
      snprintf(buf, sizeof buf, "%d OCCURRENCES", count);
    } else {
      return out;
    }
    std::string head = buf;
    if (massKnown)
      snprintf(buf, sizeof buf, "%s   MASS %.2f KG", head.c_str(),
               massG / 1000.0);
    else
      snprintf(buf, sizeof buf, "%s   MASS UNKNOWN", head.c_str());
    out.push_back(buf);
    if (haveBox) {
      snprintf(buf, sizeof buf, "BBOX %.0f X %.0f X %.0f MM   VOL %.0f CM3",
               mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, volume / 1000.0);
      out.push_back(buf);
    }
    if (doc == nullptr && state.assembly) {
      remote::RpcParams p{empty};
      if (auto r =
              remote::HandleInspectionRpc(state, doc, "contact_report", p)) {
        snprintf(buf, sizeof buf,
                 "CONTACT: %zu COMPONENTS, %zu CLASHES, %zu NEAR-MISSES",
                 r->at("components").size(), r->at("clashes").size(),
                 r->at("near_misses").size());
        out.push_back(buf);
      }
    }
  } catch (const std::exception& e) {
    out.push_back(std::string("DIGEST UNAVAILABLE: ") + e.what());
  }
  return out;
}

// Whether this method goes on the ImGui queue instead of running inline. It
// was a hand-written list of sixteen names; it is the UiAction column now, so
// a new ui_action cannot be added without the dispatcher learning about it.
bool RemoteServer::IsUiAction(const std::string& method) {
  return (remote::MethodFlagsOf(method) & remote::UiAction) != 0;
}

bool RemoteServer::QueueUiAction(const Request& request) {
#ifndef HAVE_TEST_ENGINE
  // No test engine in this build: nothing here can run, and the
  // caller has already refused every ui_action with that reason.
  (void)request;
  return false;
#else
  const std::string& m = request.method;
  const json p = request.params; // copy: captured by the action lambda

  // Required params are extracted HERE, on the RPC thread, where a missing
  // key throws into ProcessPending's catch and becomes an error response. A
  // p.at() inside an action lambda would throw in the test-engine coroutine
  // instead, escape it, and std::terminate the whole app.
  //
  // `need` because these come off a RAW json (the lambdas capture it by
  // value), which skips RpcParams' caller-grade message: a missing `chord`
  // used to answer "[json.exception.out_of_range.403] key 'chord' not
  // found". `alias` names the key someone plausibly reached for instead,
  // since "missing 'chord'" is not much help to whoever just typed `key`.
  const auto need = [&m](const json& obj, const char* key,
                         const char* alias = nullptr) -> const json& {
    if (obj.contains(key))
      return obj.at(key);
    std::string msg =
        "missing required parameter '" + std::string(key) + "' for " + m;
    if (alias != nullptr && obj.contains(alias))
      msg += " — you passed '" + std::string(alias) + "'";
    throw std::runtime_error(msg);
  };
  if (m == "menu_click") {
    const std::string path = need(p, "path", "menu").get<std::string>();
    m_uiAction = [path](ImGuiTestContext* ctx) {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick(path.c_str());
    };
  } else if (m == "item_click") {
    const ItemTarget target(p, m);           // rpc-params: ref, id
    const int button = p.value("button", 0); // 1 = right-click (context menus)
    m_uiAction = [target, button](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemClick(target.Ref(), button);
    };
  } else if (m == "item_double_click") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    m_uiAction = [target](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemDoubleClick(target.Ref());
    };
  } else if (m == "item_input") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    const json value = need(p, "value", "text");
    if (!value.is_number() && !value.is_string())
      throw std::runtime_error("item_input value must be a number or string");
    m_uiAction = [target, value](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      if (value.is_number()) {
        // Typed as the JSON spells it, which is the shortest text that reads
        // back as the same double: the engine's float overload types "%f"
        // of a float, so 24.7 arrived as 24.700001.
        ctx->ItemInputValue(target.Ref(), value.dump().c_str());
      } else {
        ctx->ItemInputValue(target.Ref(), value.get<std::string>().c_str());
      }
    };
  } else if (m == "item_hover") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    m_uiAction = [target](ImGuiTestContext* ctx) {
      ctx->MouseMove(target.Ref());
    };
  } else if (m == "item_combo" || m == "popup_click") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    // item_combo names the entry `value`; popup_click (menus, context menus)
    // names it `item`. Same motion either way.
    const std::string entry = m == "item_combo"
                                  ? need(p, "value").get<std::string>()
                                  : need(p, "item", "value").get<std::string>();
    const int button = p.value("button", 0); // 1 = a right-click menu
    // Open + choose in ONE action: a queued action ends with its own frames
    // and the popup closes in between, so two RPCs can never do this.
    m_uiAction = [target, entry, button](ImGuiTestContext* ctx) {
      ImGuiWindow* popup = OpenPopupOf(ctx, target, button);
      if (popup == nullptr)
        return;
      const std::string root = std::string("//") + popup->Name + "/";
      if (entry.find('/') != std::string::npos) {
        // A path walks submenus ("Part 1/Front"): MenuClick knows how to
        // hover a submenu open without cutting the corner and closing it.
        ctx->MenuClick((root + entry).c_str());
      } else {
        ctx->ItemClick((root + "**/" + entry).c_str());
      }
    };
  } else if (m == "menu_items") {
    // The menu bar is the one surface ui_items/popup_items cannot describe:
    // its entries only exist while the menu is open, and opening one needs a
    // path you were supposed to know already. {path} omitted lists the top
    // level; "File" lists that menu; "File/Recent" walks into a submenu.
    const std::string path = p.value("path", std::string());
    m_uiAction = [this, path](ImGuiTestContext* ctx) {
      if (path.empty()) {
        // GatherItems cannot see menu-bar buttons (they live in an internal
        // child it does not walk), so the bar reports its own names —
        // recorded by the code that draws them, in AppState::menuBarTop.
        json out = json::array();
        if (m_state == nullptr) {
          IM_ERRORF_NOHDR("%s", "no app state yet");
          return;
        }
        for (const std::string& name : m_state->menuBarTop)
          out.push_back(json{{"label", name}, {"submenu", true}});
        m_uiActionResult = json{{"menu", ""}, {"items", std::move(out)}};
        return;
      }
      ctx->MenuClick(("//##MainMenuBar/" + path).c_str()); // opens it
      ImGuiWindow* popup = ctx->GetWindowByRef("//$FOCUSED");
      if (popup == nullptr) {
        IM_ERRORF_NOHDR("%s", "that menu path opened nothing");
        return;
      }
      ImGuiTestItemList items;
      ctx->GatherItems(&items, (std::string("//") + popup->Name).c_str());
      m_uiActionResult = json{{"menu", path}, {"items", ItemsToJson(items)}};
      ctx->KeyPress(ImGuiKey_Escape); // close it again
    };
  } else if (m == "popup_items") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    const int button = p.value("button", 0);
    m_uiAction = [this, target, button](ImGuiTestContext* ctx) {
      ImGuiWindow* popup = OpenPopupOf(ctx, target, button);
      if (popup == nullptr)
        return;
      ImGuiTestItemList items;
      ctx->GatherItems(&items, (std::string("//") + popup->Name).c_str());
      m_uiActionResult =
          json{{"popup", popup->Name}, {"items", ItemsToJson(items)}};
      // Close it directly. Neither input gesture works: Escape falls through
      // to the app and cancels the tool session the popup belongs to (listing
      // a Loft's sections closed the Loft), and clicking the opener again
      // cannot reach it — a combo popup is drawn OVER its own opener, so the
      // click lands on the popup.
      if (ctx->UiContext->OpenPopupStack.Size > 0)
        ImGui::ClosePopupToLevel(0, true);
    };
  } else if (m == "scroll_panel") {
    // For screenshots, not for reach. Round 2 of the CAM dogfooding reported
    // that a panel's lower half was unreachable; it was not — nothing here
    // uses ImGuiListClipper, so every item is submitted whether or not it is
    // on screen, and the test engine scrolls to an item before clicking it.
    // What a script genuinely cannot do without this is LOOK at the bottom of
    // a panel, because a capture shows the pixels that exist.
    const std::string window = p.at("window").get<std::string>();
    const json to = p.value("to", json("bottom"));
    m_uiAction = [this, window, to](ImGuiTestContext* ctx) {
      const std::string ref =
          window.rfind("//", 0) == 0 ? window : "//" + window;
      if (to.is_number()) {
        ctx->ScrollToY(ref.c_str(), to.get<float>());
      } else {
        const std::string where = to.get<std::string>();
        if (where == "top")
          ctx->ScrollToTop(ref.c_str());
        else
          ctx->ScrollToBottom(ref.c_str());
      }
      ctx->Yield(2);
      ImGuiTestItemList items;
      ctx->GatherItems(&items, ref.c_str());
      m_uiActionResult =
          json{{"window", window}, {"items", ItemsToJson(items)}};
    };
  } else if (m == "focus_panel") {
    // Focusing a docked window takes effect on the NEXT frame, and a tab
    // sharing its node with several others needs the request repeated before
    // it wins (the same thing the History panel needed). So this sets a
    // request the dock host re-applies, and waits for the tab to arrive.
    const std::string name = p.at("name").get<std::string>();
    m_uiAction = [this, name](ImGuiTestContext* ctx) {
      if (m_state != nullptr) {
        m_state->focusPanel = name;
        m_state->focusPanelFrames = 60;
      }
      ctx->Yield(8); // let the request take, so the next RPC can address it
      m_uiActionResult = json{{"name", name}};
    };
  } else if (m == "ui_items") {
    const std::string ref = p.value("ref", "//$FOCUSED");
    const int depth = p.value("depth", -1);
    m_uiAction = [this, ref, depth](ImGuiTestContext* ctx) {
      ImGuiTestItemList items;
      ctx->GatherItems(&items, ref.c_str(), depth);
      m_uiActionResult = json{{"items", ItemsToJson(items)}};
    };
  } else if (m == "viewport_click") {
    // rpc-params: x, y
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const bool ctrl = p.value("ctrl", false);
      ctx->MouseMoveToPos(FractionToPos(p));
      if (ctrl)
        ctx->KeyDown(ImGuiMod_Ctrl);
      ctx->MouseClick(p.value("button", 0));
      if (ctrl)
        ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "viewport_drag") {
    // FractionToPos reads these, and the scanner cannot follow it -- which is
    // how both of these methods came to advertise their MODIFIERS and not the
    // place to click. Every drag a script could ask for started at the centre
    // of the viewport and ended a tenth of the way out.
    // rpc-params: x0, y0, x1, y1
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const ImVec2 from = FractionToPos(p, "x0", "y0", 0.5f, 0.5f);
      const ImVec2 to = FractionToPos(p, "x1", "y1", 0.6f, 0.6f);
      const int button = p.value("button", ImGuiMouseButton_Middle);
      const bool ctrl = p.value("ctrl", false);
      const bool shift = p.value("shift", false);
      ctx->MouseMoveToPos(from);
      if (ctrl)
        ctx->KeyDown(ImGuiMod_Ctrl);
      if (shift)
        ctx->KeyDown(ImGuiMod_Shift);
      ctx->MouseDown(button);
      ctx->MouseMoveToPos(to);
      ctx->MouseUp(button);
      if (shift)
        ctx->KeyUp(ImGuiMod_Shift);
      if (ctrl)
        ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "key_press") {
    std::string error;
    ImGuiKeyChord chord =
        ParseChord(need(p, "chord", "key").get<std::string>(), &error);
    if (chord == 0) {
      // Returning false here made the dispatcher answer "unknown method:
      // key_press" — the chord is what was unknown, and it said so.
      throw std::runtime_error(error.empty() ? "could not parse the key chord"
                                             : error);
    }
    m_uiAction = [chord](ImGuiTestContext* ctx) { ctx->KeyPress(chord); };
  } else if (m == "scroll") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      if (p.contains("window")) {
        // V5: scroll a PANEL by {window, dy} pixels (dy > 0 = down) — the
        // remote surface had no way to reach a panel's lower sections.
        ImGuiWindow* w =
            ImGui::FindWindowByName(p.at("window").get<std::string>().c_str());
        IM_CHECK_SILENT(w != nullptr);
        ImGui::SetScrollY(
            w, ImClamp(w->Scroll.y + static_cast<float>(p.value("dy", 0.0)),
                       0.0f, w->ScrollMax.y));
        return;
      }
      ctx->MouseMoveToPos(FractionToPos(p));
      ctx->MouseWheelY(p.value("amount", 1.0f));
    };
  } else {
    return false;
  }

  m_uiActionId = request.id;
  m_uiActionClient = request.client;
  m_uiActionResult = json::object();
  m_uiActionInFlight = true;
#ifdef HAVE_TEST_ENGINE
  ImGuiTestEngine_QueueTest(m_engine, s_UiActionTest);
#endif
  return true;
#endif
}

// --- Parked requests (docs/architecture.md#requests-that-wait) -------------

struct RemoteServer::Parked {
  Request request;
  // Over `request.params`, kept for the whole wait: the keys the waiting
  // steps read are what the unknown-parameter check compares against when
  // the answer comes from them rather than from a handler.
  std::unique_ptr<remote::RpcParams> params;
  std::unique_ptr<remote::Parking> wait;
};

bool RemoteServer::ConnectionOpen(uint64_t connection) {
  std::lock_guard<std::mutex> lock(m_clientsMutex);
  return m_openConnections.contains(connection);
}

bool RemoteServer::ClientConnected(int client) {
  std::lock_guard<std::mutex> lock(m_clientsMutex);
  return std::find(m_clients.begin(), m_clients.end(), client) !=
         m_clients.end();
}

void RemoteServer::PollParked(ui::AppState& state) {
  std::vector<Request> resume;
  for (auto it = m_parked.begin(); it != m_parked.end();) {
    Parked& parked = **it;
    // Nobody to answer: the passes keep cutting (they are cached, and paid
    // for), the answer is simply not sent.
    if (!ClientConnected(parked.request.client)) {
      it = m_parked.erase(it);
      continue;
    }
    json answer;
    remote::CamParking::Next next = remote::CamParking::Next::Wait;
    try {
      const watchdog::Label label("RPC " + parked.request.method + " (parked)");
      next = parked.wait->Poll(state, *parked.params, answer);
    } catch (...) {
      // The ladder's rule (below): a handler that fell over says `internal`.
      const remote::CaughtError caught =
          remote::DescribeCurrentException(parked.request.method);
      json message{{"id", parked.request.id}, {"error", caught.message}};
      if (!caught.diagnostic.is_null())
        message["diagnostic"] = caught.diagnostic;
      Send(parked.request.client, message);
      it = m_parked.erase(it);
      continue;
    }
    if (next == remote::CamParking::Next::Wait) {
      ++it;
      continue;
    }
    if (next == remote::CamParking::Next::Dispatch) {
      parked.request.resumed = true;
      resume.push_back(std::move(parked.request));
    } else if (auto unknown = parked.params->UnknownKeys(); !unknown.empty()) {
      RespondError(parked.request.client, parked.request.id,
                   UnknownParamError(parked.request.method, unknown,
                                     parked.params->SeenKeys()));
    } else {
      if (m_recording.is_open()) {
        m_recording << json{{"method", parked.request.method},
                            {"params", parked.request.params}}
                           .dump()
                    << "\n";
        m_recording.flush();
      }
      Respond(parked.request.client, parked.request.id, answer);
    }
    it = m_parked.erase(it);
  }
  if (resume.empty())
    return;
  // At the HEAD of the queue, in the order they were parked: each is its
  // client's oldest request, and its client's later ones wait behind it.
  std::lock_guard<std::mutex> lock(m_queueMutex);
  for (auto r = resume.rbegin(); r != resume.rend(); ++r)
    m_requests.push_front(std::move(*r));
}

namespace {

// The RPC lines of the activity journal: one per call, by method name only —
// the params can be large, and they name the user's files
// (docs/architecture.md#crashes-and-stalls). A run of the same call that
// changed nothing is one line, not one per call: a client polling get_state
// would otherwise bury the edits it is there to show.
std::string g_lastRpcLogged;
bool g_lastRpcEdited = false;

bool SameEdits(const EditMarks& a, const EditMarks& b) {
  return a.document == b.document && a.commandDepth == b.commandDepth &&
         a.assembly == b.assembly && a.assemblyUndo == b.assemblyUndo &&
         a.drawing == b.drawing && a.drawingUndo == b.drawingUndo &&
         a.job == b.job && a.jobUndo == b.jobUndo &&
         a.activePart == b.activePart && a.activeStamp == b.activeStamp;
}

void LogRpc(const std::string& method, const nlohmann::json& params) {
  if (method == g_lastRpcLogged && !g_lastRpcEdited)
    return;
  g_lastRpcLogged = method;
  if (method != "batch" || !params.is_object() || !params.contains("calls") ||
      !params["calls"].is_array()) {
    LOG_INFO("rpc: {}", method);
    return;
  }
  // A batch names its calls: it is the one method whose name says nothing.
  std::string calls;
  size_t shown = 0;
  for (const auto& call : params["calls"]) {
    if (shown == 12) {
      calls += ", …";
      break;
    }
    calls += (shown++ == 0 ? "" : ", ") +
             (call.is_object() && call.contains("method") &&
                      call["method"].is_string()
                  ? call["method"].get<std::string>()
                  : std::string("?"));
  }
  LOG_INFO("rpc: batch [{}]", calls);
}

// Whether the call it spans edited anything, for the next LogRpc.
struct RpcEditWatch {
  const ui::AppState& state;
  EditMarks before;
  explicit RpcEditWatch(const ui::AppState& s)
      : state(s), before(MarkEdits(s)) {}
  ~RpcEditWatch() { g_lastRpcEdited = !SameEdits(before, MarkEdits(state)); }
};

} // namespace

size_t RemoteServer::ProcessPending(ui::AppState& state) {
  if (!m_running)
    return 0;
  // Remote activity this tick, for the idle pacer: requests executed, plus
  // one while an async job still spans frames (a ui action driven by the
  // test engine, a staged screenshot, a sheet render) — those need the loop
  // at full rate to make progress.
  size_t activity = 0;

  m_state = &state; // UI actions read it from the engine's coroutine

  // Fractional viewport coordinates are resolved against the 3D viewport's
  // rect, so snapshot it while we are on the main thread with `state` in hand.
#ifdef HAVE_TEST_ENGINE
  s_ViewportPos = ImVec2(state.viewportPos.x, state.viewportPos.y);
  s_ViewportSize = ImVec2(state.viewportSize.x, state.viewportSize.y);
#endif

  // Store a finished background job (a solve, a mesh, an adaptive pass…)
  // even if the client never polls for it — this is the RPC pump's own
  // main-thread join point, and it applies before it answers.
  ui::ReapBackgroundJobs(state);
  // A study job dies with the connection that started it, unless it was
  // asked to outlive it (docs/simulation/README.md#studies-as-jobs).
  {
    std::vector<uint64_t> closed;
    {
      std::lock_guard<std::mutex> lock(m_clientsMutex);
      closed.swap(m_closedConnections);
    }
    for (uint64_t connection : closed)
      if (const size_t n = ui::CancelJobsOwnedBy(state, connection,
                                                 "its client disconnected"))
        LOG_INFO("Remote control: cancelled {} study job(s) of a client that "
                 "disconnected",
                 n);
  }
  // Parked requests next: one whose passes just landed is answered on the
  // same tick, ahead of anything that arrived since.
  PollParked(state);
  // While one waits, the loop stays at full rate so it is answered the tick
  // its work lands (docs/rendering/frame-pacing.md#idle-throttle).
  const auto parkedActivity = [this] { return m_parked.empty() ? 0u : 1u; };

  // Finish an in-flight UI action once the engine is done with it
#ifdef HAVE_TEST_ENGINE
  if (m_uiActionInFlight && ImGuiTestEngine_IsTestQueueEmpty(m_engine)) {
    m_uiActionInFlight = false;
    m_uiAction = nullptr;
    if (s_UiActionTest->Output.Status == ImGuiTestStatus_Success) {
      Respond(m_uiActionClient, m_uiActionId, m_uiActionResult);
    } else {
      const std::string why = UiActionFailureReason(s_UiActionTest);
      LOG_ERROR("Remote UI action failed: {}",
                why.empty() ? "no reason logged" : why);
      RespondError(m_uiActionClient, m_uiActionId,
                   why.empty() ? "ui action failed"
                               : "ui action failed: " + why);
    }
  }
#endif

  // V3 sheet pump: one captured view per frame. Runs before the request
  // loop so the camera write for the next tile lands ahead of this frame's
  // scene update, and requests stay queued while a job is active.
  state.sheetCapture = m_sheet.active; // hide viewport chrome in the tiles
  if (m_sheet.active && !m_screenshotPending) {
    if (m_sheet.nextView < m_sheet.views.size()) {
      if (!ApplyStandardView(state, m_sheet.views[m_sheet.nextView], true,
                             m_sheet.hasFocus ? &m_sheet.focusMin : nullptr,
                             m_sheet.hasFocus ? &m_sheet.focusMax : nullptr)) {
        RespondError(m_sheet.client, m_sheet.id,
                     "unknown view: " + m_sheet.views[m_sheet.nextView] +
                         "; views: front, back, left, right, top, bottom, iso");
        for (const std::string& tile : m_sheet.tilePaths)
          std::filesystem::remove(tile);
        m_sheet = SheetJob{};
      } else {
        m_screenshotPath = m_sheet.tilePaths[m_sheet.nextView];
        m_screenshotPending = true;
        m_screenshotHeldFrames = 0;
        m_sheet.nextView++;
      }
    } else {
      rendersheet::Spec spec;
      spec.tilePaths = m_sheet.tilePaths;
      spec.labels = m_sheet.views;
      spec.footer = m_sheet.footer;
      spec.cropX = m_sheet.cropX;
      spec.cropY = m_sheet.cropY;
      spec.cropW = m_sheet.cropW;
      spec.cropH = m_sheet.cropH;
      std::string err;
      if (rendersheet::Composite(spec, m_sheet.outPath, &err)) {
        json resp = {{"path", m_sheet.outPath},
                     {"views", m_sheet.views},
                     {"footer", m_sheet.footer}};
        if (!m_sheet.warnings.empty())
          resp["warnings"] = m_sheet.warnings;
        Respond(m_sheet.client, m_sheet.id, resp);
      } else {
        RespondError(m_sheet.client, m_sheet.id, err);
      }
      for (const std::string& tile : m_sheet.tilePaths)
        std::filesystem::remove(tile);
      m_sheet = SheetJob{};
    }
  }

  // Serialize UI actions and screenshots: one at a time
  while (true) {
    Request request;
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      if ((m_uiActionInFlight || m_screenshotPending || m_sheet.active))
        return activity + 1; // in flight: full rate until it completes
      // The oldest request whose client has none parked: a parked request
      // holds only its OWN connection's later ones, so a second client's
      // ping or get_state is answered while a cam_verify waits.
      auto next = m_requests.begin();
      for (; next != m_requests.end(); ++next) {
        bool held = false;
        for (const std::shared_ptr<Parked>& parked : m_parked)
          held |= parked->request.client == next->client;
        if (!held)
          break;
      }
      if (next == m_requests.end())
        return activity + parkedActivity();
      request = std::move(*next);
      m_requests.erase(next);
    }
    activity++;

    // Every key against the method's describe row, BEFORE anything runs --
    // parking, a handler, a UI action, a screenshot. A key no path of the
    // handler reads is refused while refusing still means "nothing
    // happened": cam_post {_timeout} used to write its G-code and only then
    // say the call was wrong (docs/scripting/rpc.md#parameters).
    if (!request.resumed) {
      // A method with an inspection contract refuses through it first: its
      // refusals carry a machine-readable diagnostic the plain one has not.
      try {
        remote::ValidateInspectionRequest(request.method, request.params);
      } catch (const remote::ContractError& e) {
        Send(request.client, {{"id", request.id},
                              {"error", e.what()},
                              {"diagnostic", e.diagnostic}});
        continue;
      }
      std::optional<remote::ContractError> refusal =
          UndeclaredParamsError(request.method, request.params);
      if (!refusal)
        refusal = NulStringError(request.method, request.params);
      // Then the values, for a method whose row is typed
      // (docs/scripting/rpc.md#typed-contracts): a missing key, a wrong
      // type, an enum or a range is refused here, before anything runs.
      if (!refusal) {
        try {
          ValidateRequestContract(request.method, request.params);
        } catch (const remote::ContractError& e) {
          refusal = e;
        }
      }
      if (refusal) {
        Send(request.client, {{"id", request.id},
                              {"error", refusal->what()},
                              {"diagnostic", refusal->diagnostic}});
        continue;
      }
    }

    // A request that would compute on the frame waits instead
    // (remote/RemoteParking.h): a CAM one on its passes, a drawing one on its
    // projection, an exact interference of two named sides on its boolean. A
    // `batch` never parks -- its calls run inside HandleSemantic, one
    // transaction -- and neither does a request coming back from parking. Nor
    // does a drawing request on --headless, where no frame is held and the
    // synchronous handler is the simpler answer.
    if (!request.resumed) {
      auto parked = std::make_shared<Parked>();
      parked->request = std::move(request);
      parked->params =
          std::make_unique<remote::RpcParams>(parked->request.params);
      try {
        const watchdog::Label label("RPC " + parked->request.method +
                                    " (parking)");
        parked->wait = remote::ParkCamRequest(state, parked->request.method,
                                              *parked->params);
        if (!parked->wait && m_mode != RemoteMode::Headless)
          parked->wait = remote::ParkDrawingRequest(
              state, parked->request.method, *parked->params);
        if (!parked->wait)
          parked->wait = remote::ParkInspectionRequest(
              state, parked->request.method, parked->request.params,
              *parked->params);
      } catch (...) {
        parked->wait = nullptr; // the handler says what went wrong
      }
      if (parked->wait) {
        m_parked.push_back(std::move(parked));
        continue;
      }
      request = std::move(parked->request);
    }

    // Outside the try: the catch below reports which keys the handler read,
    // which is half of what makes a type error fixable from the message.
    remote::RpcParams params{request.params};
    // Where the active tab's histories stood while a semantic handler runs:
    // a call it refuses -- a type error on its fifth key, a range check after
    // the edit it guards -- is taken back whole, so an error means nothing
    // happened (docs/scripting/rpc.md#a-refused-call-changes-nothing). Not
    // for a method that reaches outside the document (NotInBatch: a file, a
    // tab, the git tree), which has nothing here to roll back and may have
    // moved the tab the marks were taken on; and not for `batch`, which
    // takes its own.
    std::optional<EditMarks> marks;
    const auto rolledBack = [&]() -> std::string {
      if (!marks)
        return {};
      const EditMarks taken = *marks;
      marks.reset();
      std::string undone;
      try {
        undone = RollBackEdits(state, taken);
      } catch (const std::exception& e) {
        return std::string(" Rolling the call back failed too: ") + e.what();
      } catch (const Standard_Failure&) {
        return " Rolling the call back failed too (OpenCASCADE).";
      }
      return undone.empty() ? std::string()
                            : " The call was rolled back (" + undone +
                                  "), so the document is as it was.";
    };
    try {
      bool handled = false;
      // Named on the watchdog's stall line if it takes that long.
      const watchdog::Label rpcLabel("RPC " + request.method);
      LogRpc(request.method, request.params);
      const RpcEditWatch editWatch(state);
      // Who is asking, for a study job to belong to and a synchronous run to
      // stop on; cleared however the handler leaves.
      struct Caller {
        ui::AppState& state;
        ~Caller() {
          state.rpcConnection = 0;
          state.rpcCallerGone = nullptr;
        }
      } caller{state};
      state.rpcConnection = request.connection;
      state.rpcCallerGone = [this, connection = request.connection] {
        return !ConnectionOpen(connection);
      };
      if (request.method != "batch" &&
          !(remote::MethodFlagsOf(request.method) &
            (remote::NotInBatch | remote::UiAction)))
        marks = MarkEdits(state);
      json result = HandleSemantic(request, state, handled, params);
      if (!handled)
        marks.reset(); // nothing ran: what follows is a capture or the GUI
      if (handled) {
        // A key the row lists but THIS path never read: a key that means
        // nothing in the mode the call asked for (`target` on an op whose
        // depths were typed). A key outside the row never gets here -- it was
        // refused before the handler ran -- so this is the narrow remainder,
        // and the edit is rolled back with the refusal. A method that reaches
        // outside the document has nothing to roll back, so it says that it
        // ran rather than let the error read as "nothing happened".
        if (auto unknown = params.UnknownKeys(); !unknown.empty()) {
          const bool undoable = marks.has_value();
          std::string message =
              UnknownParamError(request.method, unknown, params.SeenKeys());
          message += undoable
                         ? rolledBack()
                         : " The call had already run: '" + request.method +
                               "' reaches outside the document, so it "
                               "cannot be rolled back.";
          RespondError(request.client, request.id, message);
          continue;
        }
        marks.reset();
        // Session recording (S4): successful semantic requests only — a
        // throwing call mutates nothing, so replay equivalence holds without
        // it (UI actions/screenshots are also skipped: not
        // headless-replayable).
        if (m_recording.is_open() && request.method != "ping") {
          m_recording << json{{"method", request.method},
                              {"params", request.params}}
                             .dump()
                      << "\n";
          m_recording.flush();
        }
        // A guarded write answers with the revision it left, so the next
        // write in a chain can guard against this one
        // (docs/scripting/rpc.md#the-revision-guard).
        if (request.params.is_object() &&
            request.params.contains("expect_revision") && result.is_object())
          result["revision"] = ActivePartRevision(state);
        Respond(request.client, request.id, result);
        continue;
      }
      if (request.method == "screenshot") {
        if (!CaptureAvailable()) {
          RespondError(request.client, request.id,
                       "screenshot requires a rendered frame — start "
                       "lobole without --headless (--offscreen renders "
                       "without a window)");
          continue;
        }
        // Drawing sheets are drawn by the GUI layer, which the offscreen
        // capture deliberately excludes — returning the empty frame here
        // cost a real session an eyeball round on five blank PNGs.
        if (m_mode == RemoteMode::Offscreen && state.drawing) {
          RespondError(request.client, request.id,
                       "a drawing sheet renders in the GUI layer, which "
                       "--offscreen does not draw — export_drawing writes "
                       "the sheet as SVG/PDF, or screenshot from a windowed "
                       "instance");
          continue;
        }
        // Optional `view` reorients + fits before the capture, matching the
        // MCP wrapper so raw callers get the oriented shot too.
        if (request.params.contains("view")) {
          const std::string view = request.params.at("view").get<std::string>();
          if (!ApplyStandardView(state, view,
                                 request.params.value("fit", true))) {
            RespondError(
                request.client, request.id,
                "unknown view: " + view +
                    "; views: front, back, left, right, top, bottom, iso");
            continue;
          }
        }
        m_screenshotId = request.id;
        m_screenshotClient = request.client;
        m_screenshotPath = request.params.at("path").get<std::string>();
        m_screenshotPending = true;
        m_screenshotHeldFrames = 0;
        return activity + parkedActivity(); // main loop captures after
      }
      if (request.method == "render_sheet") {
        // V3: stage a multi-view sheet job; the ProcessPending pump captures
        // one view per frame and composites when all tiles are in.
        if (!CaptureAvailable()) {
          RespondError(request.client, request.id,
                       "render_sheet requires a rendered frame — start "
                       "lobole without --headless (--offscreen renders "
                       "without a window)");
          continue;
        }
        const std::string outPath =
            request.params.at("path").get<std::string>();
        std::vector<std::string> views = {"iso", "front", "right", "top"};
        if (request.params.contains("views")) {
          views.clear();
          for (const auto& v : request.params.at("views"))
            views.push_back(v.get<std::string>());
        }
        if (views.empty() || views.size() > 6) {
          RespondError(request.client, request.id,
                       "render_sheet takes 1..6 views");
          continue;
        }
        m_sheet = SheetJob{};
        m_sheet.views = views;
        m_sheet.outPath = outPath;
        m_sheet.footer = BuildSheetFooter(state, state.document.get());
        for (size_t i = 0; i < views.size(); i++)
          m_sheet.tilePaths.push_back(
              (std::filesystem::temp_directory_path() /
               ("designer-sheet-" + std::to_string(m_port) + "-" +
                std::to_string(i) + ".png"))
                  .string());
        // Crop each tile to the 3D viewport (panel chrome stays out of the
        // sheet). Framebuffer scale is 1 on this platform; Composite clamps
        // a stale rect instead of failing.
        m_sheet.cropX = static_cast<int>(state.viewportPos.x);
        m_sheet.cropY = static_cast<int>(state.viewportPos.y);
        m_sheet.cropW = static_cast<int>(state.viewportSize.x);
        m_sheet.cropH = static_cast<int>(state.viewportSize.y);
        if (request.params.contains("focus")) {
          // Round-8 retro: frame ONE occurrence large in every view — the
          // bend-direction misread happened because the interesting part
          // was 40 pixels tall in a whole-dock tile.
          if (state.assembly == nullptr) {
            RespondError(request.client, request.id,
                         "focus frames an occurrence, so it needs an "
                         "assembly tab (see list_parts)");
            continue;
          }
          const nlohmann::json& f = request.params.at("focus");
          const Occurrence* found = nullptr;
          if (f.is_string()) {
            for (const Occurrence& o : state.assembly->GetOccurrences())
              if (o.name == f.get<std::string>())
                found = &o;
          } else {
            found = state.assembly->FindById(f.get<uint64_t>());
          }
          if (found == nullptr) {
            RespondError(request.client, request.id,
                         "no occurrence " + f.dump() +
                             " to focus (see list_occurrences)");
            continue;
          }
          const Document* pd = ui::OccurrencePartDoc(state, *found);
          glm::dvec3 lo(0.0), hi(0.0);
          if (pd == nullptr ||
              !Measure::WorldBounds(pd->GetBodies(),
                                    ui::OccurrenceMatrix(*found), lo, hi)) {
            RespondError(request.client, request.id,
                         "occurrence '" + found->name +
                             "' has no geometry to frame");
            continue;
          }
          // The camera frames in float; the box narrows here.
          m_sheet.focusMin = glm::vec3(lo);
          m_sheet.focusMax = glm::vec3(hi);
          m_sheet.hasFocus = true;
          m_sheet.footer.insert(m_sheet.footer.begin(),
                                "FOCUS: " + found->name);
        }
        m_sheet.id = request.id;
        m_sheet.client = request.client;
        m_sheet.active = true;
        // the pump kicks tile 0 next frame; requests queue behind
        return activity + parkedActivity();
      }
      if (!UiActionsAvailable() && IsUiAction(request.method)) {
        // Three different reasons, three different remedies: nothing to
        // click (no GUI running), no GUI at all (--offscreen renders but
        // draws no interface), or nothing to click WITH (this build has no
        // test engine). Saying which is the difference between "run it
        // differently" and "build it differently".
        RespondError(
            request.client, request.id,
            m_mode == RemoteMode::Offscreen
                ? "UI actions are unavailable in --offscreen — there is no "
                  "GUI to drive. The semantic RPCs and renders all work."
            : m_mode == RemoteMode::Gui
                ? "UI actions need the ImGui Test Engine, and this build was "
                  "configured with -DENABLE_TEST_ENGINE=OFF. The semantic "
                  "RPCs (add_feature, measure, screenshot, …) all work."
                : "UI actions are unavailable in --headless — start lobole "
                  "without it");
        continue;
      }
      if (QueueUiAction(request)) {
        // response sent when the engine finishes the action
        return activity + parkedActivity();
      }
      RespondError(request.client, request.id,
                   "unknown method: " + request.method);
    } catch (const remote::ContractError& e) {
      json diagnostic = e.diagnostic;
      // RpcParams' missing-key refusal does not know its method.
      if (diagnostic.value("operation", "").empty())
        diagnostic["operation"] = request.method;
      Send(request.client, {{"id", request.id},
                            {"error", e.what() + rolledBack()},
                            {"diagnostic", diagnostic}});
    } catch (const json::type_error& e) {
      // A parameter of the wrong TYPE used to come back as nlohmann's own
      // words — "[json.exception.type_error.302] type must be string, but is
      // boolean" — which name neither the method, nor the parameter, nor
      // what would have been accepted. The refusal convention ("name what IS
      // accepted") was honoured for enums and skipped for types. Say the
      // same thing a caller needs: which call, what it was handed, and where
      // the vocabulary is.
      RespondError(request.client, request.id,
                   TypeErrorMessage(request.method, request.params,
                                    params.SeenKeys(), e.what()) +
                       rolledBack());
    } catch (...) {
      // Everything else. A deliberate refusal (std::runtime_error) is sent
      // as it was said. A handler that fell over -- std::logic_error and
      // its out_of_range, nlohmann's out_of_range (an `at` on a key nobody
      // checked), bad_optional_access, bad_alloc -- is labelled `internal`
      // in its diagnostic, so a caller can tell a handler that checked its
      // input from one that did not (remote/RpcErrors.h). So is the last
      // line of defence: OCC reports failure by throwing Standard_Failure,
      // which does NOT derive from std::exception, so a kernel throw from
      // any handler that forgot its own guard would escape every catch and
      // terminate the process. The call is rolled back either way.
      const remote::CaughtError caught =
          remote::DescribeCurrentException(request.method);
      const std::string message = caught.message + rolledBack();
      if (caught.diagnostic.is_null())
        RespondError(request.client, request.id, message);
      else
        Send(request.client, {{"id", request.id},
                              {"error", message},
                              {"diagnostic", caught.diagnostic}});
    }
  }
}
