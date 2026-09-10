#include "remote/RemoteDrawings.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingValidate.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteProject.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Drawing / sheet RPCs (create_drawing..) carved out of
// RemoteServer::HandleSemantic. Act on the active drawing tab / project.
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc DrawingMethods[] = {
    {"create_drawing",
     "Add a drawing document (sheet size, title block) to the project.",
     "name, sheet"},
    {"set_drawing_sheet",
     "Edit the active drawing's sheet {size?, landscape?, title_block?}.",
     "landscape, size, title_block"},
    {"list_drawings", "List drawing documents.", ""},
    {"set_drawing_view",
     "Edit a view {id, position?, scale?, name?, "
     "show_hidden?, quality?, base_view?}; folded axes note. "
     "`quality` picks the projection: \"exact\" (the default — curve HLR, "
     "every curve traced to the 3D edge that made it, which is what a "
     "dimension attaches to) or \"fast\" (polygonal HLR over the shapes' "
     "triangulations: orders of magnitude cheaper, and a LOOK — polylines with "
     "no source refs, so nothing can be dimensioned against them). Exact is "
     "never silently swapped for fast when it is slow; ask for it. An "
     "exact costs what the SURFACES cost, not the part count: 16 bored "
     "boxes are 3.1 ms a view, one 4-turn helical spring is 6.4 s. "
     "`hidden` leaves occurrences OFF this view, by id or name, refused if the "
     "name is not in the source. It is a per-SHEET choice, not a model change. "
     "Exact HLR is priced by surface complexity, so one swept part can "
     "dominate a sheet: the single-cylinder engine is 47 s a view and 0.0 s "
     "with its two valve springs left off, while the other seventeen bodies "
     "still draw exactly. ",
     "base_view, hidden, id, name, position, quality, scale, show_hidden"},
    {"check_drawing",
     "Validate sheet layout: views vs border/title block, overlaps, dim "
     "placement, dangling/stale {update?}. {drawing?} (name or id) checks "
     "THAT sheet rather than whichever one is active — a report about the "
     "wrong sheet is worse than none.",
     "drawing, update"},
    {"add_drawing_view",
     "Add an HLR view of a part/assembly {drawing, source, orientation, "
     "scale, position}. Omit BOTH scale and position and it is placed the "
     "way the Add view button places it: fitted to the sheet, clear of the "
     "views already there, and projected (needs_update false). "
     "`quality` picks the projection: \"exact\" (the default — curve HLR, "
     "every curve traced to the 3D edge that made it, which is what a "
     "dimension attaches to) or \"fast\" (polygonal HLR over the shapes' "
     "triangulations: orders of magnitude cheaper, and a LOOK — polylines with "
     "no source refs, so nothing can be dimensioned against them). Exact is "
     "never silently swapped for fast when it is slow; ask for it. An "
     "exact costs what the SURFACES cost, not the part count: 16 bored "
     "boxes are 3.1 ms a view, one 4-turn helical spring is 6.4 s. "
     "`hidden` leaves occurrences OFF this view, by id or name, refused if the "
     "name is not in the source. It is a per-SHEET choice, not a model change. "
     "Exact HLR is priced by surface complexity, so one swept part can "
     "dominate a sheet: the single-cylinder engine is 47 s a view and 0.0 s "
     "with its two valve springs left off, while the other seventeen bodies "
     "still draw exactly. ",
     "base_view, drawing, hidden, name, orientation, position, quality, "
     "scale, source"},
    {"remove_drawing_view", "Remove a view by id.", "id"},
    {"list_drawing_views", "List views with placement and scale.", ""},
    {"update_drawing",
     "Re-project all views from current geometry. {drawing?} names the sheet. "
     "`budget_s` bounds the WHOLE "
     "update: a view still running when it is gone stops, keeps its previous "
     "curves and reports that it ran out, so a slow sheet costs what you said "
     "instead of blocking the server for as long as it takes. Exact HLR is "
     "priced by SURFACE complexity, not part count — 16 bored boxes are "
     "3.1 ms a view and one 4-turn helical spring is 6.4 s — so a view's "
     "`quality: \"fast\"` is the other lever. Omit for no budget.",
     "budget_s, drawing"},
    {"list_drawing_geometry",
     "Projected 2D curves of a view (for picking/dimensioning).", "view"},
    {"add_drawing_dim", "Add a driven dimension between picked view geometry.",
     "kind, offset, refs, view"},
    {"remove_drawing_dim", "Remove a dimension by id.", "id"},
    {"list_drawing_dims", "List dimensions with values.", ""},
    {"export_drawing", "Export the drawing to SVG or PDF by extension {path}.",
     "path", remote::NotInBatch},
    {"add_drawing_note",
     "Add a text note ({{expression}} resolves project variables, or "
     "source-part variables with view) {text, position: [x,y] sheet mm, view?, "
     "ref?: "
     "{feature_id, element}} — ref draws a leader from that geometry (needs "
     "view). The annotation class no dimension kind covers.",
     "position, ref, text, view"},
    {"remove_drawing_note", "Remove a note by id.", "id"},
    {"list_drawing_notes", "List notes with leader state.", ""},
};
std::span<const MethodDoc> DrawingMethodDocs() { return DrawingMethods; }

