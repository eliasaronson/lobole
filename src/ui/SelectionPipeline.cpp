#include "ui/SelectionPipeline.h"
#include "ui/AssemblyActions.h"
#include "ui/StudyPanel.h"

#include "cad/FaceSelector.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include "ui/AppActions.h"         // EdgeElementRef, AppendPrimLine
#include "ui/AppActionsInternal.h" // PartDocById, DisplayMatrix (assembly)
#include "ui/FeatureEditor.h"      // SlotPickClicked (U3)
#include "ui/SketchInteraction.h"  // FaceElementRef

#include "core/Occurrence.h" // AssemblyDocument, Occurrence
#include "core/features/ExtrudeFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include "imgui.h"

#include <algorithm>
#include <optional>

namespace ui {

namespace {

constexpr uint32_t KindBit(SelectionItem::Kind k) {
  return 1u << static_cast<int>(k);
}
constexpr uint32_t kDefaultKinds = KindBit(SelectionItem::Kind::Edge) |
                                   KindBit(SelectionItem::Kind::Face) |
                                   KindBit(SelectionItem::Kind::Body);

// Assembly tabs: a picked mesh face/edge → the part-local ElementRef plus
// the owning occurrence (the identity assembly measure needs). Null when the
// mesh isn't an occurrence's or the element has no stable name.
struct AssemblyElementHit {
  uint64_t occurrenceId = 0;
  ElementRef ref;
  std::vector<uint64_t> innerPath; // empty unless the hit is inside a unit
};
std::optional<AssemblyElementHit>
AssemblyElementAt(AppState& state, uint32_t meshId, uint32_t index, bool edge) {
  if (!state.assembly)
    return std::nullopt;
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.meshId != meshId)
      continue;
    if (m.occurrenceId == 0)
      return std::nullopt;
    const Occurrence* occ = state.assembly->FindById(m.occurrenceId);
    if (!occ)
      return std::nullopt;
    // A5: the face belongs to the LEAF part, which for a sub-assembly
    // occurrence is not occ->partId — that resolves to an assembly, has no
    // bodies, and used to make this return nullopt, so clicking anything
    // inside a placed unit produced no pick at all.
    Document* part =
        PartDocById(state, m.leafPartId ? m.leafPartId : occ->partId);
    if (!part || m.bodyIndex >= part->GetBodies().size())
      return std::nullopt;
    const Body& body = part->GetBodies()[m.bodyIndex];
    const std::string* name =
        edge ? body.elementMap.EdgeNameOf(static_cast<int>(index))
             : body.elementMap.NameOf(static_cast<int>(index));
    if (!name)
      return std::nullopt;
    return AssemblyElementHit{m.occurrenceId, ElementRef{body.createdBy, *name},
                              m.innerPath};
  }
  return std::nullopt;
}

// The body a render mesh mirrors, or null (assembly meshes, stale index).
const Body* BodyOfMesh(const AppState& state, uint32_t meshId) {
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.meshId == meshId && state.document &&
        m.bodyIndex < state.document->GetBodies().size()) {
      return &state.document->GetBodies()[m.bodyIndex];
    }
  }
  return nullptr;
}

// A ranked candidate: the stable item plus the render handles it resolved
// from (armed consumers like the blend remap anchor on those).
struct Candidate {
  SelectionItem item;
  uint32_t meshId = 0;
  uint32_t index = 0;
  // Connector candidates only: the snap was an edge MIDPOINT (vs a hole
  // center / plain face centroid) — drives the hover marker style.
  bool snapMidpoint = false;
};

// Candidates under the cursor, best first, restricted to `kinds`. Edges
// outrank the face they bound (PickEdge's angular tolerance only fires near
// an edge, and it already rejects occluded ones); the face's body comes last
// so same-spot cycling reaches it (edge → face → body).
std::optional<Candidate> RegionCandidate(AppState& state, const Ray& ray);

