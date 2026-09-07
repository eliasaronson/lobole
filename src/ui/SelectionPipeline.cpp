#include "ui/SelectionPipeline.h"
#include "ui/AssemblyActions.h"
#include "ui/StudyPanel.h"

#include "cad/FaceSelector.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include "ui/AppActionsInternal.h" // PartDocById, DisplayMatrix (assembly)
#include "ui/DocumentSlots.h"
#include "ui/FeatureEditor.h" // SlotPickClicked (U3)
#include "ui/JobActions.h"    // JobPlacedBodies (a job tab's bodies)
#include "ui/MeasureActions.h"
#include "ui/SketchInteraction.h" // FaceElementRef
#include "ui/SketchOverlay.h"     // EdgeElementRef, AppendPrimLine

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
  // Whatever the viewport is showing: a job tab draws the job's parts, not a
  // document's bodies (v73/J6), and it is the only place a body carries a
  // jobPartId -- which is what tells two parts sharing a feature id apart.
  const std::vector<Body>& bodies =
      ui::ViewportBodies(const_cast<AppState&>(state));
  for (const DocumentRenderMesh& m : state.documentMeshes)
    if (m.meshId == meshId && m.bodyIndex < bodies.size())
      return &bodies[m.bodyIndex];
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
  bool snapCorner = false;
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
    if (PickAssemblyFace(state, ray, pick) &&
        // MEASURE arms Connector only to reach its CORNER snap — a point no
        // face or edge can express. Every other snap already IS the face or
        // edge behind it, so offering one here would put a candidate the
        // measure tool cannot use at the front of the list and silently eat
        // the click. The mate tool wants them all.
        (!state.measure.active ||
         pick.snapType == AssemblyFacePick::SnapType::Corner)) {
      Candidate c;
      c.item.kind = SelectionItem::Kind::Connector;
      c.item.id = pick.occurrenceId;
      c.item.element = pick.face;
      c.item.snapElement = pick.snapEdge;
      c.item.snapElement2 = pick.snapEdge2;
      c.item.innerPath = pick.innerPath;
      c.snapMidpoint =
          pick.snapType == AssemblyFacePick::SnapType::EdgeMidpoint;
      c.snapCorner = pick.snapType == AssemblyFacePick::SnapType::Corner;
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
          SelectionItem item{SelectionItem::Kind::Face, 0, *ref};
          // Which placed part was actually under the cursor. Without it a CAM
          // target resolves by searching for the first body that answers to
          // the same element name (v76).
          if (const Body* body = BodyOfMesh(state, face.meshId))
            item.jobPartId = body->jobPartId;
          out.push_back({std::move(item), face.meshId, face.faceIndex});
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
  // part tabs) — part of the measured element's identity. A Body carries its
  // createdBy there instead, and an Occurrence its own id.
  MeasureState::Element e;
  switch (c.item.kind) {
  case SelectionItem::Kind::Edge:
    e.kind = MeasureState::Element::Kind::Edge;
    e.ref = c.item.element;
    e.occurrenceId = c.item.id;
    e.jobPartId = c.item.jobPartId;
    break;
  case SelectionItem::Kind::Body:
    e.kind = MeasureState::Element::Kind::Body;
    e.ref = ElementRef{c.item.id, ""};
    e.jobPartId = c.item.jobPartId;
    break;
  case SelectionItem::Kind::Occurrence:
    e.kind = MeasureState::Element::Kind::Occurrence;
    e.occurrenceId = c.item.id;
    break;
  case SelectionItem::Kind::Connector:
    // Only a CORNER snap is a measurable point; the other snaps are already
    // reachable as the face or edge they belong to.
    if (!c.item.snapElement2.IsSet())
      return;
    e = MeasureState::Element::OfCorner(c.item.snapElement, c.item.snapElement2,
                                        c.item.id);
    break;
  default:
    e.kind = MeasureState::Element::Kind::Face;
    e.ref = c.item.element;
    e.occurrenceId = c.item.id;
    e.jobPartId = c.item.jobPartId;
    break;
  }
  MeasureElementClicked(state, e);
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
    // Whole shapes measure too (M5): a BODY on a part tab, an OCCURRENCE on
    // an assembly. Measure::ContactBetween has answered "how far apart are
    // these two parts" since I4 and this list was the only thing refusing
    // the pick.
    // Connector rides along on assemblies purely to reach its CORNER snap:
    // a corner is the one measurable that is a POINT, and the face picker
    // already ranks corners against hole centres and midpoints. Candidates()
    // drops the non-corner snaps for measure, so this cannot shadow a face.
    const uint32_t kinds =
        KindBit(SelectionItem::Kind::Face) |
        KindBit(SelectionItem::Kind::Edge) |
        (state.assembly ? KindBit(SelectionItem::Kind::Occurrence) |
                              KindBit(SelectionItem::Kind::Connector)
                        : KindBit(SelectionItem::Kind::Body));
    return ArmedContext{kinds,
                        "Measure: pick faces, edges or whole parts (a second "
                        "for distance) - Esc exits",
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
  // The measure tool's hover measurable (M3), cleared on every path that
  // clears a hover — including the off-viewport early return, which is how a
  // preview gets stranded on screen after the cursor leaves.
  state.measure.hover.has = false;
  // The connector-hover mirror (triad + snap marker overlay reads it).
  state.mateTool.hasHover = false;
  state.mateTool.hover = Connector{};
  state.mateTool.hoverSnapIsMidpoint = false;
  state.mateTool.hoverSnapIsCorner = false;
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
  // A JOB tab has neither a document nor an assembly and is full of pickable
  // geometry — its parts, laid out (v73/J6). Leaving it out here meant a
  // click on a job selected nothing and said nothing about it, which is how
  // a CAM pick could only ever be made through the panel.
  if (!state.faceSelector || !state.meshRenderer || !state.camera ||
      (!state.document && !state.assembly && !state.job)) {
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
    // While the measure tool is armed, the top candidate is also a
    // MEASURABLE: remember it whole (ref + kind + occurrence) so the hover
    // measurement can be taken against it without re-deriving any of that.
    if (state.measure.active && (top.item.kind == SelectionItem::Kind::Face ||
                                 top.item.kind == SelectionItem::Kind::Edge)) {
      MeasureState::Element he;
      bool measurable = true;
      switch (top.item.kind) {
      case SelectionItem::Kind::Face:
        he = MeasureState::Element::OfFace(top.item.element, top.item.id);
        break;
      case SelectionItem::Kind::Edge:
        he = MeasureState::Element::OfEdge(top.item.element, top.item.id);
        break;
      case SelectionItem::Kind::Body:
        he = MeasureState::Element::OfBody(top.item.id);
        break;
      case SelectionItem::Kind::Occurrence:
        he = MeasureState::Element::OfOccurrence(top.item.id);
        break;
      case SelectionItem::Kind::Connector:
        if (!top.item.snapElement2.IsSet()) {
          measurable = false; // only a corner snap is a measurable point
          break;
        }
        he = MeasureState::Element::OfCorner(
            top.item.snapElement, top.item.snapElement2, top.item.id);
        break;
      default:
        measurable = false;
        break;
      }
      if (measurable) {
        state.measure.hover.has = true;
        state.measure.hover.element = he;
      }
    }
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
      state.mateTool.hover.anchor.snapEdge2 = top.item.snapElement2;
      // A face inside a placed unit: the connector records the way down to
      // it, so the mate anchors on that face while still positioning the
      // unit (v57).
      state.mateTool.hover.innerPath = top.item.innerPath;
      state.mateTool.hoverSnapIsMidpoint = top.snapMidpoint;
      state.mateTool.hoverSnapIsCorner = top.snapCorner;
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
        // A whole OCCURRENCE lights every mesh that belongs to it, the way
        // selecting it would — there is no single face to shade.
        if (e.kind == MeasureState::Element::Kind::Occurrence) {
          for (const DocumentRenderMesh& m : state.documentMeshes)
            if (m.occurrenceId == e.occurrenceId)
              state.meshRenderer->HighlightInstance(m.meshId);
          continue;
        }
        if (e.IsEdge())
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
      if (!e.IsEdge())
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
      if (!e.IsEdge())
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

// The calibrate-image picks, drawn on the sketch plane.
//
// Without this the tool asks you to "click two points a known distance apart"
// and then shows you nothing at all — you cannot see where the first click
// landed, whether it hit the tick you aimed at, or what span the typed
// distance is about to be assigned to. The numbers it produces are a SCALE
// for the whole image, so a mis-picked point silently rescales everything
// traced over it.
void AppendCalibrationMarkers(AppState& state, std::vector<PrimVertex>& out) {
  const SketchEditState& edit = state.sketchEdit;
  if (edit.tool != SketchTool::CalibrateImage || edit.calibImageId == 0)
    return;
  const SketchFeature* sk = ResolveActiveSketch(state);
  if (sk == nullptr)
    return;
  const glm::vec4 kPick{1.00f, 0.85f, 0.30f, 1.0f}; // the same hover yellow
  const glm::vec4 kSpan{0.35f, 0.80f, 1.00f, 1.0f}; // and selection blue
  if (edit.calibStage >= 1)
    AppendPrimDot(sk->plane.PlaneToWorld(edit.calibP1), 5.0f, kPick, out);
  if (edit.calibStage >= 2) {
    AppendPrimDot(sk->plane.PlaneToWorld(edit.calibP2), 5.0f, kPick, out);
    // The span the typed distance will be assigned to — the thing that is
    // actually being measured, and the only way to check it before Apply.
    AppendPrimLine(sk->plane.PlaneToWorld(edit.calibP1),
                   sk->plane.PlaneToWorld(edit.calibP2), 1.8f, kSpan, out);
  }
}

// ---------------------------------------------------------------------------
// What is selected, from AppActions.cpp
// (docs/architecture-review-2026-09-06.md, stage 3, third wave). This file
// already turned a click into a SelectionItem; these are the questions the rest
// of the app asks about the result, and the pruning that keeps it honest when
// the document changes underneath it.

// SyncAssemblyRenderers / SyncInContextGhosts / EvictPartRenderCache are
// defined in AssemblyActions.cpp (the assembly domain, extracted from this TU)
// and declared in AppActionsInternal.h, which this TU now includes for the
// render-cache types — so the hand-copied declarations that used to sit here
// are gone, and cannot drift from the real ones.

uint64_t SelectedFeatureId(const AppState& state) {
  const SelectionItem* it = state.selection.First(SelectionItem::Kind::Feature);
  return it != nullptr ? it->id : 0;
}

Feature* SelectedFeature(AppState& state, size_t* indexOut) {
  const uint64_t id = SelectedFeatureId(state);
  if (id == 0 || !state.document)
    return nullptr;
  return state.document->FindFeatureById(id, indexOut);
}

int SelectedFeatureIndexOrMinus1(AppState& state) {
  size_t index = 0;
  return SelectedFeature(state, &index) != nullptr ? static_cast<int>(index)
                                                   : -1;
}

void SelectFeature(AppState& state, uint64_t featureId) {
  state.selection.Replace({SelectionItem::Kind::Feature, featureId, {}});
}

void OpenFeatureEditor(AppState& state, uint64_t featureId) {
  if (!state.document || state.document->FindFeatureById(featureId) == nullptr)
    return;
  SelectFeature(state, featureId);
  state.editFeatureId = featureId; // the Tool window shows the editor session
}

void SelectFeatureByIndex(AppState& state, size_t index) {
  if (!state.document || index >= state.document->FeatureCount())
    return;
  SelectFeature(state, state.document->GetFeature(index)->GetId());
}

uint64_t SelectedOccurrenceId(const AppState& state) {
  const SelectionItem* it =
      state.selection.First(SelectionItem::Kind::Occurrence);
  return it != nullptr ? it->id : 0;
}

std::vector<uint64_t> SelectedOccurrenceIds(const AppState& state) {
  // The selection has always been a SET — the viewport has ctrl-clicked
  // occurrences into it since U1d — and SelectedOccurrenceId returning only
  // the FIRST is what made every consumer single-select. This is the same
  // question asked without that narrowing.
  std::vector<uint64_t> out;
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Occurrence && it.id != 0)
      out.push_back(it.id);
  return out;
}

uint64_t SelectedMateId(const AppState& state) {
  const SelectionItem* it = state.selection.First(SelectionItem::Kind::Mate);
  return it != nullptr ? it->id : 0;
}

namespace {
// Replace only the given kind's items (occurrence and mate selections are
// independent, as their pre-U1 fields were); id 0 just removes the kind.
void ReplaceKind(Selection& sel, SelectionItem::Kind kind, uint64_t id) {
  auto& items = sel.items;
  items.erase(
      std::remove_if(items.begin(), items.end(),
                     [&](const SelectionItem& it) { return it.kind == kind; }),
      items.end());
  if (id != 0)
    items.push_back({kind, id, {}});
}
} // namespace

void SelectMate(AppState& state, uint64_t mateId) {
  ReplaceKind(state.selection, SelectionItem::Kind::Mate, mateId);
}

void PruneSelection(AppState& state) {
  auto& items = state.selection.items;
  items.erase(
      std::remove_if(
          items.begin(), items.end(),
          [&](const SelectionItem& it) {
            switch (it.kind) {
            case SelectionItem::Kind::Feature:
              return !state.document ||
                     state.document->FindFeatureById(it.id) == nullptr;
            case SelectionItem::Kind::Occurrence:
              return !state.assembly ||
                     state.assembly->FindById(it.id) == nullptr;
            case SelectionItem::Kind::Mate:
              return !state.assembly ||
                     state.assembly->FindMateById(it.id) == nullptr;
            case SelectionItem::Kind::Operation: {
              if (!state.job)
                return true;
              for (const cam::Setup& s : state.job->GetSetups())
                for (const cam::Operation& op : s.operations)
                  if (op.id == it.id)
                    return false;
              return true;
            }
            default:
              return false; // geometry refs resolve lazily; stale = no-op
            }
          }),
      items.end());
}

} // namespace ui