namespace {

// Parse a sheet-size string, throwing the accepted list on failure (the
// silent A3-stays-A4 bug class). Case handled by SheetSizeFromString.
SheetSize ParseSheetSizeOrThrow(const std::string& s) {
  SheetSize size;
  if (!SheetSizeFromString(s, size))
    throw std::runtime_error("unknown sheet size '" + s +
                             "' (expected one of: a4, a3, a2, a1, a0, "
                             "letter)");
  return size;
}

// A `sheet` parameter that is not an object dies deep in nlohmann as a raw
// type_error — `{"sheet": "A4"}` was the first thing an agent tried. Refuse
// naming the accepted shape, BEFORE any tab exists to half-configure.
void RequireSheetObject(const json& sj) {
  if (!sj.is_object())
    throw std::runtime_error(
        "sheet is an object: {size: a4|a3|a2|a1|a0|letter, landscape?: "
        "bool, title_block?: {project, document, author, date, notes}} — "
        "for just a size, pass {\"size\": \"a4\"}");
  if (sj.contains("title_block") && !sj.at("title_block").is_object())
    throw std::runtime_error(
        "title_block is an object of strings: {project?, document?, "
        "author?, date?, notes?}");
}

// Apply a create_drawing/set_drawing_sheet `sheet` JSON block. The shape
// (RequireSheetObject) and the size string (ParseSheetSizeOrThrow) must
// already be validated so this never half-applies.
void ApplySheetJson(Sheet& sheet, const json& sj) {
  if (sj.contains("size"))
    sheet.size = ParseSheetSizeOrThrow(sj.at("size").get<std::string>());
  sheet.landscape = sj.value("landscape", sheet.landscape);
  if (sj.contains("title_block")) {
    const json& tb = sj.at("title_block");
    sheet.titleBlock.project = tb.value("project", sheet.titleBlock.project);
    sheet.titleBlock.document = tb.value("document", sheet.titleBlock.document);
    sheet.titleBlock.author = tb.value("author", sheet.titleBlock.author);
    sheet.titleBlock.date = tb.value("date", sheet.titleBlock.date);
    sheet.titleBlock.notes = tb.value("notes", sheet.titleBlock.notes);
  }
}

// The layout report shared by check_drawing and export_drawing: resolve
// every dim, compute per-view staleness, run the validator. Reads only
// derived state (plus the document) — never mutates or snapshots.
json DrawingIssuesJson(ui::AppState& state) {
  std::vector<drawing::ResolvedDim> dims;
  for (const DrawingDim& dim : state.drawing->GetDims())
    dims.push_back(ui::ResolveDrawingDim(state, dim));
  std::map<uint64_t, bool> stale;
  for (const DrawingView& view : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(view.id);
    stale[view.id] = it != state.drawingRenders.end() &&
                     ui::DrawingSourceVersion(state, view.sourceDocId) !=
                         it->second.solvedSourceVersion;
  }
  std::vector<drawing::ResolvedNote> notes;
  for (const auto& note : state.drawing->GetNotes())
    notes.push_back(ui::ResolveDrawingNote(state, note));
  json arr = json::array();
  for (const drawing::DrawingIssue& i : drawing::ValidateDrawing(
           *state.drawing, state.drawingRenders, dims, stale, notes)) {
    json ij = {{"kind", i.kind}, {"detail", i.detail}};
    if (i.viewId)
      ij["view"] = i.viewId;
    if (i.otherViewId)
      ij["other_view"] = i.otherViewId;
    if (i.noteId)
      ij["note"] = i.noteId;
    if (i.dimId)
      ij["dim"] = i.dimId;
    arr.push_back(std::move(ij));
  }
  return arr;
}

} // namespace