std::vector<Candidate> Candidates(AppState& state, const Ray& ray,
                                  uint32_t kinds) {
  std::vector<Candidate> out;
  if (kinds & KindBit(SelectionItem::Kind::SketchRegion)) {
    if (auto region = RegionCandidate(state, ray))
      out.push_back(*region);
  }
  if ((kinds & KindBit(SelectionItem::Kind::Connector)) && state.assembly) {
    // Mate connectors (U7c): the assembly face picker resolves the owning
    // occurrence + named face + hole-center/edge-midpoint snap in one go.
    AssemblyFacePick pick;
    if (PickAssemblyFace(state, ray, pick)) {
      Candidate c;
      c.item.kind = SelectionItem::Kind::Connector;
      c.item.id = pick.occurrenceId;
      c.item.element = pick.face;
      c.item.snapElement = pick.snapEdge;
      c.item.innerPath = pick.innerPath;
      c.snapMidpoint =
          pick.snapType == AssemblyFacePick::SnapType::EdgeMidpoint;
      out.push_back(c);
    }
  }
  if (kinds & KindBit(SelectionItem::Kind::Occurrence)) {
    // Assembly tab: a face hit selects the whole occurrence it belongs to.
    const RayHit face = state.faceSelector->PickFace(ray);
    if (face.hit) {
      for (const DocumentRenderMesh& m : state.documentMeshes) {
        if (m.meshId == face.meshId && m.occurrenceId != 0) {
          out.push_back({{SelectionItem::Kind::Occurrence, m.occurrenceId, {}},
                         face.meshId,
                         face.faceIndex});
          break;
        }
      }
    }
  }
  if (kinds & KindBit(SelectionItem::Kind::Edge)) {
    const EdgeHit edge = state.faceSelector->PickEdge(ray);
    if (edge.hit) {
      if (state.assembly) {
        // Assembly tab (measure): the item carries the occurrence in `id`.
        if (auto ae = AssemblyElementAt(state, edge.meshId, edge.edgeIndex,
                                        /*edge=*/true))
          out.push_back({{SelectionItem::Kind::Edge, ae->occurrenceId, ae->ref},
                         edge.meshId,
                         edge.edgeIndex});
      } else if (auto ref =
                     EdgeElementRef(state, edge.meshId, edge.edgeIndex)) {
        out.push_back({{SelectionItem::Kind::Edge, 0, *ref},
                       edge.meshId,
                       edge.edgeIndex});
      }
    }
  }
  if (kinds & (KindBit(SelectionItem::Kind::Face) |
               KindBit(SelectionItem::Kind::Body))) {
    const RayHit face = state.faceSelector->PickFace(ray);
    if (face.hit) {
      if (kinds & KindBit(SelectionItem::Kind::Face)) {
        if (state.assembly) {
          if (auto af = AssemblyElementAt(state, face.meshId, face.faceIndex,
                                          /*edge=*/false))
            out.push_back(
                {{SelectionItem::Kind::Face, af->occurrenceId, af->ref},
                 face.meshId,
                 face.faceIndex});
        } else if (auto ref =
                       FaceElementRef(state, face.meshId, face.faceIndex)) {
          out.push_back({{SelectionItem::Kind::Face, 0, *ref},
                         face.meshId,
                         face.faceIndex});
        }
      }
      if (kinds & KindBit(SelectionItem::Kind::Body)) {
        if (const Body* body = BodyOfMesh(state, face.meshId))
          out.push_back(
              {{SelectionItem::Kind::Body, body->createdBy, {}}, 0, 0});
      }
    }
  }
  return out;
}

