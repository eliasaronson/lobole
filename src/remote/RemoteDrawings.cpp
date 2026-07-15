#include "remote/RemoteDrawings.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/DrawingExport.h"
#include "core/Serialization.h"
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
std::optional<json> HandleDrawingRpc(ui::AppState& state,
                                     const std::string& m, const json& p) {
  if (m == "create_drawing") {
    // Adds a drawing tab. Optional `sheet` seeds paper size / orientation /
    // title-block fields so a headless caller can set them in one call.
    const std::string name =
        p.value("name", "Drawing " + std::to_string(state.parts.size() + 1));
    const size_t idx = ui::AddDrawing(state, name);
    if (p.contains("sheet") && state.drawing) {
      const json& sj = p.at("sheet");
      Sheet& sheet = state.drawing->GetSheet();
      SheetSize size;
      if (sj.contains("size") &&
          SheetSizeFromString(sj.at("size").get<std::string>(), size))
        sheet.size = size;
      sheet.landscape = sj.value("landscape", sheet.landscape);
      if (sj.contains("title_block")) {
        const json& tb = sj.at("title_block");
        sheet.titleBlock.project = tb.value("project", sheet.titleBlock.project);
        sheet.titleBlock.document = tb.value("document", sheet.titleBlock.document);
        sheet.titleBlock.author = tb.value("author", sheet.titleBlock.author);
        sheet.titleBlock.date = tb.value("date", sheet.titleBlock.date);
        sheet.titleBlock.notes = tb.value("notes", sheet.titleBlock.notes);
      }
      state.drawing->BumpVersion();
    }
    return json{{"index", idx},
            {"name", name},
            {"id", state.parts[idx].id},
            {"size", SheetSizeToString(state.drawing->GetSheet().size)}};
  }
  if (m == "list_drawings") {
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (state.parts[i].kind != ui::DocKind::Drawing) continue;
      // The active drawing lives in AppState; a stashed one in its slot.
      const DrawingDocument* dwg =
          static_cast<int>(i) == state.activePart
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
      throw std::runtime_error("no part or assembly named '" + sourceName + "'");
    DrawingView v;
    v.sourceDocId = sourceId;
    if (p.contains("orientation") &&
        !ViewOrientationFromString(p.at("orientation").get<std::string>(),
                                   v.orientation))
      throw std::runtime_error("unknown orientation '" +
                               p.at("orientation").get<std::string>() + "'");
    // Default the label to the orientation (front/top/iso…) — clearer on the
    // sheet than repeating the source name across every view.
    v.name = p.value("name", std::string(ViewOrientationToString(v.orientation)));
    v.baseViewId = p.value("base_view", uint64_t{0});
    v.scale = p.value("scale", 1.0);
    if (p.contains("position")) {
      const json& pos = p.at("position");
      v.position = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    } else {  // default to the sheet centre
      glm::dvec2 ext = SheetExtentMM(state.drawing->GetSheet().size);
      if (state.drawing->GetSheet().landscape) ext = {ext.y, ext.x};
      v.position = ext * 0.5;
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
      state.drawingHistory.undo.pop_back();  // nothing changed; drop the snapshot
      throw std::runtime_error("no such view");
    }
    ui::UpdateDrawingRenders(state);  // drop the removed view's geometry
    return json::object();
  }
  if (m == "update_drawing") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::UpdateDrawingRenders(state);  // the explicit re-projection
    json views = json::array();
    for (const DrawingView& view : state.drawing->GetViews()) {
      json vj = {{"id", view.id}, {"name", view.name}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        int vis = 0, hid = 0, outline = 0;
        for (const drawing::SheetCurve& c : it->second.curves) {
          if (c.visible) ++vis; else ++hid;
          if (c.kind == drawing::CurveKind::Outline) ++outline;
        }
        vj["curves"] = it->second.curves.size();
        vj["visible"] = vis;
        vj["hidden"] = hid;
        vj["silhouettes"] = outline;
        vj["has_geometry"] = it->second.hasGeometry;
        if (!it->second.error.empty()) vj["error"] = it->second.error;
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
        if (!it->second.error.empty()) vj["error"] = it->second.error;
      } else {
        vj["curves"] = 0;
        vj["has_geometry"] = false;
        vj["stale"] = true;  // never projected
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
    return json{{"path", path}, {"views", state.drawing->ViewCount()}};
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
      if (!ref.IsSet()) continue;
      const std::string key = std::to_string(c.sourceId) + "|" +
                              (isEdge ? "e|" : "s|") +
                              std::to_string(ref.featureId) + "|" + ref.name;
      if (!seen.insert(key).second) continue;
      json e = {{"kind", isEdge ? "edge" : "silhouette"},
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
        r.silFace = ElementRef{rj.at("sil_face").value("feature_id", uint64_t{0}),
                               rj.at("sil_face").value("element", std::string{})};
      dim.refs.push_back(r);
    }
    if (p.contains("offset")) {
      const json& o = p.at("offset");
      dim.labelOffset = {o.at(0).get<double>(), o.at(1).get<double>()};
    }
    ui::SnapshotDrawing(state);
    const uint64_t id = state.drawing->AddDim(dim);
    // Echo the resolved value now (the view is already projected).
    const drawing::ResolvedDim rd = ui::ResolveDrawingDim(state, *state.drawing->FindDim(id));
    return json{{"id", id},
            {"resolved", rd.resolved},
            {"value", rd.value},
            {"text", rd.text}};
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

}  // namespace remote