namespace {

// "exact" or "fast", REFUSED by name otherwise. A quality nobody recognises
// silently becoming "exact" is the substitution this project does not make —
// a sheet asked for fast and given exact takes a minute and a half a view and
// looks like a hang, and one asked for exact and given fast is a look
// presented as a deliverable.
// The occurrences a view leaves off, by id or by name, refused when the name
// is not in the source assembly — a silent miss would draw the spring you
// meant to drop and cost a minute a view to find out.
std::vector<uint64_t> ParseHidden(ui::AppState& state, const DrawingView& view,
                                  const nlohmann::json& j) {
  std::vector<uint64_t> out;
  if (!j.is_array())
    throw std::runtime_error("`hidden` is an array of occurrence ids or names");
  const AssemblyDocument* asmDoc = nullptr;
  for (const ui::PartSlot& slot : state.parts)
    if (slot.id == view.sourceDocId && slot.kind == ui::DocKind::Assembly)
      asmDoc = slot.assembly ? slot.assembly.get() : state.assembly.get();
  for (const auto& e : j) {
    if (e.is_number_unsigned()) {
      out.push_back(e.get<uint64_t>());
      continue;
    }
    const std::string name = e.get<std::string>();
    std::vector<uint64_t> hits;
    std::vector<std::string> known;
    if (asmDoc != nullptr)
      for (const Occurrence& o : asmDoc->GetOccurrences()) {
        known.push_back(o.name);
        if (o.name == name)
          hits.push_back(o.id);
      }
    if (hits.empty())
      throw std::runtime_error("no occurrence '" + name +
                               "' in this view's source — " + KnownList(known));
    // A part placed twice answers to one name, and picking the first would
    // drop one spring and leave the other on the sheet — at a minute a view,
    // you find out slowly. Same rule as the fastener picks: say which.
    if (hits.size() > 1) {
      std::string ids;
      for (size_t k = 0; k < hits.size(); k++)
        ids += (k ? ", " : "") + std::to_string(hits[k]);
      throw std::runtime_error("'" + name + "' is " +
                               std::to_string(hits.size()) +
                               " occurrences in this view's source (" + ids +
                               ") — say which by id, or list them all");
    }
    out.push_back(hits.front());
  }
  return out;
}

std::string RequireQuality(const nlohmann::json& j) {
  const std::string q = j.get<std::string>();
  if (q != "exact" && q != "fast")
    throw std::runtime_error(
        "unknown view quality '" + q +
        "' — it is \"exact\" (curve HLR: every curve traced to the 3D edge "
        "that made it, which is what a dimension attaches to) or \"fast\" "
        "(polygonal HLR: orders of magnitude cheaper, polylines, nothing to "
        "dimension against)");
  return q;
}

namespace {

// `drawing` (name or id) switches to that tab first. Every verb that acts on
// A drawing takes it: without one, `check_drawing` judged whichever sheet
// happened to be active, which is how a general arrangement shipped with its
// side view inside its top view while its report said "ok".
void SelectDrawing(ui::AppState& state, const RpcParams& p) {
  if (!p.contains("drawing"))
    return;
  const nlohmann::json& which = p.at("drawing");
  for (size_t i = 0; i < state.parts.size(); i++) {
    const auto& slot = state.parts[i];
    if (slot.kind != ui::DocKind::Drawing)
      continue;
    if (which.is_number() ? slot.id == which.get<uint64_t>()
                          : slot.name == which.get<std::string>()) {
      ui::SetActivePart(state, i);
      return;
    }
  }
  std::string have;
  for (const auto& slot : state.parts)
    if (slot.kind == ui::DocKind::Drawing)
      have += (have.empty() ? "" : ", ") + slot.name;
  throw std::runtime_error("no drawing named " + which.dump() +
                           "; drawings: " + (have.empty() ? "(none)" : have));
}

} // namespace

} // namespace