// U2 region candidates: intersect the pick ray with the armed feature's
// sketch plane, and find the region whose material contains the hit point —
// even-odd: the point is in material when an odd number of loops contain it,
// and the region's OUTER loop is the innermost containing one. The region is
// carried in the item's element as {sketchId, "id,id,..."} (the outer loop's
// entity ids — the same key ExtrudeFeature::regions stores).
std::string JoinIds(const std::vector<uint32_t>& ids) {
  std::string s;
  for (uint32_t id : ids)
    s += (s.empty() ? "" : ",") + std::to_string(id);
  return s;
}

const SketchFeature* SlotFeatureSketch(AppState& state) {
  if (!state.document)
    return nullptr;
  Feature* f = state.document->FindFeatureById(state.slotPick.featureId);
  uint64_t sketchId = 0;
  if (auto* ext = dynamic_cast<ExtrudeFeature*>(f))
    sketchId = ext->sketchId;
  else if (auto* rev = dynamic_cast<RevolveFeature*>(f))
    sketchId = rev->sketchId;
  return dynamic_cast<const SketchFeature*>(
      state.document->FindFeatureById(sketchId));
}

std::optional<Candidate> RegionCandidate(AppState& state, const Ray& ray) {
  const SketchFeature* sk = SlotFeatureSketch(state);
  if (sk == nullptr)
    return std::nullopt;
  const glm::vec3 n = sk->plane.GetNormal();
  const float denom = glm::dot(ray.direction, n);
  if (std::abs(denom) < 1e-6f)
    return std::nullopt; // plane edge-on
  const float t = glm::dot(sk->plane.GetOrigin() - ray.origin, n) / denom;
  if (t <= 0.0f)
    return std::nullopt;
  const glm::vec2 uv = sk->plane.WorldToPlane(ray.origin + t * ray.direction);

  const std::vector<sketch::Loop> loops = sketch::BuildLoops(sk->entities);
  const sketch::Loop* innermost = nullptr;
  double innermostArea = 0.0;
  int containing = 0;
  for (const sketch::Loop& loop : loops) {
    if (!sketch::PointInLoop(loop, uv))
      continue;
    containing++;
    const double a = std::abs(loop.SignedArea());
    if (innermost == nullptr || a < innermostArea) {
      innermost = &loop;
      innermostArea = a;
    }
  }
  if (innermost == nullptr || containing % 2 == 0)
    return std::nullopt; // empty space or inside a hole
  Candidate c;
  c.item = {
      SelectionItem::Kind::SketchRegion, 0,
      ElementRef{sk->GetId(), JoinIds(sketch::LoopEntityIds(*innermost))}};
  return c;
}

// The armed pick context, derived fresh each frame from the tools' own state
// — never stored, so there is no second source of truth for "what's armed".
// Priority mirrors the old ViewportInput chain: dialog slots, measure, sim.
struct ArmedContext {
  uint32_t kinds = 0;
  const char* prompt = nullptr;
  void (*consume)(AppState&, const Candidate&) = nullptr;
};

void ConsumeSlotPick(AppState& state, const Candidate& c) {
  SlotPickClicked(state, c.item, c.meshId, c.index);
}
void ConsumeMeasure(AppState& state, const Candidate& c) {
  // On assemblies the Face/Edge item's id is the owning occurrence (0 on
  // part tabs) — part of the measured element's identity.
  MeasureElementClicked(state, c.item.element,
                        c.item.kind == SelectionItem::Kind::Edge, c.item.id);
}
void ConsumeSimFace(AppState& state, const Candidate& c) {
  SimulationApplyToFace(state, c.item.element);
}

std::optional<ArmedContext> ActiveArmedContext(AppState& state) {
  if (state.slotPick.Active()) {
    return ArmedContext{state.slotPick.kinds, state.slotPick.prompt.c_str(),
                        ConsumeSlotPick};
  }
  if (state.measure.active) {
    return ArmedContext{KindBit(SelectionItem::Kind::Face) |
                            KindBit(SelectionItem::Kind::Edge),
                        "Measure: pick a face or edge (a second for "
                        "distance) - Esc exits",
                        ConsumeMeasure};
  }
  if (state.simulation.IsActive() &&
      state.simulation.tool != SimulationState::Tool::None) {
    const char* prompt =
        state.simulation.tool == SimulationState::Tool::Keep
            ? "Pick a face to keep solid - Esc cancels"
            : "Pick a face for the boundary condition - Esc cancels";
    return ArmedContext{KindBit(SelectionItem::Kind::Face), prompt,
                        ConsumeSimFace};
  }
  return std::nullopt;
}

