#include "remote/RemoteDrawings.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingValidate.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
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
     "Add a drawing document (sheet size, title block) to the project."},
    {"set_drawing_sheet",
     "Edit the active drawing's sheet {size?, landscape?, title_block?}."},
    {"list_drawings", "List drawing documents."},
    {"set_drawing_view", "Edit a view {id, position?, scale?, name?, "
                         "show_hidden?, base_view?}; folded axes note."},
    {"check_drawing",
     "Validate sheet layout: views vs border/title block, overlaps, dim "
     "placement, dangling/stale {update?}."},
    {"add_drawing_view", "Add an HLR view of a part/assembly {drawing, source, "
                         "orientation, scale, position}."},
    {"remove_drawing_view", "Remove a view by id."},
    {"list_drawing_views", "List views with placement and scale."},
    {"update_drawing", "Re-project all views from current geometry."},
    {"list_drawing_geometry",
     "Projected 2D curves of a view (for picking/dimensioning)."},
    {"add_drawing_dim", "Add a driven dimension between picked view geometry."},
    {"remove_drawing_dim", "Remove a dimension by id."},
    {"list_drawing_dims", "List dimensions with values."},
    {"export_drawing", "Export the drawing to SVG or PDF by extension {path}."},
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

// Apply a create_drawing/set_drawing_sheet `sheet` JSON block. The size
// string must already be validated (ParseSheetSizeOrThrow) so this never
// half-applies.
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
  json arr = json::array();
  for (const drawing::DrawingIssue& i : drawing::ValidateDrawing(
           *state.drawing, state.drawingRenders, dims, stale)) {
    json ij = {{"kind", i.kind}, {"detail", i.detail}};
    if (i.viewId)
      ij["view"] = i.viewId;
    if (i.otherViewId)
      ij["other_view"] = i.otherViewId;
    if (i.dimId)
      ij["dim"] = i.dimId;
    arr.push_back(std::move(ij));
  }
  return arr;
}

} // namespace

std::optional<json> HandleDrawingRpc(ui::AppState& state, const std::string& m,
                                     const RpcParams& p) {
  if (m == "create_drawing") {
    // Adds a drawing tab. Optional `sheet` seeds paper size / orientation /
    // title-block fields so a headless caller can set them in one call.
    const std::string name =
        p.value("name", "Drawing " + std::to_string(state.parts.size() + 1));
    // Validate BEFORE creating the tab — a bad size string must not leave a
    // half-configured drawing behind.
    if (p.contains("sheet") && p.at("sheet").contains("size"))
      ParseSheetSizeOrThrow(p.at("sheet").at("size").get<std::string>());
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
    v.scale = p.value("scale", 1.0);
    if (p.contains("position")) {
      const json& pos = p.at("position");
      v.position = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    } else { // default to the sheet centre
      v.position = ChromeOf(state.drawing->GetSheet()).extent * 0.5;
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
    // Report-only sheet-layout validation. update=true (default) re-projects
    // first so the verdict reflects current geometry (matching what
    // export_drawing would produce); update=false judges the cached renders
    // and reports staleness instead. Derived-cache write only — no snapshot.
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    if (p.value("update", true))
      ui::UpdateDrawingRenders(state);
    const json issues = DrawingIssuesJson(state);
    return json{{"ok", issues.empty()},
                {"issues", issues},
                {"views", state.drawing->ViewCount()},
                {"dims", state.drawing->DimCount()}};
  }
  if (m == "update_drawing") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::UpdateDrawingRenders(state); // the explicit re-projection
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
                 {"show_hidden", view.showHidden}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        vj["curves"] = it->second.curves.size();
        vj["has_geometry"] = it->second.hasGeometry;
        vj["stale"] = ui::DrawingSourceVersion(state, view.sourceDocId) !=
                      it->second.solvedSourceVersion;
        if (!it->second.error.empty())
          vj["error"] = it->second.error;
      } else {
        vj["curves"] = 0;
        vj["has_geometry"] = false;
        vj["stale"] = true; // never projected
      }
      arr.push_back(vj);
    }
    return json{{"views", arr}};
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
    // hidden segments); keep the first geometry seen.
    json edges = json::array();
    std::set<std::string> seen;
    for (const drawing::SheetCurve& c : it->second.curves) {
      const bool isEdge = c.edge.IsSet();
      const ElementRef& ref = isEdge ? c.edge : c.silFace;
      if (!ref.IsSet())
        continue;
      const std::string key = std::to_string(c.sourceId) + "|" +
                              (isEdge ? "e|" : "s|") +
                              std::to_string(ref.featureId) + "|" + ref.name;
      if (!seen.insert(key).second)
        continue;
      json e = {
          {"kind", isEdge ? "edge" : "silhouette"},
          {"source_id", c.sourceId},
          {"ref", {{"feature_id", ref.featureId}, {"element", ref.name}}}};
      if (c.conicRadius > 0.0) {
        e["type"] = "circle";
        e["center"] = {c.conicCenter.x, c.conicCenter.y};
        e["radius"] = c.conicRadius;
      } else if (c.points.size() >= 2) {
        e["type"] = c.curveType == 0 ? "line" : "curve";
        e["p1"] = {c.points.front().x, c.points.front().y};
        e["p2"] = {c.points.back().x, c.points.back().y};
      }
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
      dim.labelOffset = {o.at(0).get<double>(), o.at(1).get<double>()};
    } else {
      // No offset given: place the dimension clear of the view instead of on
      // top of the geometry (offset {0,0} was the old, useless default).
      const auto it = state.drawingRenders.find(viewId);
      dim.labelOffset = it != state.drawingRenders.end()
                            ? drawing::DefaultDimOffset(dim, it->second)
                            : glm::dvec2{0.0, -12.0};
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