std::optional<json> HandleDrawingRpc(ui::AppState& state, const std::string& m,
                                     const RpcParams& p) {
  if (m == "create_drawing") {
    // Adds a drawing tab. Optional `sheet` seeds paper size / orientation /
    // title-block fields so a headless caller can set them in one call.
    std::string name;
    if (p.contains("name")) {
      name = p.at("name").get<std::string>();
      RequireFreshTabName(state, name);
    } else {
      name = FreshDefaultTabName(state, "Drawing");
    }
    // Validate BEFORE creating the tab — a bad shape or size string must
    // not leave a half-configured drawing behind.
    if (p.contains("sheet")) {
      RequireSheetObject(p.at("sheet"));
      if (p.at("sheet").contains("size"))
        ParseSheetSizeOrThrow(p.at("sheet").at("size").get<std::string>());
    }
    const size_t idx = ui::AddDrawing(state, name);
    if (p.contains("sheet") && state.drawing) {
      ApplySheetJson(state.drawing->GetSheet(), p.at("sheet"));
      state.drawing->BumpVersion();
    }
    return json{{"index", idx},
                {"name", name},
                {"id", state.parts[idx].id},
                {"size", SheetSizeToString(state.drawing->GetSheet().size)}};
  }
  if (m == "set_drawing_sheet") {
    // Edit the active drawing's sheet after creation (was GUI-only). params:
    // {size?, landscape?, title_block?{project,document,author,date,notes}}.
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    if (p.contains("size"))
      ParseSheetSizeOrThrow(p.at("size").get<std::string>()); // validate first
    if (p.contains("title_block") && !p.at("title_block").is_object())
      throw std::runtime_error(
          "title_block is an object of strings: {project?, document?, "
          "author?, date?, notes?}");
    ui::SnapshotDrawing(state);
    json sj = json::object();
    if (p.contains("size"))
      sj["size"] = p.at("size");
    if (p.contains("landscape"))
      sj["landscape"] = p.at("landscape");
    if (p.contains("title_block"))
      sj["title_block"] = p.at("title_block");
    ApplySheetJson(state.drawing->GetSheet(), sj);
    state.drawing->BumpVersion();
    state.sheetView.scale = 0.0; // refit the 2D camera to the new paper
    const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
    return json{{"size", SheetSizeToString(state.drawing->GetSheet().size)},
                {"landscape", state.drawing->GetSheet().landscape},
                {"extent", {chrome.extent.x, chrome.extent.y}}};
  }
  if (m == "list_drawings") {
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (state.parts[i].kind != ui::DocKind::Drawing)
        continue;
      // The active drawing lives in AppState; a stashed one in its slot.
      const DrawingDocument* dwg = static_cast<int>(i) == state.activePart
                                       ? state.drawing.get()
                                       : state.parts[i].drawing.get();
      arr.push_back({{"name", state.parts[i].name},
                     {"index", i},
                     {"id", state.parts[i].id},
                     {"views", dwg ? dwg->ViewCount() : 0},
                     {"dims", dwg ? dwg->DimCount() : 0}});
    }
    return json{{"drawings", arr}};
  }
  if (m == "add_drawing_view") {
    // SelectDrawing reads it; the scanner cannot follow a helper.
    // rpc-params: drawing
    // `drawing` (name or id) switches to that drawing tab first — the doc
    // has always described it, and the handler used to refuse the key.
    if (p.contains("drawing"))
      SelectDrawing(state, p);
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    // `source` = the source document's NAME (a part or an assembly), resolved
    // to its stable id.
    const std::string sourceName = p.at("source").get<std::string>();
    uint64_t sourceId = 0;
    for (const auto& slot : state.parts) {
      if (slot.name == sourceName && (slot.kind == ui::DocKind::Part ||
                                      slot.kind == ui::DocKind::Assembly)) {
        sourceId = slot.id;
        break;
      }
    }
    if (sourceId == 0)
      throw std::runtime_error("no part or assembly named '" + sourceName +
                               "'");
    DrawingView v;
    v.sourceDocId = sourceId;
    if (p.contains("orientation") &&
        !ViewOrientationFromString(p.at("orientation").get<std::string>(),
                                   v.orientation))
      throw std::runtime_error("unknown orientation '" +
                               p.at("orientation").get<std::string>() + "'");
    // Default the label to the orientation (front/top/iso…) — clearer on the
    // sheet than repeating the source name across every view.
    v.name =
        p.value("name", std::string(ViewOrientationToString(v.orientation)));
    v.baseViewId = p.value("base_view", uint64_t{0});
    // Say nothing about size or place and you get what the "Add view" button
    // gives: a standard scale that fits this sheet, and a spot clear of the
    // views already on it. This RPC dropped every view at the sheet centre at
    // 1:1, so three scripted views landed in one pile while three clicked
    // ones laid themselves out — the same action, two behaviours.
    const bool autoPlace = !p.contains("position") && !p.contains("scale");
    v.scale = p.value("scale", 1.0);
    if (p.contains("quality"))
      v.quality = RequireQuality(p.at("quality"));
    if (p.contains("hidden"))
      v.hidden = ParseHidden(state, v, p.at("hidden"));
    if (p.contains("position")) {
      const json& pos = p.at("position");
      v.position = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    } else { // default to the sheet centre
      v.position = ChromeOf(state.drawing->GetSheet()).extent * 0.5;
    }
    if (autoPlace) {
      // AddDrawingViewPlaced snapshots and re-projects itself, so it goes
      // BEFORE the snapshot below rather than alongside it.
      v.scale = 0.0; // <= 0 asks it to choose a standard scale
      const uint64_t id = ui::AddDrawingViewPlaced(state, v);
      // Placing a view means measuring it, so it is already projected.
      return json{{"id", id},
                  {"source_id", sourceId},
                  {"needs_update", false},
                  {"auto_placed", true}};
    }
    ui::SnapshotDrawing(state);
    const uint64_t id = state.drawing->AddView(v);
    // Projection is explicit (design §2, the FEM rule): the view is added now;
    // call update_drawing to project it.
    return json{{"id", id}, {"source_id", sourceId}, {"needs_update", true}};
  }
  if (m == "remove_drawing_view") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::SnapshotDrawing(state);
    if (!state.drawing->RemoveView(p.at("id").get<uint64_t>())) {
      state.drawingHistory.undo
          .pop_back(); // nothing changed; drop the snapshot
      throw std::runtime_error("no such view");
    }
    ui::UpdateDrawingRenders(state); // drop the removed view's geometry
    return json::object();
  }
  if (m == "set_drawing_view") {
    // Edit a view in place — the missing mutation that used to force
    // remove+re-add (losing the view's dims). params: {id, position?: [x,y],
    // scale?, name?, show_hidden?, base_view?}. Everything is validated
    // BEFORE the undo snapshot so a failed call leaves no undo entry.
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    DrawingView* view = state.drawing->FindView(p.at("id").get<uint64_t>());
    if (!view)
      throw std::runtime_error("no such view");
    if (p.contains("scale") && p.at("scale").get<double>() <= 0.0)
      throw std::runtime_error("scale must be positive");
    if (p.contains("base_view")) {
      const uint64_t baseId = p.at("base_view").get<uint64_t>();
      if (baseId != 0) {
        const DrawingView* base = state.drawing->FindView(baseId);
        if (!base || baseId == view->id)
          throw std::runtime_error("base_view must name another view");
        if (base->baseViewId == view->id)
          throw std::runtime_error(
              "base_view cycle: that view already folds from this one");
      }
    }
    ui::SnapshotDrawing(state);
    if (p.contains("position")) {
      const json& pos = p.at("position");
      view->position = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    }
    if (p.contains("scale"))
      view->scale = p.at("scale").get<double>();
    if (p.contains("name"))
      view->name = p.at("name").get<std::string>();
    if (p.contains("show_hidden"))
      view->showHidden = p.at("show_hidden").get<bool>();
    if (p.contains("quality"))
      view->quality = RequireQuality(p.at("quality"));
    if (p.contains("hidden"))
      view->hidden = ParseHidden(state, *view, p.at("hidden"));
    if (p.contains("base_view"))
      view->baseViewId = p.at("base_view").get<uint64_t>();
    state.drawing->BumpVersion();
    ui::UpdateDrawingRenders(state); // bboxes/dims are baked at projection

    const ui::FoldedPosition folded = ui::DrawingFoldedPosition(state, *view);
    json out = {{"id", view->id},
                {"position", {view->position.x, view->position.y}},
                {"effective_position", {folded.pos.x, folded.pos.y}}};
    json axes = json::array();
    if (folded.xLocked)
      axes.push_back("x");
    if (folded.yLocked)
      axes.push_back("y");
    if (!axes.empty()) {
      out["folded_axes"] = axes;
      if (p.contains("position") && (folded.pos.x != view->position.x ||
                                     folded.pos.y != view->position.y))
        out["note"] = "a folded child's shared axis follows its base view; "
                      "the overridden coordinate was ignored";
    }
    return out;
  }
  if (m == "check_drawing") {
    // SelectDrawing reads it; the scanner cannot follow a helper.
    // rpc-params: drawing
    if (p.contains("drawing"))
      SelectDrawing(state,
                    p); // {drawing}: check THAT sheet, not the active one
    // Report-only sheet-layout validation. update=true (default) re-projects
    // first so the verdict reflects current geometry (matching what
    // export_drawing would produce); update=false judges the cached renders
    // and reports staleness instead. Derived-cache write only — no snapshot.
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    if (p.value("update", true))
      ui::UpdateDrawingRenders(state);
    const json issues = DrawingIssuesJson(state);
    json out{{"ok", issues.empty()},
             {"issues", issues},
             {"views", state.drawing->ViewCount()},
             {"dims", state.drawing->DimCount()}};
    {
      // A sheet with no views passes every check there is. That is the
      // "valid over nothing" shape again (core/Receipt.h): `ok: true` is
      // true and it is not a drawing anyone can build from.
      receipt::Receipt receipt;
      const receipt::Oracle oracle{"designer/drawing-rules", "1"};
      const std::string claim = "the drawing is complete and consistent";
      if (state.drawing->ViewCount() == 0) {
        receipt.Unverifiable(claim, oracle,
                             "this sheet has no views — there is nothing for "
                             "the drawing rules to check");
      } else {
        receipt::Claim c(claim, oracle,
                         issues.empty() ? receipt::Verdict::Pass
                                        : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Verified;
        c.value = static_cast<double>(state.drawing->DimCount());
        c.unit = "dimensions";
        c.note = std::to_string(state.drawing->ViewCount()) +
                 " view(s) checked against the drawing rules";
        receipt.Add(std::move(c));
        // A dimension-free drawing is not wrong, and it is not a drawing you
        // can make a part from either. Said, not enforced.
        if (state.drawing->DimCount() == 0)
          receipt.Unverifiable(
              "the drawing dimensions the part", oracle,
              "no dimensions on this sheet, so nothing here says how big "
              "anything is");
      }
      AttachReceipt(out, receipt);
    }
    return out;
  }
  if (m == "update_drawing") {
    // SelectDrawing reads it; the scanner cannot follow a helper.
    // rpc-params: drawing
    if (p.contains("drawing"))
      SelectDrawing(state, p); // {drawing}: re-project THAT sheet
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    // `budget_s` bounds the whole update. Without one it runs to completion,
    // which on a model carrying a swept surface can be a minute a view and
    // looks exactly like a hang.
    ui::UpdateDrawingRenders(state, p.value("budget_s", 0.0));
    json views = json::array();
    for (const DrawingView& view : state.drawing->GetViews()) {
      json vj = {{"id", view.id}, {"name", view.name}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        int vis = 0, hid = 0, outline = 0;
        for (const drawing::SheetCurve& c : it->second.curves) {
          if (c.visible)
            ++vis;
          else
            ++hid;
          if (c.kind == drawing::CurveKind::Outline)
            ++outline;
        }
        vj["curves"] = it->second.curves.size();
        vj["visible"] = vis;
        vj["hidden"] = hid;
        vj["silhouettes"] = outline;
        vj["has_geometry"] = it->second.hasGeometry;
        if (!it->second.error.empty())
          vj["error"] = it->second.error;
      }
      views.push_back(vj);
    }
    return json{{"views", views}};
  }
  if (m == "list_drawing_views") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    json arr = json::array();
    for (const DrawingView& view : state.drawing->GetViews()) {
      json vj = {{"id", view.id},
                 {"name", view.name},
                 {"source_id", view.sourceDocId},
                 {"orientation", ViewOrientationToString(view.orientation)},
                 {"scale", view.scale},
                 {"base_view", view.baseViewId},
                 {"position", {view.position.x, view.position.y}},
                 {"show_hidden", view.showHidden},
                 {"quality", view.quality},
                 {"hidden", view.hidden}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        vj["curves"] = it->second.curves.size();
        vj["has_geometry"] = it->second.hasGeometry;
        vj["stale"] = ui::DrawingSourceVersion(state, view.sourceDocId) !=
                      it->second.solvedSourceVersion;
        // Sheet-mm bounds of the projected geometry — the rect check_drawing
        // judges and set_drawing_view layouts against.
        vj["bbox"] = it->second.hasGeometry
                         ? json{{it->second.bboxMin.x, it->second.bboxMin.y},
                                {it->second.bboxMax.x, it->second.bboxMax.y}}
                         : json(nullptr);
        // What the projection COST, so "this sheet is slow" is a number
        // rather than a stopwatch. A view holds no geometry until something
        // projects it, and the bill is all HLR (W3).
        {
          const auto& stats = it->second.projectionStats;
          vj["cost"] = {{"outline_ms", it->second.updateMs},
                        {"hide_ms", it->second.hideMs},
                        {"hlr_edges", it->second.dsEdges},
                        {"bodies", it->second.bodies},
                        {"self_ms", stats.selfMs},
                        {"pair_ms", stats.pairMs},
                        {"self_calls", stats.selfCalls},
                        {"pair_calls", stats.pairCalls},
                        {"self_hits", stats.selfHits},
                        {"pair_hits", stats.pairHits},
                        {"projection_hit", stats.projectionHit},
                        {"cache_bytes", stats.cacheBytes}};
        }
        if (!it->second.error.empty())
          vj["error"] = it->second.error;
      } else {
        vj["curves"] = 0;
        vj["has_geometry"] = false;
        vj["stale"] = true; // never projected
        vj["bbox"] = nullptr;
      }
      arr.push_back(vj);
    }
    return json{{"views", arr}};
  }
  if (m == "add_drawing_note") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const std::string text = p.at("text").get<std::string>();
    if (text.empty())
      throw std::runtime_error("text must be non-empty");
    const json& pos = p.at("position");
    if (!pos.is_array() || pos.size() != 2 || !pos.at(0).is_number() ||
        !pos.at(1).is_number())
      throw std::runtime_error("position is [x, y] in sheet mm");
    DrawingNote n;
    n.text = text;
    n.pos = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    if (p.contains("view")) {
      n.viewId = p.at("view").get<uint64_t>();
      if (!state.drawing->FindView(n.viewId))
        throw std::runtime_error("no such view");
    }
    if (p.contains("ref")) {
      if (n.viewId == 0)
        throw std::runtime_error(
            "a leader ref needs `view` — the ref resolves against that "
            "view's projection");
      const json& rj = p.at("ref");
      n.leader.viewId = n.viewId;
      n.leader.edge = ElementRef{rj.value("feature_id", uint64_t{0}),
                                 rj.value("element", std::string{})};
    }
    const auto resolved = ui::ResolveDrawingNote(state, n);
    if (!resolved.resolved)
      throw std::runtime_error(resolved.error);
    ui::SnapshotDrawing(state);
    return json{{"id", state.drawing->AddNote(std::move(n))},
                {"text", resolved.text}};
  }
  if (m == "remove_drawing_note") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::SnapshotDrawing(state);
    if (!state.drawing->RemoveNote(p.at("id").get<uint64_t>()))
      throw std::runtime_error("no note with that id");
    return json::object();
  }
  if (m == "list_drawing_notes") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    json arr = json::array();
    for (const DrawingNote& n : state.drawing->GetNotes()) {
      const auto rn = ui::ResolveDrawingNote(state, n);
      arr.push_back({{"id", n.id},
                     {"text", n.text},
                     {"resolved_text", rn.text},
                     {"resolved", rn.resolved},
                     {"error", rn.error},
                     {"pos", {n.pos.x, n.pos.y}},
                     {"view", n.viewId},
                     {"leader_resolved", rn.hasLeader}});
    }
    return json{{"notes", arr}};
  }
  if (m == "export_drawing") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    if (!ui::ExportActiveDrawing(state, path, &err))
      throw std::runtime_error(err);
    // Echo the layout report (renders are fresh — ExportActiveDrawing just
    // updated them) so a script can't miss a broken sheet. Never blocks.
    return json{{"path", path},
                {"views", state.drawing->ViewCount()},
                {"issues", DrawingIssuesJson(state)}};
  }
  if (m == "list_drawing_geometry") {
    // Discovery for dimensioning: the distinct source edges/silhouettes of a
    // view, each with the ref to pass to add_drawing_dim and a summary of its
    // sheet-mm geometry (endpoints, or circle centre + radius).
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const uint64_t viewId = p.at("view").get<uint64_t>();
    const auto it = state.drawingRenders.find(viewId);
    if (it == state.drawingRenders.end())
      throw std::runtime_error("view not projected; call update_drawing first");
    // Deduplicate curves by their source ref (an edge splits into visible +
    // hidden segments): prefer a VISIBLE segment as the representative, and
    // OR `closed` across segments (a bore rim's full sweep may live on one
    // interval while another is a partial arc).
    const Document* srcDoc = nullptr;
    if (const DrawingView* view = state.drawing->FindView(viewId))
      srcDoc = ui::DrawingSourceDocument(state, view->sourceDocId);
    json edges = json::array();
    std::map<std::string, size_t> seen; // key -> index into `edges`
    for (const drawing::SheetCurve& c : it->second.curves) {
      const bool isEdge = c.edge.IsSet();
      const ElementRef& ref = isEdge ? c.edge : c.silFace;
      if (!ref.IsSet())
        continue;
      const std::string key = std::to_string(c.sourceId) + "|" +
                              (isEdge ? "e|" : "s|") +
                              std::to_string(ref.featureId) + "|" + ref.name;
      const auto fillGeometry = [&](json& e) {
        e["visible"] = c.visible;
        if (c.conicRadius > 0.0) {
          e["type"] = "circle";
          e["center"] = {c.conicCenter.x, c.conicCenter.y};
          e["radius"] = c.conicRadius;
          e["closed"] = c.closed; // full circle (bore) vs arc (fillet)
        } else if (c.points.size() >= 2) {
          e["type"] = c.curveType == 0 ? "line" : "curve";
          e["p1"] = {c.points.front().x, c.points.front().y};
          e["p2"] = {c.points.back().x, c.points.back().y};
        }
      };
      const auto found = seen.find(key);
      if (found != seen.end()) {
        json& e = edges[found->second];
        // A later full-sweep segment upgrades `closed`; a visible segment
        // replaces a hidden representative's geometry.
        if (c.conicRadius > 0.0 && c.closed && e.contains("closed"))
          e["closed"] = true;
        if (!e.value("visible", true) && c.visible) {
          const bool wasClosed = e.value("closed", false);
          fillGeometry(e);
          if (e.contains("closed"))
            e["closed"] = e["closed"].get<bool>() || wasClosed;
        }
        continue;
      }
      json e = {
          {"kind", isEdge ? "edge" : "silhouette"},
          {"source_id", c.sourceId},
          {"ref", {{"feature_id", ref.featureId}, {"element", ref.name}}}};
      // Which feature made THIS element (part sources only): the MappedName
      // encodes it (a hole's rim mentions the Hole feature id), which beats
      // ref.featureId — that is the body's creator, i.e. "Extrude" for every
      // element of the fused body. Prefer the LAST id in the name (the most
      // recent maker); fall back to the body creator.
      if (srcDoc) {
        const Feature* maker = nullptr;
        const auto ids = drawing::MappedNameFeatureIds(ref.name);
        for (auto it2 = ids.rbegin(); it2 != ids.rend() && !maker; ++it2)
          maker = const_cast<Document*>(srcDoc)->FindFeatureById(*it2);
        if (!maker)
          maker = const_cast<Document*>(srcDoc)->FindFeatureById(ref.featureId);
        if (maker)
          e["source_feature"] = {{"type", maker->TypeName()},
                                 {"name", maker->GetName()}};
      }
      fillGeometry(e);
      seen.emplace(key, edges.size());
      edges.push_back(std::move(e));
    }
    return json{{"geometry", edges}};
  }
  if (m == "add_drawing_dim") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const uint64_t viewId = p.at("view").get<uint64_t>();
    if (!state.drawing->FindView(viewId))
      throw std::runtime_error("no such view");
    DrawingDim dim;
    dim.viewId = viewId;
    if (!DrawingDimKindFromString(p.value("kind", std::string("distance")),
                                  dim.kind))
      throw std::runtime_error("unknown dimension kind '" +
                               p.value("kind", std::string()) + "'");
    // refs: [{edge|sil_face: {feature_id, element}, end?, occurrence?}, ...]
    for (const json& rj : p.at("refs")) {
      ProjRef r;
      r.viewId = viewId;
      r.end = rj.value("end", 0);
      r.occurrenceId = rj.value("occurrence", uint64_t{0});
      if (rj.contains("edge"))
        r.edge = ElementRef{rj.at("edge").value("feature_id", uint64_t{0}),
                            rj.at("edge").value("element", std::string{})};
      if (rj.contains("sil_face"))
        r.silFace =
            ElementRef{rj.at("sil_face").value("feature_id", uint64_t{0}),
                       rj.at("sil_face").value("element", std::string{})};
      dim.refs.push_back(r);
    }
    if (p.contains("offset")) {
      const json& o = p.at("offset");
      // A bare number here died as a raw nlohmann type_error ("cannot use
      // at() with number") — the first shape an agent guessed.
      if (!o.is_array() || o.size() != 2 || !o.at(0).is_number() ||
          !o.at(1).is_number())
        throw std::runtime_error(
            "offset is [x, y] in sheet mm (the label's offset from the "
            "dimension's anchor); omit it to auto-place clear of the view");
      dim.labelOffset = {o.at(0).get<double>(), o.at(1).get<double>()};
    } else {
      // No offset given: place the dimension clear of the views, the title
      // block AND the labels already on the sheet (R12 — every below-the-
      // view dim used to land on the same line, and the pile ran into the
      // title block).
      dim.labelOffset = ui::AutoDimOffset(state, dim);
    }
    ui::SnapshotDrawing(state);
    const uint64_t id = state.drawing->AddDim(dim);
    // Echo the resolved value now (the view is already projected).
    const drawing::ResolvedDim rd =
        ui::ResolveDrawingDim(state, *state.drawing->FindDim(id));
    return json{{"id", id},
                {"resolved", rd.resolved},
                {"value", rd.value},
                {"text", rd.text},
                {"offset", {dim.labelOffset.x, dim.labelOffset.y}}};
  }
  if (m == "remove_drawing_dim") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::SnapshotDrawing(state);
    if (!state.drawing->RemoveDim(p.at("id").get<uint64_t>())) {
      state.drawingHistory.undo.pop_back();
      throw std::runtime_error("no such dimension");
    }
    return json::object();
  }
  if (m == "list_drawing_dims") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    json arr = json::array();
    for (const DrawingDim& dim : state.drawing->GetDims()) {
      const drawing::ResolvedDim rd = ui::ResolveDrawingDim(state, dim);
      arr.push_back({{"id", dim.id},
                     {"kind", DrawingDimKindToString(dim.kind)},
                     {"view", dim.viewId},
                     {"resolved", rd.resolved},
                     {"value", rd.value},
                     {"text", rd.text}});
    }
    return json{{"dims", arr}};
  }
  return std::nullopt;
}

} // namespace remote