void ClearHoverPreviews(AppState& state) {
  state.pick.edgeHover = false;
  state.pick.regionHoverIds.clear();
  state.pick.hoverFace = {};
  // The connector-hover mirror (triad + snap marker overlay reads it).
  state.mateTool.hasHover = false;
  state.mateTool.hover = Connector{};
  state.mateTool.hoverSnapIsMidpoint = false;
  if (state.faceSelector)
    state.faceSelector->ClearHover();
  if (state.meshRenderer)
    state.meshRenderer->ClearFaceHover();
}

} // namespace

namespace {
// Shared resolve for Face/Edge items: part tabs match the active document's
// bodies; assembly items (id = occurrenceId) match that occurrence's meshes
// against its part's bodies.
bool ResolveElementItem(const AppState& state, const SelectionItem& item,
                        bool edge, uint32_t& meshId, uint32_t& index) {
  const std::vector<Body>* bodies = nullptr;
  if (item.id != 0 && state.assembly) {
    const Occurrence* occ = state.assembly->FindById(item.id);
    Document* part =
        occ ? PartDocById(const_cast<AppState&>(state), occ->partId) : nullptr;
    if (!part)
      return false;
    bodies = &part->GetBodies();
  } else if (state.document) {
    bodies = &state.document->GetBodies();
  } else {
    return false;
  }
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != item.id || m.bodyIndex >= bodies->size())
      continue;
    const Body& body = (*bodies)[m.bodyIndex];
    if (body.createdBy != item.element.featureId)
      continue;
    const auto idx = edge ? body.elementMap.ResolveEdge(item.element.name)
                          : body.elementMap.Resolve(item.element.name);
    if (idx) {
      meshId = m.meshId;
      index = static_cast<uint32_t>(*idx);
      return true;
    }
  }
  return false;
}
} // namespace

bool ResolveFaceItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& faceIndex) {
  if (item.kind != SelectionItem::Kind::Face)
    return false;
  return ResolveElementItem(state, item, /*edge=*/false, meshId, faceIndex);
}

bool ResolveEdgeItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& edgeIndex) {
  if (item.kind != SelectionItem::Kind::Edge)
    return false;
  return ResolveElementItem(state, item, /*edge=*/true, meshId, edgeIndex);
}

const char* ArmedPrompt(AppState& state) {
  const auto armed = ActiveArmedContext(state);
  return armed ? armed->prompt : nullptr;
}

void HandleSelectionInput(AppState& state, bool overViewport,
                          bool allowEscapeClear) {
  if (!state.faceSelector || !state.meshRenderer || !state.camera ||
      (!state.document && !state.assembly)) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const std::optional<ArmedContext> armed = ActiveArmedContext(state);

  // Escape clears the selection — only when nothing armed consumed it.
  if (!armed && allowEscapeClear && !io.WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Escape) && !state.selection.Empty()) {
    state.selection.Clear();
  }

  if (!overViewport || state.viewportDrag != ViewportDrag::None) {
    ClearHoverPreviews(state);
    return;
  }

  const Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  const uint32_t defaultKinds =
      state.assembly ? KindBit(SelectionItem::Kind::Occurrence) : kDefaultKinds;
  const std::vector<Candidate> candidates =
      Candidates(state, ray, armed ? armed->kinds : defaultKinds);

  // Hover preview: what a click would take (the top candidate). While a
  // blend collects edges, mirror the edge hover into its own state — the
  // blend overlay and the element-name label read it.
  ClearHoverPreviews(state);
  if (!candidates.empty()) {
    const Candidate& top = candidates.front();
    if (top.item.kind == SelectionItem::Kind::SketchRegion) {
      state.pick.regionHoverIds = ParseRegionIds(top.item.element.name);
    } else if (top.item.kind == SelectionItem::Kind::Edge) {
      state.pick.edgeHover = true;
      state.pick.edgeHoverMesh = top.meshId;
      state.pick.edgeHoverIndex = top.index;
    } else if (top.item.kind == SelectionItem::Kind::Connector) {
      // Mirror into the mate tool's hover so its triad/snap overlay draws.
      state.mateTool.hasHover = true;
      state.mateTool.hover.occurrenceId = top.item.id;
      state.mateTool.hover.anchor.face = top.item.element;
      state.mateTool.hover.anchor.snapEdge = top.item.snapElement;
      // A face inside a placed unit: the connector records the way down to
      // it, so the mate anchors on that face while still positioning the
      // unit (v57).
      state.mateTool.hover.innerPath = top.item.innerPath;
      state.mateTool.hoverSnapIsMidpoint = top.snapMidpoint;
    } else if (top.item.kind == SelectionItem::Kind::Face ||
               top.item.kind == SelectionItem::Kind::Occurrence) {
      // Occurrences hover via the hit face's range for now (a whole-instance
      // hover channel is renderer work, U5).
      state.pick.hoverFace = top.item.element;
      uint32_t first = 0, count = 0;
      state.faceSelector->SetHoveredFace(top.meshId, top.index);
      if (state.faceSelector->GetFaceVertexRange(top.meshId, top.index, first,
                                                 count))
        state.meshRenderer->SetFaceHover(top.meshId, first, count);
    }
  }

  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    return;

  // An armed tool consumes the pick; the Selection itself is untouched.
  if (armed) {
    if (!candidates.empty()) {
      state.slotPick.miss.clear();
      armed->consume(state, candidates.front());
    } else if (state.slotPick.Active()) {
      // Nothing the tool accepts was under the cursor. Saying so beats the
      // silence that made a missed pick indistinguishable from a broken tool.
      state.slotPick.miss =
          "Nothing there - " + (state.slotPick.prompt.empty()
                                    ? std::string("the tool wants something "
                                                  "else")
                                    : state.slotPick.prompt);
    }
    return;
  }

  const glm::vec2 pos(io.MousePos.x, io.MousePos.y);
  const bool samePos = glm::length(pos - state.pick.lastClickPos) < 6.0f;
  state.pick.lastClickPos = pos;

  if (candidates.empty()) {
    if (!io.KeyCtrl) {
      // Click empty space = deselect. On an assembly tab only the
      // occurrence kind clears (a selected mate stays, as before U1d).
      if (state.assembly)
        SelectOccurrence(state, 0);
      else
        state.selection.Clear();
    }
    state.pick.cycle = 0;
    return;
  }

  if (io.KeyCtrl) {
    state.selection.Toggle(candidates.front().item); // multi-select
    state.pick.cycle = 0;
  } else {
    // Same-spot repeat cycles the stacked candidates (edge → face → body,
    // face behind face …); a fresh spot takes the best one.
    state.pick.cycle =
        samePos ? (state.pick.cycle + 1) % static_cast<int>(candidates.size())
                : 0;
    const SelectionItem& chosen =
        candidates[static_cast<size_t>(state.pick.cycle)].item;
    if (chosen.kind == SelectionItem::Kind::Occurrence)
      SelectOccurrence(state, chosen.id); // kind-scoped: mate item survives
    else
      state.selection.Replace(chosen);
  }

  // Shift+click: also align the camera to the picked face (the pre-U1 Ctrl
  // gesture; Ctrl now means multi-select).
  if (io.KeyShift) {
    for (const Candidate& c : candidates) {
      if (c.item.kind == SelectionItem::Kind::Face) {
        state.camera->AlignToFace(
            state.faceSelector->GetFaceCenter(c.meshId, c.index),
            state.faceSelector->GetFaceNormal(c.meshId, c.index));
        break;
      }
    }
  }
}

void SyncSelectionHighlights(AppState& state) {
  if (!state.faceSelector || !state.meshRenderer)
    return;
  // Assembly tab: highlight every instance of each selected occurrence.
  // (ClearFaceHighlight resets instance highlights too, which is why this
  // must be part of the projection — an imperative HighlightInstance call
  // would be wiped the next frame.)
  if (state.assembly) {
    state.meshRenderer->ClearFaceHighlight();
    for (const SelectionItem& it : state.selection.items) {
      if (it.kind != SelectionItem::Kind::Occurrence)
        continue;
      for (const DocumentRenderMesh& m : state.documentMeshes)
        if (m.occurrenceId == it.id)
          state.meshRenderer->HighlightInstance(m.meshId);
    }
    // Measured faces highlight on their occurrence's instance while the
    // tool is armed (the element's id carries the occurrence).
    if (state.measure.active) {
      for (const MeasureState::Element& e : state.measure.picked) {
        if (e.isEdge)
          continue;
        uint32_t mesh = 0, index = 0, first = 0, count = 0;
        if (ResolveFaceItem(state,
                            {SelectionItem::Kind::Face, e.occurrenceId, e.ref},
                            mesh, index) &&
            state.faceSelector->GetFaceVertexRange(mesh, index, first, count))
          state.meshRenderer->SetFaceHighlight(mesh, first, count);
      }
    }
    return;
  }
  // Every Face item highlights — SetFaceHighlight is additive per instance
  // (multiple ranges draw as extra sub-draws), so multi-select shows fully
  // both across bodies and on one body (the U1e projection-completeness
  // item). The FaceSelector mirror carries the most recent for legacy
  // readers.
  state.meshRenderer->ClearFaceHighlight();
  state.faceSelector->ClearSelection();
  const auto highlightFace = [&](const SelectionItem& it) {
    uint32_t mesh = 0, index = 0, first = 0, count = 0;
    if (ResolveFaceItem(state, it, mesh, index) &&
        state.faceSelector->GetFaceVertexRange(mesh, index, first, count)) {
      state.faceSelector->SetSelectedFace(mesh, index);
      state.meshRenderer->SetFaceHighlight(mesh, first, count);
    }
  };
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Face)
      highlightFace(it);
  // Measured faces show with the same visuals while the tool is armed.
  if (state.measure.active)
    for (const MeasureState::Element& e : state.measure.picked)
      if (!e.isEdge)
        highlightFace({SelectionItem::Kind::Face, 0, e.ref});
}

void AppendSelectionPrimitives(AppState& state, std::vector<PrimVertex>& out) {
  if ((!state.document && !state.assembly) || ArmedBlendFeatureId(state) != 0 ||
      state.sketchEdit.IsEditing()) {
    return; // blend edge-pick / sketch mode own the overlay channel
  }
  const glm::vec4 kHover{1.00f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kSelected{0.35f, 0.80f, 1.00f, 1.0f};

  const auto drawEdge = [&](uint32_t meshId, uint32_t edgeIndex,
                            const glm::vec4& color) {
    for (const DocumentRenderMesh& m : state.documentMeshes) {
      if (m.meshId != meshId)
        continue;
      if (edgeIndex >= m.edgePolylines.size())
        return;
      const auto& poly = m.edgePolylines[edgeIndex];
      // Part meshes cache world-space polylines; occurrence meshes cache the
      // part's LOCAL ones — place those at the occurrence's display transform.
      glm::mat4 xf(1.0f);
      if (m.occurrenceId != 0 && state.assembly) {
        const Occurrence* occ = state.assembly->FindById(m.occurrenceId);
        if (!occ)
          return;
        xf = DisplayMatrix(state, *occ);
      }
      for (size_t i = 0; i + 1 < poly.size(); i++)
        AppendPrimLine(glm::vec3(xf * glm::vec4(poly[i], 1.0f)),
                       glm::vec3(xf * glm::vec4(poly[i + 1], 1.0f)), 2.6f,
                       color, out);
      return;
    }
  };

  for (const SelectionItem& it : state.selection.items) {
    uint32_t mesh = 0, index = 0;
    if (it.kind == SelectionItem::Kind::Edge &&
        ResolveEdgeItem(state, it, mesh, index)) {
      drawEdge(mesh, index, kSelected);
    }
  }
  // Measured edges show with the same emphasis while the tool is armed,
  // plus the nearest-distance line between two picked elements.
  if (state.measure.active) {
    for (const MeasureState::Element& e : state.measure.picked) {
      if (!e.isEdge)
        continue;
      uint32_t mesh = 0, index = 0;
      if (ResolveEdgeItem(state,
                          {SelectionItem::Kind::Edge, e.occurrenceId, e.ref},
                          mesh, index))
        drawEdge(mesh, index, kSelected);
    }
    // The dimension itself moved to AppendMeasureOverlay (M2): it needs the
    // on-top channel and it is a great deal more than a line.
  }
  if (state.pick.edgeHover)
    drawEdge(state.pick.edgeHoverMesh, state.pick.edgeHoverIndex, kHover);
}

std::vector<uint32_t> ParseRegionIds(const std::string& joined) {
  std::vector<uint32_t> ids;
  size_t pos = 0;
  while (pos < joined.size()) {
    const size_t comma = joined.find(',', pos);
    const std::string tok = joined.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!tok.empty())
      ids.push_back(static_cast<uint32_t>(std::stoul(tok)));
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return ids;
}

void AppendRegionPickPrimitives(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.slotPick.Active() ||
      !(state.slotPick.kinds & KindBit(SelectionItem::Kind::SketchRegion))) {
    return;
  }
  const SketchFeature* sk = SlotFeatureSketch(state);
  Feature* f = state.document
                   ? state.document->FindFeatureById(state.slotPick.featureId)
                   : nullptr;
  if (sk == nullptr || f == nullptr)
    return;
  const std::vector<std::vector<uint32_t>>* regions = nullptr;
  if (auto* ext = dynamic_cast<ExtrudeFeature*>(f))
    regions = &ext->regions;
  else if (auto* rev = dynamic_cast<RevolveFeature*>(f))
    regions = &rev->regions;
  if (regions == nullptr)
    return;

  const glm::vec4 kHover{1.00f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kSelected{0.35f, 0.80f, 1.00f, 1.0f};
  const auto drawLoop = [&](const sketch::Loop& loop, const glm::vec4& color) {
    const std::vector<glm::vec2> poly = sketch::LoopBoundaryPolygon(loop);
    for (size_t i = 0; i < poly.size(); i++) {
      const glm::vec3 a = sk->plane.PlaneToWorld(poly[i]);
      const glm::vec3 b = sk->plane.PlaneToWorld(poly[(i + 1) % poly.size()]);
      AppendPrimLine(a, b, 2.6f, color, out);
    }
  };
  for (const sketch::Loop& loop : sketch::BuildLoops(sk->entities)) {
    const std::vector<uint32_t> ids = sketch::LoopEntityIds(loop);
    const bool selected =
        std::find(regions->begin(), regions->end(), ids) != regions->end();
    const bool hovered =
        !state.pick.regionHoverIds.empty() && ids == state.pick.regionHoverIds;
    if (hovered)
      drawLoop(loop, kHover);
    else if (selected)
      drawLoop(loop, kSelected);
  }
}

} // namespace ui
