#include "remote/RemoteInspection.h"

#include "cad/CADKernel.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "render/ClipPlanes.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using nlohmann::json;

namespace remote {

namespace {

// Resolve an optional `part` RPC param (slot name, or stable PartSlot id as a
// number or numeric string) to that part's document WITHOUT switching the
// active tab — kills the set_active_part round-trips for cross-part queries.
Document* ResolvePartParam(ui::AppState& state, const json& part) {
  ui::EnsureParts(state);
  uint64_t id = 0;
  // A numeric `part` is tried as a STABLE id first (what occurrences
  // reference); if no part carries that id it falls back to the slot INDEX from
  // list_parts, so passing the visible 0..N index no longer silently returns
  // another part.
  std::optional<int> indexFallback;
  if (part.is_number_unsigned() || part.is_number_integer()) {
    id = part.get<uint64_t>();
    indexFallback = part.get<int>();
  } else if (part.is_string()) {
    const std::string s = part.get<std::string>();
    id = ui::PartIdByName(state, s);
    if (id == 0) { // allow a stable id / index passed as a string
      try {
        id = std::stoull(s);
        indexFallback = std::stoi(s);
      } catch (...) {
        id = 0;
      }
    }
  }
  Document* d = id != 0 ? ui::PartDocById(state, id) : nullptr;
  if (d == nullptr && indexFallback && *indexFallback >= 0 &&
      *indexFallback < static_cast<int>(state.parts.size())) {
    d = ui::PartDocById(state, state.parts[*indexFallback].id);
  }
  if (d == nullptr)
    throw std::runtime_error("no part with id or index " + part.dump() +
                             " — pass a part name, stable id or slot index "
                             "(see list_parts)");
  return d;
}

// The active part document, required by the part-tab-only inspection RPCs.
Document& RequirePartDoc(Document* doc, const char* what) {
  if (doc == nullptr)
    throw std::runtime_error(std::string(what) +
                             ": no active part document (on an assembly tab, "
                             "pass `part` or occurrence refs)");
  return *doc;
}

// V1 contact classification: a pair either overlaps (clash), sits within the
// touch tolerance (touching — a boltable joint), or is clear by `distance`.
const char* ContactState(const Measure::SetContact& c, double touchTol) {
  if (c.volume > 1e-9)
    return "clash";
  return c.distance <= touchTol ? "touching" : "clear";
}

// Round-8 retro: connected components of a part's bodies (largest first) —
// the compact core of contact_report's body scope, reused by
// check_geometry's automatic floating-bodies warning so the class is
// opt-OUT instead of opt-in.
std::vector<std::vector<std::string>>
BodyComponentNames(const Document& doc, double touchTol, double window) {
  struct Ent {
    const Body* body;
    std::vector<Body> single;
    glm::vec3 min, max;
  };
  std::vector<Ent> ents;
  for (const Body& b : doc.GetBodies()) {
    Ent e{&b, {}, {}, {}};
    e.single.push_back(b);
    if (Measure::WorldBounds(e.single, glm::mat4(1.0f), e.min, e.max))
      ents.push_back(std::move(e));
  }
  std::vector<size_t> parent(ents.size());
  for (size_t i = 0; i < parent.size(); ++i)
    parent[i] = i;
  const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
    return parent[i] == i ? i : parent[i] = find(parent[i]);
  };
  for (size_t i = 0; i < ents.size(); ++i)
    for (size_t j = i + 1; j < ents.size(); ++j) {
      const glm::vec3 gap = glm::max(
          glm::max(ents[i].min - ents[j].max, ents[j].min - ents[i].max),
          glm::vec3(0.0f));
      if (glm::length(gap) > window)
        continue;
      const auto c = Measure::ContactBetween(ents[i].single, glm::mat4(1.0f),
                                             ents[j].single, glm::mat4(1.0f));
      if (c && (c->volume > 1e-9 || c->distance <= touchTol))
        parent[find(i)] = find(j);
    }
  std::map<size_t, std::vector<std::string>> comps;
  for (size_t i = 0; i < ents.size(); ++i)
    comps[find(i)].push_back(ents[i].body->name);
  std::vector<std::vector<std::string>> out;
  for (auto& [root, names] : comps)
    out.push_back(std::move(names));
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  return out;
}

} // namespace

// Inspection RPCs (section clip, interference, mass properties, measure,
// element listing) carved out of RemoteServer::HandleSemantic. `docIn` is
// null on an assembly/drawing tab.
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc InspectionMethods[] = {
    {"check_geometry",
     "Did the model build? {ok, valid, body_count, "
     "feature_errors, bodies:[...B-Rep checks]}.",
     "body"},
    {"mass_properties",
     "Mass/volume/center-of-mass per body and total (density from materials).",
     "body"},
    // This row said "point/distance/angle/radius" for a year and the handler
    // implemented distance. `describe` is what an agent trusts INSTEAD of
    // guessing, so a row that promises a branch nobody wrote is the
    // "refuse, don't substitute" rule inverted — we refuse the call and
    // advertise the capability. It now names exactly what runs.
    {"measure",
     "Geometry queries over stable element names. Each of {a} and {b} names "
     "one thing: {feature_id, element} for a face or edge, {feature_id} alone "
     "for a whole BODY, or {occurrence: id|name|\"Unit/Part\"} for a whole "
     "placed part — and each carries its own {part} or {occurrence}, so a "
     "measurement can span two parts or two occurrences. {a}: that thing's "
     "own measure (face area / edge length + shape kind + radius/diameter; a "
     "body reports volume, surface area and bounding size). {a,b}: distance, "
     "closest points and a SIGNED delta, plus overlap_volume/clash for two "
     "whole shapes. distance_kind: minimum (default) | maximum (clearance) | "
     "centres. kind:'angle' or 'both' adds the angle, taken between the "
     "outward normals when both are planar faces (adjacent box faces read 90, "
     "opposite ones 180) and unsigned in 0-90 when either direction's sign is "
     "arbitrary, as a line edge's is; `basis` says which.",
     "a, b, distance_kind, kind, part"},
    {"list_elements",
     "Stable face/edge names per body (the toponaming handles "
     "measure/fillet/mates use).",
     "body, part"},
    {"interference",
     "Pairwise check between bodies or occurrences: {clash, volume, distance, "
     "state: clash|touching|clear}. touch_tol (mm, default 0.001) splits "
     "touching from clear; the no-pair assembly sweep stays clash-only — use "
     "contact_report for the full picture. A part INSIDE a sub-assembly is "
     "addressed by path — \"Gangway/Board 19\", or the id chain \"2/17\" "
     "when a name repeats — which is the only way to ask a question that "
     "spans the boundary (read-only; a sub-assembly is edited on its tab).",
     "a, b, touch_tol"},
    {"contact_report",
     "Contact graph: connected components (len 1 = one connected "
     "structure), per-pair state/distance, near_misses (gap <= near_tol, "
     "default 1.0 mm), clashes. Assembly tab: over occurrences, plus "
     "intended-vs-realized mate diffs when mates exist. Part tab: over the "
     "part's BODIES — catches a bracket floating inside one part, which "
     "the occurrence-level sweep cannot see.",
     "near_tol, touch_tol"},
    {"query_geometry",
     "Declarative element query with a built-in assertion: filter faces/"
     "edges by geometry_type, normal/axis (+angle_tol_deg), radius "
     "(+radius_tol), area/length bounds, near_point (+max_distance); "
     "expected_count makes the result carry ok true/false. Covers UNNAMED "
     "elements too; matches carry stable names where they exist.",
     "angle_tol_deg, axis, body, element_type, expected_count, geometry_type, "
     "max_area, max_distance, max_length, max_results, min_area, min_length, "
     "near_point, normal, part, radius, radius_tol"},
    {"compare_geometry",
     "Boolean-diff similarity: {golden_path} (STEP/BREP reference) vs the "
     "active doc, or {a, b} bodies/occurrences. Returns iou/dice/precision/"
     "recall, volume_ratio, center_offset, bbox_iou, topology counts and "
     "plain-language verdicts; {missing_path}/{extra_path} export the diff "
     "solids as BREP. materialize:true DRAWS them on the part instead — "
     "missing red, extra green — so one screenshot shows WHERE it differs "
     "(part tab only; clear_geometry_delta turns it off).",
     "a, b, extra_path, golden_path, materialize, missing_path"},
    {"clear_geometry_delta",
     "Remove the compare_geometry materialize overlay, leaving the plain "
     "model. Harmless when there is none.",
     ""},
    {"section",
     "Cross-section area where a plane cuts the bodies; on an assembly tab, "
     "every visible occurrence in place. Omit `offset` to cut through the "
     "middle of what is shown (the chosen offset comes back).",
     "base_plane, normal, offset, origin"},
    {"set_section",
     "Enable/disable the live section clip {base_plane|origin+normal, "
     "offset}. Works on part AND assembly tabs, with filled cut caps on "
     "both; omit `offset` to cut through the middle of what is shown. "
     "Shorthand for one cut — add_section/update_section address several.",
     "add, base_plane, enabled, offset, reverse"},
    {"add_section",
     "Add a section cut and return its id {base_plane: xy|xz|yz, plane_id? "
     "(a datum plane), offset?, tilt_u?, tilt_v? (degrees), reverse?}. Up to "
     "four; they persist with the project.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"list_sections",
     "Every section cut: id, source (base plane / datum / face, with "
     "resolved:false when a named datum/face is gone — such a cut stops "
     "cutting), offset, tilt, reverse, visible, and the cut plane "
     "{normal, d}.",
     ""},
    {"update_section",
     "Change one cut {id, base_plane?, plane_id?, offset?, tilt_u?, tilt_v?, "
     "reverse?, visible?}. Only the fields present are touched.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"remove_section", "Remove one cut {id}, or all of them with {all: true}.",
     "all, id"},
};
std::span<const MethodDoc> InspectionMethodDocs() { return InspectionMethods; }

std::optional<json> HandleInspectionRpc(ui::AppState& state, Document* docIn,
                                        const std::string& m,
                                        const RpcParams& p) {
  Document* doc = docIn;
  if (m == "set_section") {
    // The viewport section cut. params: {enabled, base_plane: "xy"|"xz"|"yz",
    // offset?, reverse?} or {enabled, origin, normal}. Disabled clears every
    // section. This edits the SAME list the Section tool shows, so a script
    // and the tool window are never two different truths.
    if (!p.value("enabled", true)) {
      ui::ClearSections(state);
      return json::object();
    }
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
      // One section unless asked otherwise: scripts that toggle a cut on and
      // off should not stack four of them up.
      const bool add = p.value("add", false);
      if (!add)
        ui::ClearSections(state);
      const uint64_t id = ui::AddSection(state, basePlane);
      if (id == 0)
        throw std::runtime_error(
            "at most " + std::to_string(kMaxClipPlanes) +
            " sections at once; remove one first (set_section enabled=false "
            "clears them)");
      ui::AppState::SectionView* sec = ui::FindSection(state, id);
      // No offset given: AddSection already centered it on what the tab
      // shows, which is the only offset guaranteed to cut something.
      if (p.contains("offset"))
        sec->offset = p.at("offset").get<float>();
      sec->reverse = p.value("reverse", false);
      return json{
          {"id", id}, {"offset", sec->offset}, {"reverse", sec->reverse}};
    }
    throw std::runtime_error(
        "set_section wants base_plane (\"xy\"|\"xz\"|\"yz\") with an "
        "optional offset/reverse, or enabled=false to clear");
  }
  if (m == "add_section" || m == "update_section") {
    const bool adding = m == "add_section";
    ui::AppState::SectionView* sec = nullptr;
    if (adding) {
      const std::string bp = p.value("base_plane", std::string("xy"));
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
      const uint64_t id = ui::AddSection(state, basePlane);
      if (id == 0)
        throw std::runtime_error("at most " + std::to_string(kMaxClipPlanes) +
                                 " sections at once; remove one first");
      sec = ui::FindSection(state, id);
    } else {
      sec = ui::FindSection(state, p.at("id").get<uint64_t>());
      if (sec == nullptr)
        throw std::runtime_error("no section with that id; list_sections "
                                 "shows the ones that exist");
      if (p.contains("base_plane")) {
        const std::string bp = p.at("base_plane").get<std::string>();
        sec->basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
        sec->planeId = 0; // a base plane and a datum are alternatives
        sec->face = ElementRef{};
      }
    }
    if (p.contains("plane_id")) {
      // A datum plane in the ACTIVE PART; the cut then follows it when the
      // datum moves, which is the only reason to name one.
      sec->planeId = p.at("plane_id").get<uint64_t>();
      sec->face = ElementRef{};
      if (sec->planeId != 0 && doc != nullptr &&
          doc->FindFeatureById(sec->planeId) == nullptr)
        throw std::runtime_error("no feature with that id in this part");
    }
    if (p.contains("offset"))
      sec->offset = p.at("offset").get<float>();
    if (p.contains("tilt_u"))
      sec->tiltU = p.at("tilt_u").get<float>();
    if (p.contains("tilt_v"))
      sec->tiltV = p.at("tilt_v").get<float>();
    if (p.contains("reverse"))
      sec->reverse = p.at("reverse").get<bool>();
    if (p.contains("visible"))
      sec->visible = p.at("visible").get<bool>();
    const glm::vec4 plane = ui::SectionPlaneOf(state, *sec);
    return json{{"id", sec->id},
                {"offset", sec->offset},
                {"tilt_u", sec->tiltU},
                {"tilt_v", sec->tiltV},
                {"reverse", sec->reverse},
                {"visible", sec->visible},
                {"normal", {plane.x, plane.y, plane.z}},
                {"d", plane.w}};
  }
  if (m == "list_sections") {
    json out = json::array();
    for (const ui::AppState::SectionView& sec : state.sections) {
      const glm::vec4 plane = ui::SectionPlaneOf(state, sec);
      json sj = {{"id", sec.id},
                 {"offset", sec.offset},
                 {"tilt_u", sec.tiltU},
                 {"tilt_v", sec.tiltV},
                 {"reverse", sec.reverse},
                 {"visible", sec.visible},
                 {"normal", {plane.x, plane.y, plane.z}},
                 {"d", plane.w}};
      // The source, named the way the tool window names it, so a script and
      // the window describe the same cut in the same words.
      if (sec.face.IsSet())
        sj["source"] =
            json{{"kind", "face"},
                 {"feature_id", sec.face.featureId},
                 {"element", sec.face.name},
                 // False = the face is gone, or this is an
                 // assembly tab; the cut is off until it resolves.
                 {"resolved", ui::SectionSourceResolved(state, sec)}};
      else if (sec.planeId != 0)
        sj["source"] =
            json{{"kind", "datum"},
                 {"plane_id", sec.planeId},
                 {"resolved", ui::SectionSourceResolved(state, sec)}};
      else
        sj["source"] = json{{"kind", "base_plane"},
                            {"base_plane", sec.basePlane == 1   ? "xz"
                                           : sec.basePlane == 2 ? "yz"
                                                                : "xy"}};
      out.push_back(std::move(sj));
    }
    return json{{"sections", out}};
  }
  if (m == "remove_section") {
    if (p.value("all", false)) {
      ui::ClearSections(state);
      return json::object();
    }
    const uint64_t id = p.at("id").get<uint64_t>();
    if (ui::FindSection(state, id) == nullptr)
      throw std::runtime_error("no section with that id; list_sections shows "
                               "the ones that exist");
    ui::RemoveSection(state, id);
    return json::object();
  }
  if (m == "section") {
    // Cross-section area where a plane cuts through all bodies (I3). Plane:
    // {base_plane: "xy"|"xz"|"yz", offset?: 0} OR {origin: [x,y,z],
    // normal: [x,y,z]}.
    glm::vec3 origin(0.0f), normal(0.0f, 0.0f, 1.0f);
    double usedOffset = 0.0;
    bool derived = false;
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
      normal = basePlane == 1   ? glm::vec3(0, 1, 0)
               : basePlane == 2 ? glm::vec3(1, 0, 0)
                                : glm::vec3(0, 0, 1);
      // Omitting `offset` means the same here as in set_section: the middle
      // of what this tab shows. Two readings of one word would be worse than
      // either.
      derived = !p.contains("offset");
      usedOffset =
          derived
              ? static_cast<double>(ui::CenteredSectionOffset(state, basePlane))
              : p.at("offset").get<double>();
      origin = normal * static_cast<float>(usedOffset);
    } else {
      const auto& o = p.at("origin");
      const auto& nrm = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      normal = {nrm[0].get<float>(), nrm[1].get<float>(), nrm[2].get<float>()};
    }
    // On an ASSEMBLY tab the same question is about the assembled thing, so
    // every visible occurrence's bodies are placed by its solved transform
    // and the area is the sum over all of them. Refusing here (which it used
    // to do) makes "how much material does this cut cross?" unanswerable for
    // exactly the models where it matters most.
    std::vector<Body> placed;
    const std::vector<Body>* bodies = nullptr;
    if (doc != nullptr) {
      bodies = &doc->GetBodies();
    } else if (state.assembly) {
      // A5: the leaf parts of the whole tree, placed.
      for (const assembly::FlatInstance& inst :
           ui::FlattenActiveAssembly(state)) {
        if (!inst.visible)
          continue;
        const glm::mat4 xf = inst.world;
        for (const Body& b : inst.part->GetBodies()) {
          Body nb = b;
          nb.shape = b.shape.Transformed(xf);
          placed.push_back(std::move(nb));
        }
      }
      bodies = &placed;
    } else {
      RequirePartDoc(doc, "section"); // throws with the standard message
    }
    const auto area = Measure::CrossSectionArea(*bodies, origin, normal);
    if (!area)
      throw std::runtime_error(
          "section failed — the plane may cut no body at all; check the "
          "bodies' bbox_min/bbox_max in get_state against the plane");
    json out{{"area", *area}};
    if (derived)
      out["offset"] = usedOffset; // say where it chose to cut
    return out;
  }
  if (m == "interference") {
    // Clash detection (inspection I4). Part tab: two bodies interfere when
    // their intersection has volume — params: {a: index, b: index}. Active
    // ASSEMBLY tab: a/b are occurrence ids (numbers) or names (strings); each
    // occurrence's part bodies are placed by its solved occurrence transform
    // and the same check runs in world space. Headless-safe (pure geometry).
    if (doc == nullptr && state.assembly) {
      // A resolved side of the question: which occurrence, and its geometry
      // already placed in world space.
      struct Target {
        const Occurrence* occ = nullptr;
        std::vector<Body> bodies;
        std::string label;
      };
      const auto subtreeOf = [&](uint64_t rootId) {
        std::vector<Body> out;
        for (const assembly::FlatInstance& inst :
             ui::FlattenActiveAssembly(state)) {
          if (!inst.visible || inst.rootOccurrenceId != rootId)
            continue;
          for (const Body& b : inst.part->GetBodies()) {
            Body nb = b;
            nb.shape = b.shape.Transformed(inst.world);
            out.push_back(std::move(nb));
          }
        }
        return out;
      };
      auto targetOf = [&](const json& v) -> Target {
        Target t;
        if (v.is_number()) {
          t.occ = state.assembly->FindById(v.get<uint64_t>());
          if (t.occ == nullptr)
            throw std::runtime_error("occurrence id " + v.dump() +
                                     " not found (see list_occurrences)");
          t.label = t.occ->name;
          t.bodies = subtreeOf(t.occ->id);
          return t;
        }
        const std::string name = v.get<std::string>();
        for (const Occurrence& o : state.assembly->GetOccurrences())
          if (o.name == name) {
            t.occ = &o;
            t.label = o.name;
            t.bodies = subtreeOf(o.id); // a unit is its whole subtree
            return t;
          }
        // A5: a PATH into a sub-assembly ("Gangway/Board 19", or the id
        // chain "2/17"). Read-only, and the only way to ask a question that
        // spans the boundary — the board lives in the sub-assembly's
        // document and the hinge at the top, so no tab holds both.
        if (name.find('/') != std::string::npos) {
          auto hit = assembly::ResolvePath(*state.assembly, name,
                                           ui::PartResolverFor(state),
                                           ui::AssemblyResolverFor(state));
          if (!hit)
            throw std::runtime_error(hit.error());
          t.occ = hit->occ;
          t.label = name;
          // Its own geometry, at the COMPOSED transform the resolver
          // returned. Never re-derived from the id: ids are unique per
          // document, so an inner id collides with a top-level one.
          if (hit->occ->refKind == OccurrenceRef::Assembly) {
            if (const AssemblyDocument* sub =
                    ui::AssemblyDocById(state, hit->occ->partId)) {
              auto flat = assembly::Flatten(*sub, ui::PartResolverFor(state),
                                            ui::AssemblyResolverFor(state));
              if (flat)
                for (const assembly::FlatInstance& inst : *flat)
                  for (const Body& b : inst.part->GetBodies()) {
                    Body nb = b;
                    nb.shape = b.shape.Transformed(hit->world * inst.world);
                    t.bodies.push_back(std::move(nb));
                  }
            }
          } else if (const Document* pd =
                         ui::PartDocById(state, hit->occ->partId)) {
            for (const Body& b : pd->GetBodies()) {
              Body nb = b;
              nb.shape = b.shape.Transformed(hit->world);
              t.bodies.push_back(std::move(nb));
            }
          }
          return t;
        }
        throw std::runtime_error(
            "occurrence '" + name +
            "' not found (see list_occurrences; a part inside a "
            "sub-assembly is addressed by path, e.g. \"Gangway/Board 19\")");
      };
      const auto pairVolume = [&](const Target& ta,
                                  const Target& tb) -> double {
        if (ta.bodies.empty() || tb.bodies.empty())
          throw std::runtime_error(
              "'" + (ta.bodies.empty() ? ta.label : tb.label) +
              "' resolves to no geometry (missing, hidden or empty)");
        const auto v = Measure::InterferenceVolume(ta.bodies, glm::mat4(1.0f),
                                                   tb.bodies, glm::mat4(1.0f));
        if (!v)
          throw std::runtime_error(
              "interference check failed — an OpenCASCADE boolean/extrema "
              "threw, usually on an invalid or empty body; run "
              "check_geometry on both sides");
        return *v;
      };
      if (!p.contains("a") && !p.contains("b")) {
        // No pair named: sweep EVERY visible, unsuppressed pair — the
        // whole-assembly clash check a design review actually wants.
        std::vector<Target> occs;
        for (const Occurrence& o : state.assembly->GetOccurrences()) {
          if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
            continue;
          Target t;
          t.occ = &o;
          t.label = o.name;
          t.bodies = subtreeOf(o.id);
          if (!t.bodies.empty())
            occs.push_back(std::move(t));
        }
        json pairs = json::array();
        bool any = false;
        for (size_t i = 0; i < occs.size(); ++i)
          for (size_t j = i + 1; j < occs.size(); ++j) {
            const double v = pairVolume(occs[i], occs[j]);
            if (v > 1e-9)
              any = true;
            pairs.push_back({{"a", occs[i].label},
                             {"b", occs[j].label},
                             {"clash", v > 1e-9},
                             {"volume", v}});
          }
        return json{{"clash", any}, {"pairs", pairs}};
      }
      const Target ta = targetOf(p.at("a"));
      const Target tb = targetOf(p.at("b"));
      if (ta.occ == tb.occ)
        throw std::runtime_error("pick two different occurrences");
      const double touchTol = p.value("touch_tol", 1e-3);
      if (ta.bodies.empty() || tb.bodies.empty())
        throw std::runtime_error(
            "'" + (ta.bodies.empty() ? ta.label : tb.label) +
            "' resolves to no geometry (missing, hidden or empty)");
      const auto c = Measure::ContactBetween(ta.bodies, glm::mat4(1.0f),
                                             tb.bodies, glm::mat4(1.0f));
      if (!c)
        throw std::runtime_error(
            "interference check failed — an OpenCASCADE boolean/extrema "
            "threw, usually on an invalid or empty body; run "
            "check_geometry on both sides");
      return json{{"clash", c->volume > 1e-9},
                  {"volume", c->volume},
                  {"distance", c->distance},
                  {"state", ContactState(*c, touchTol)},
                  {"a", ta.label},
                  {"b", tb.label}};
    }
    const auto& bodies = RequirePartDoc(doc, "interference").GetBodies();
    const double touchTol = p.value("touch_tol", 1e-3);
    const int ia = p.at("a").get<int>();
    const int ib = p.at("b").get<int>();
    if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
        ib >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError(
          "body", ia < 0 || ia >= static_cast<int>(bodies.size()) ? ia : ib,
          bodies.size(), "get_state lists bodies"));
    std::vector<Body> va(1), vb(1);
    va[0].shape = bodies[ia].shape;
    vb[0].shape = bodies[ib].shape;
    const auto c =
        Measure::ContactBetween(va, glm::mat4(1.0f), vb, glm::mat4(1.0f));
    if (!c)
      throw std::runtime_error(
          "interference check failed — an OpenCASCADE boolean/extrema "
          "threw, usually on an invalid or empty body; run "
          "check_geometry on both sides");
    return json{{"clash", c->volume > 1e-9},
                {"volume", c->volume},
                {"distance", c->distance},
                {"state", ContactState(*c, touchTol)}};
  }
  if (m == "query_geometry") {
    // Declarative geometry predicates with an assertion built in (survey
    // round 2, the expected_count idea): "there are 4 cylinder faces of
    // radius 3±0.01 with axis ±Z" is ONE call returning ok true/false —
    // instead of a zoo of named finders or measure-probing every element.
    // params: {part?, body?, element_type?: face|edge, geometry_type?,
    // normal? + angle_tol_deg?, axis? + angle_tol_deg?, radius? +
    // radius_tol?, min_area?/max_area?, min_length?/max_length?,
    // near_point? + max_distance?, expected_count?, max_results? (50)}
    const auto& bodies =
        p.contains("part") ? ResolvePartParam(state, p.at("part"))->GetBodies()
                           : RequirePartDoc(doc, "query_geometry").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    const std::string elementType = p.value("element_type", "face");
    if (elementType != "face" && elementType != "edge")
      throw std::runtime_error("element_type must be face or edge");
    const bool wantFaces = elementType == "face";
    const std::string geomType = p.value("geometry_type", "");
    const double angleTol = glm::radians(p.value("angle_tol_deg", 5.0));
    const double radiusTol = p.value("radius_tol", 0.01);
    const size_t maxResults = p.value("max_results", 50u);
    const auto vec3 = [](const json& a) {
      return glm::vec3(a.at(0).get<float>(), a.at(1).get<float>(),
                       a.at(2).get<float>());
    };
    std::optional<glm::vec3> wantNormal, wantAxis, nearPoint;
    if (p.contains("normal"))
      wantNormal = glm::normalize(vec3(p.at("normal")));
    if (p.contains("axis"))
      wantAxis = glm::normalize(vec3(p.at("axis")));
    if (p.contains("near_point"))
      nearPoint = vec3(p.at("near_point"));
    const double maxDistance = p.value("max_distance", 1e30);
    std::optional<double> wantRadius, minArea, maxArea, minLength, maxLength;
    if (p.contains("radius"))
      wantRadius = p.at("radius").get<double>();
    if (p.contains("min_area"))
      minArea = p.at("min_area").get<double>();
    if (p.contains("max_area"))
      maxArea = p.at("max_area").get<double>();
    if (p.contains("min_length"))
      minLength = p.at("min_length").get<double>();
    if (p.contains("max_length"))
      maxLength = p.at("max_length").get<double>();

    size_t count = 0;
    json matches = json::array();
    constexpr int kElementCap = 20000; // runaway guard, not a real limit
    for (size_t bi = 0; bi < bodies.size(); bi++) {
      if (only >= 0 && bi != static_cast<size_t>(only))
        continue;
      // Stable names where toponaming assigned one; unnamed elements still
      // match (patterns aren't named yet) and are addressable by index.
      std::map<int, std::string> names;
      if (wantFaces)
        for (const auto& [idx, name] : bodies[bi].elementMap.names)
          names[idx] = name;
      else
        for (const auto& [idx, name] : bodies[bi].elementMap.edgeNames)
          names[idx] = name;
      for (int idx = 0; idx < kElementCap; idx++) {
        std::string kind;
        double area = 0.0, length = 0.0, radius = 0.0;
        bool hasRadius = false, hasAxis = false;
        glm::vec3 point(0.0f), normal(0.0f), axis(0.0f);
        if (wantFaces) {
          const auto fi = Measure::DescribeFaceAt(bodies[bi], idx);
          if (!fi)
            break;
          kind = fi->kind;
          area = fi->area;
          point = fi->centroid;
          normal = fi->normal;
          radius = fi->radius;
          hasRadius = fi->hasRadius;
          axis = fi->axis;
          hasAxis = fi->hasAxis;
        } else {
          const auto ei = Measure::DescribeEdgeAt(bodies[bi], idx);
          if (!ei)
            break;
          kind = ei->kind;
          length = ei->length;
          point = ei->midpoint;
          radius = ei->radius;
          hasRadius = ei->hasRadius;
          axis = ei->axis;
          hasAxis = ei->hasAxis;
        }
        if (!geomType.empty() && kind != geomType)
          continue;
        if (wantNormal && (glm::length(normal) < 0.5f ||
                           glm::dot(glm::normalize(normal), *wantNormal) <
                               std::cos(angleTol)))
          continue;
        // Axis sign is arbitrary (a cylinder axis has no outward), so |dot|.
        if (wantAxis &&
            (!hasAxis || std::abs(glm::dot(glm::normalize(axis), *wantAxis)) <
                             std::cos(angleTol)))
          continue;
        if (wantRadius &&
            (!hasRadius || std::abs(radius - *wantRadius) > radiusTol))
          continue;
        if (minArea && area < *minArea)
          continue;
        if (maxArea && area > *maxArea)
          continue;
        if (minLength && length < *minLength)
          continue;
        if (maxLength && length > *maxLength)
          continue;
        if (nearPoint && glm::length(point - *nearPoint) > maxDistance)
          continue;
        count++;
        if (matches.size() >= maxResults)
          continue; // keep counting, stop listing
        json entry = {
            {"body", bi},
            {"index", idx},
            {"kind", kind},
            {"created_by", bodies[bi].createdBy},
            {wantFaces ? "centroid" : "midpoint", {point.x, point.y, point.z}}};
        if (wantFaces) {
          entry["area"] = area;
          entry["normal"] = {normal.x, normal.y, normal.z};
        } else {
          entry["length"] = length;
        }
        if (hasRadius)
          entry["radius"] = radius;
        if (hasAxis)
          entry["axis"] = {axis.x, axis.y, axis.z};
        if (const auto it = names.find(idx); it != names.end())
          entry["element"] = it->second;
        matches.push_back(std::move(entry));
      }
    }
    json out = {{"count", count},
                {"matches", std::move(matches)},
                {"truncated", count > maxResults}};
    if (p.contains("expected_count")) {
      const size_t expected = p.at("expected_count").get<size_t>();
      out["expected_count"] = expected;
      out["ok"] = count == expected;
    }
    return out;
  }
  if (m == "compare_geometry") {
    // V4: boolean-diff similarity, the numeric "same shape?" oracle. The
    // REFERENCE (A) is the golden file when given, else body/occurrence `a`;
    // the CANDIDATE (B) is the active doc, else `b`. Metrics are arithmetic
    // over vol(A), vol(B), vol(A∩B); the verdict strings are what makes
    // them actionable for an agent.
    const auto bodiesOfDoc = [&]() -> std::vector<Body> {
      if (doc != nullptr)
        return doc->GetBodies();
      std::vector<Body> placed;
      if (state.assembly) {
        // A5: leaf parts of the whole tree, placed.
        for (const assembly::FlatInstance& inst :
             ui::FlattenActiveAssembly(state)) {
          if (!inst.visible)
            continue;
          for (const Body& b : inst.part->GetBodies()) {
            Body nb;
            nb.shape = b.shape.Transformed(inst.world);
            placed.push_back(std::move(nb));
          }
        }
      }
      return placed;
    };
    const auto loadGolden = [](const std::string& path) -> std::vector<Body> {
      std::vector<Body> out;
      const auto ext = std::filesystem::path(path).extension().string();
      if (ext == ".brep" || ext == ".brp") {
        CADShape s;
        if (!CADKernel::Instance().LoadBREP(path, s))
          throw std::runtime_error("cannot read BREP " + path);
        Body b;
        b.shape = s;
        out.push_back(std::move(b));
      } else {
        std::vector<CADShape> shapes;
        if (!CADKernel::Instance().LoadSTEP(path, shapes) || shapes.empty())
          throw std::runtime_error("cannot read STEP " + path);
        for (CADShape& s : shapes) {
          Body b;
          b.shape = s;
          out.push_back(std::move(b));
        }
      }
      return out;
    };
    std::vector<Body> refA, candB;
    if (p.contains("golden_path")) {
      refA = loadGolden(p.at("golden_path").get<std::string>());
      candB = bodiesOfDoc();
    } else if (doc != nullptr) {
      const auto& bodies = doc->GetBodies();
      const int ia = p.at("a").get<int>(), ib = p.at("b").get<int>();
      if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
          ib >= static_cast<int>(bodies.size()))
        throw std::runtime_error(IndexError(
            "body", ia < 0 || ia >= static_cast<int>(bodies.size()) ? ia : ib,
            bodies.size(), "get_state lists bodies"));
      refA.emplace_back().shape = bodies[ia].shape;
      candB.emplace_back().shape = bodies[ib].shape;
    } else if (state.assembly) {
      const auto occBodies = [&](const json& v) {
        const Occurrence* found = nullptr;
        if (v.is_string()) {
          for (const Occurrence& o : state.assembly->GetOccurrences())
            if (o.name == v.get<std::string>())
              found = &o;
        } else {
          found = state.assembly->FindById(v.get<uint64_t>());
        }
        if (found == nullptr)
          throw std::runtime_error("occurrence " + v.dump() + " not found");
        Document* pd = ui::PartDocById(state, found->partId);
        if (pd == nullptr)
          throw std::runtime_error("occurrence part document is missing");
        std::vector<Body> placed;
        const glm::mat4 xf = ui::OccurrenceMatrix(*found);
        for (const Body& b : pd->GetBodies()) {
          Body nb;
          nb.shape = b.shape.Transformed(xf);
          placed.push_back(std::move(nb));
        }
        return placed;
      };
      refA = occBodies(p.at("a"));
      candB = occBodies(p.at("b"));
    }
    if (refA.empty() || candB.empty())
      throw std::runtime_error("compare_geometry needs {golden_path} or "
                               "{a, b} with geometry on both sides");
    CADShape missingShape, extraShape;
    // `materialize` draws the delta on the model instead of only measuring
    // it: one screenshot then answers WHERE it differs, which no similarity
    // number can. It needs the same two solids the *_path exports do.
    const bool materialize = p.value("materialize", false);
    const bool wantMissing = p.contains("missing_path") || materialize;
    const bool wantExtra = p.contains("extra_path") || materialize;
    const auto c =
        Measure::CompareBodySets(refA, glm::mat4(1.0f), candB, glm::mat4(1.0f),
                                 wantMissing ? &missingShape : nullptr,
                                 wantExtra ? &extraShape : nullptr);
    if (!c)
      throw std::runtime_error(
          "compare failed — an OpenCASCADE boolean threw, usually on an "
          "invalid or empty body; run check_geometry on both sides");

    const double unionV = c->volumeA + c->volumeB - c->common;
    const double iou = unionV > 1e-9 ? c->common / unionV : 0.0;
    const double dice = (c->volumeA + c->volumeB) > 1e-9
                            ? 2.0 * c->common / (c->volumeA + c->volumeB)
                            : 0.0;
    const double precision = c->volumeB > 1e-9 ? c->common / c->volumeB : 0.0;
    const double recall = c->volumeA > 1e-9 ? c->common / c->volumeA : 0.0;
    const double volumeRatio =
        c->volumeA > 1e-9 ? c->volumeB / c->volumeA : 0.0;
    const double centerOffset =
        glm::length(glm::dvec3(c->comA) - glm::dvec3(c->comB));
    const auto boxVol = [](const glm::vec3& mn, const glm::vec3& mx) {
      const glm::vec3 d = glm::max(mx - mn, glm::vec3(0.0f));
      return double(d.x) * d.y * d.z;
    };
    const glm::vec3 imn = glm::max(c->minA, c->minB);
    const glm::vec3 imx = glm::min(c->maxA, c->maxB);
    const double boxInter = boxVol(imn, imx);
    const double boxUnion =
        boxVol(c->minA, c->maxA) + boxVol(c->minB, c->maxB) - boxInter;
    const double bboxIou = boxUnion > 1e-9 ? boxInter / boxUnion : 0.0;

    std::vector<std::string> verdicts;
    char buf[160];
    if (iou > 0.99)
      verdicts.push_back("identical within tolerance");
    else if (iou > 0.95)
      verdicts.push_back("excellent match");
    else if (iou > 0.8)
      verdicts.push_back("good match");
    else if (iou > 0.5)
      verdicts.push_back("partial match");
    else
      verdicts.push_back("poor match");
    if (precision < recall - 0.05) {
      snprintf(buf, sizeof buf,
               "over-generating: %.1f%% of the candidate is extra geometry",
               (1.0 - precision) * 100.0);
      verdicts.push_back(buf);
    } else if (recall < precision - 0.05) {
      snprintf(buf, sizeof buf,
               "under-generating: %.1f%% of the reference is missing",
               (1.0 - recall) * 100.0);
      verdicts.push_back(buf);
    }
    if (volumeRatio > 1e-9 && (volumeRatio < 0.95 || volumeRatio > 1.05)) {
      snprintf(buf, sizeof buf, "%s by %.1f%%",
               volumeRatio < 1.0 ? "undersized" : "oversized",
               std::abs(1.0 - volumeRatio) * 100.0);
      verdicts.push_back(buf);
    }
    if (centerOffset > 1.0) {
      snprintf(buf, sizeof buf, "center of mass offset by %.1f mm",
               centerOffset);
      verdicts.push_back(buf);
    }

    json out = {{"iou", iou},
                {"dice", dice},
                {"precision", precision},
                {"recall", recall},
                {"volume_ratio", volumeRatio},
                {"center_offset", centerOffset},
                {"bbox_iou", bboxIou},
                {"volume_reference", c->volumeA},
                {"volume_candidate", c->volumeB},
                {"volume_common", c->common},
                {"volume_missing", c->missing},
                {"volume_extra", c->extra},
                {"surface_reference", c->surfaceA},
                {"surface_candidate", c->surfaceB},
                {"faces", {{"reference", c->facesA}, {"candidate", c->facesB}}},
                {"edges", {{"reference", c->edgesA}, {"candidate", c->edgesB}}},
                {"verdicts", verdicts}};
    if (p.contains("missing_path")) {
      const std::string path = p.at("missing_path").get<std::string>();
      if (!CADKernel::Instance().SaveBREP(path, missingShape))
        throw std::runtime_error("cannot write " + path +
                                 " — does the parent directory exist?");
      out["missing_path"] = path;
    }
    if (p.contains("extra_path")) {
      const std::string path = p.at("extra_path").get<std::string>();
      if (!CADKernel::Instance().SaveBREP(path, extraShape))
        throw std::runtime_error("cannot write " + path +
                                 " — does the parent directory exist?");
      out["extra_path"] = path;
    }
    if (materialize) {
      // The overlay is drawn in the ACTIVE PART's coordinate space, so it
      // needs a part tab to be drawn on. Refusing beats an overlay floating
      // somewhere plausible-looking and wrong.
      if (doc == nullptr)
        throw std::runtime_error(
            "materialize needs a part tab — the delta is drawn in the active "
            "part's coordinate space");
      snprintf(buf, sizeof buf,
               "iou %.3f — %.0f mm³ missing (red), %.0f mm³ extra (green)", iou,
               c->missing, c->extra);
      ui::SetGeometryDelta(state, missingShape, extraShape, buf);
      // Say whether it will actually be SEEN: a --headless instance has no
      // renderer, and "materialized: true" over an invisible overlay is the
      // kind of half-truth that costs an agent a debugging round.
      out["materialized"] = static_cast<bool>(state.meshRenderer);
      if (!state.meshRenderer)
        out["note"] = "delta recorded, but this instance has no renderer to "
                      "draw it on — run under tools/headless-preview.sh to "
                      "screenshot it";
    }
    return out;
  }
  if (m == "clear_geometry_delta") {
    // The off-switch for materialize. Separate from compare_geometry because
    // an agent that wants the plain model back should not have to run a
    // comparison to get it.
    ui::ClearGeometryDelta(state);
    return json{{"cleared", true}};
  }
  if (m == "contact_report") {
    // V1: the whole-assembly connectivity gate. Box prefilter over every
    // visible, unsuppressed occurrence pair; exact contact on candidates;
    // union-find over touching/clashing edges. A scripted build asserts
    // len(components) == 1 and near_misses == [] the way it asserts a BOM.
    //
    // Round-7 finding: on a PART tab the same sweep runs over the part's
    // BODIES — the occurrence-level gate structurally cannot see a bracket
    // floating INSIDE one part (the ladder mount plate that touched
    // nothing), so multi-body parts get their own connectivity check.
    if (doc != nullptr) {
      const double touchTol = p.value("touch_tol", 1e-3);
      const double nearTol = p.value("near_tol", 1.0);
      struct BEnt {
        const Body* body;
        std::vector<Body> single; // ContactBetween takes body sets
        glm::vec3 min, max;
      };
      std::vector<BEnt> ents;
      json skipped = json::array();
      for (const Body& b : doc->GetBodies()) {
        BEnt e{&b, {}, {}, {}};
        e.single.push_back(b);
        if (!Measure::WorldBounds(e.single, glm::mat4(1.0f), e.min, e.max)) {
          skipped.push_back(b.name);
          continue;
        }
        ents.push_back(std::move(e));
      }
      const auto boxGap = [](const BEnt& a, const BEnt& b) {
        const glm::vec3 gap =
            glm::max(glm::max(a.min - b.max, b.min - a.max), glm::vec3(0.0f));
        return static_cast<double>(glm::length(gap));
      };
      std::vector<size_t> parent(ents.size());
      for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
      const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
        return parent[i] == i ? i : parent[i] = find(parent[i]);
      };
      json pairs = json::array(), nearMisses = json::array(),
           clashes = json::array();
      for (size_t i = 0; i < ents.size(); ++i)
        for (size_t j = i + 1; j < ents.size(); ++j) {
          if (boxGap(ents[i], ents[j]) > nearTol)
            continue;
          const auto c = Measure::ContactBetween(
              ents[i].single, glm::mat4(1.0f), ents[j].single, glm::mat4(1.0f));
          if (!c)
            continue;
          const char* st = ContactState(*c, touchTol);
          pairs.push_back({{"a", ents[i].body->name},
                           {"b", ents[j].body->name},
                           {"state", st},
                           {"distance", c->distance},
                           {"volume", c->volume}});
          if (std::string_view(st) != "clear")
            parent[find(i)] = find(j);
          if (std::string_view(st) == "clash")
            clashes.push_back({{"a", ents[i].body->name},
                               {"b", ents[j].body->name},
                               {"volume", c->volume}});
          else if (c->distance > touchTol && c->distance <= nearTol)
            nearMisses.push_back({{"a", ents[i].body->name},
                                  {"b", ents[j].body->name},
                                  {"distance", c->distance}});
        }
      std::map<size_t, std::vector<std::string>> comps;
      for (size_t i = 0; i < ents.size(); ++i)
        comps[find(i)].push_back(ents[i].body->name);
      std::vector<std::vector<std::string>> componentList;
      for (auto& [root, names] : comps)
        componentList.push_back(std::move(names));
      std::sort(
          componentList.begin(), componentList.end(),
          [](const auto& a, const auto& b) { return a.size() > b.size(); });
      json out = {{"scope", "bodies"},
                  {"bodies", ents.size()},
                  {"connected", componentList.size() <= 1},
                  {"components", componentList},
                  {"pairs", pairs},
                  {"near_misses", nearMisses},
                  {"clashes", clashes},
                  {"touch_tol", touchTol},
                  {"near_tol", nearTol}};
      if (!skipped.empty())
        out["skipped"] = skipped;
      return out;
    }
    if (state.assembly == nullptr)
      throw std::runtime_error(
          "contact_report needs a part or assembly tab — switch tabs with "
          "set_active_part (see list_parts)");
    const double touchTol = p.value("touch_tol", 1e-3);
    const double nearTol = p.value("near_tol", 1.0);
    // A5: one node per TOP-LEVEL occurrence, whose geometry is its whole
    // flattened subtree in world space. That is what makes "is the gangway
    // attached to the dock" a question about two nodes and the hinges
    // between them, instead of thirty parts — and the reason a clash BETWEEN
    // two parts inside one sub-assembly does not appear here: it belongs to
    // that assembly's own report, on its own tab.
    struct Ent {
      const Occurrence* occ;
      std::vector<Body> bodies; // already placed in world space
      glm::mat4 xf{1.0f};       // identity: `bodies` carry the placement
      glm::vec3 min, max;
    };
    std::vector<Ent> ents;
    json skipped = json::array();
    std::map<uint64_t, std::vector<Body>> byRoot;
    for (const assembly::FlatInstance& inst :
         ui::FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      for (const Body& b : inst.part->GetBodies()) {
        Body nb = b;
        nb.shape = b.shape.Transformed(inst.world);
        byRoot[inst.rootOccurrenceId].push_back(std::move(nb));
      }
    }
    for (const Occurrence& o : state.assembly->GetOccurrences()) {
      if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
        continue;
      const auto it = byRoot.find(o.id);
      Ent e;
      e.occ = &o;
      if (it != byRoot.end())
        e.bodies = it->second;
      if (e.bodies.empty() ||
          !Measure::WorldBounds(e.bodies, e.xf, e.min, e.max)) {
        skipped.push_back(o.name); // nothing resolvable under this occurrence
        continue;
      }
      ents.push_back(std::move(e));
    }
    // AABB gap: 0 when the boxes overlap; a lower bound on exact distance.
    const auto boxGap = [](const Ent& a, const Ent& b) {
      const glm::vec3 gap =
          glm::max(glm::max(a.min - b.max, b.min - a.max), glm::vec3(0.0f));
      return static_cast<double>(glm::length(gap));
    };
    std::vector<size_t> parent(ents.size());
    for (size_t i = 0; i < parent.size(); ++i)
      parent[i] = i;
    const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
      return parent[i] == i ? i : parent[i] = find(parent[i]);
    };
    json pairs = json::array(), nearMisses = json::array(),
         clashes = json::array();
    std::set<std::pair<uint64_t, uint64_t>> touchingIds;
    for (size_t i = 0; i < ents.size(); ++i)
      for (size_t j = i + 1; j < ents.size(); ++j) {
        if (boxGap(ents[i], ents[j]) > nearTol)
          continue; // provably clear by more than the near window
        const auto c = Measure::ContactBetween(ents[i].bodies, ents[i].xf,
                                               ents[j].bodies, ents[j].xf);
        if (!c)
          continue;
        const char* st = ContactState(*c, touchTol);
        pairs.push_back({{"a", ents[i].occ->name},
                         {"b", ents[j].occ->name},
                         {"state", st},
                         {"distance", c->distance},
                         {"volume", c->volume}});
        if (std::string_view(st) != "clear") {
          parent[find(i)] = find(j);
          touchingIds.insert({std::min(ents[i].occ->id, ents[j].occ->id),
                              std::max(ents[i].occ->id, ents[j].occ->id)});
        }
        if (std::string_view(st) == "clash")
          clashes.push_back({{"a", ents[i].occ->name},
                             {"b", ents[j].occ->name},
                             {"volume", c->volume}});
        else if (c->distance > touchTol && c->distance <= nearTol)
          nearMisses.push_back({{"a", ents[i].occ->name},
                                {"b", ents[j].occ->name},
                                {"distance", c->distance}});
      }
    std::map<size_t, std::vector<std::string>> comps;
    for (size_t i = 0; i < ents.size(); ++i)
      comps[find(i)].push_back(ents[i].occ->name);
    std::vector<std::vector<std::string>> componentList;
    for (auto& [root, names] : comps)
      componentList.push_back(std::move(names));
    std::sort(componentList.begin(), componentList.end(),
              [](const auto& a, const auto& b) { return a.size() > b.size(); });
    json out = {
        {"occurrences", ents.size()},  {"connected", componentList.size() <= 1},
        {"components", componentList}, {"pairs", pairs},
        {"near_misses", nearMisses},   {"clashes", clashes},
        {"touch_tol", touchTol},       {"near_tol", nearTol}};
    if (!skipped.empty())
      out["skipped"] = skipped;
    // Intended vs realized: only meaningful when the assembly HAS mates —
    // in a fixed-occurrence assembly every touching pair is "unmated".
    if (!state.assembly->GetMates().empty()) {
      json unrealized = json::array(), unmated = json::array();
      std::set<std::pair<uint64_t, uint64_t>> mated;
      for (const Mate& mt : state.assembly->GetMates()) {
        const auto key =
            std::make_pair(std::min(mt.a.occurrenceId, mt.b.occurrenceId),
                           std::max(mt.a.occurrenceId, mt.b.occurrenceId));
        mated.insert(key);
        if (touchingIds.find(key) == touchingIds.end())
          unrealized.push_back({{"mate", mt.name}, {"id", mt.id}});
      }
      for (const auto& [ia, ib] : touchingIds)
        if (mated.find({ia, ib}) == mated.end()) {
          const Occurrence* a = state.assembly->FindById(ia);
          const Occurrence* b = state.assembly->FindById(ib);
          if (a != nullptr && b != nullptr)
            unmated.push_back({{"a", a->name}, {"b", b->name}});
        }
      out["unrealized_mates"] = unrealized;
      out["unmated_contacts"] = unmated;
    }
    return out;
  }
  if (m == "mass_properties") {
    // Volume, surface area, center of mass, and inertia (about the CoM) per
    // body, plus a document total. Mass assumes model units are millimetres:
    // grams = density[kg/m^3] * volume[mm^3] * 1e-6; bodies without an
    // assigned material report mass null (density unknown), and so does the
    // total if any contributor is unknown. params: {body?: index}
    //
    // On an ASSEMBLY tab there is no part doc: aggregate every visible,
    // non-suppressed occurrence's part mass in WORLD space (each part's CoM
    // carried through its solved occurrence transform — the same placement the
    // interference/BOM paths use). The reply's `bodies` array carries one row
    // per occurrence; `total` is the combined world mass/volume/CoM.
    if (doc == nullptr && state.assembly) {
      const assembly::PartResolver resolve = [&](uint64_t id) {
        return ui::PartDocById(state, id);
      };
      const assembly::AssemblyMass am = assembly::MassProperties(
          *state.assembly, resolve, ui::AssemblyResolverFor(state));
      json rows = json::array();
      for (const assembly::OccurrenceMass& om : am.occurrences) {
        json entry = {
            {"occurrence", om.occurrenceId},
            {"name", om.name},
            {"part_id", om.partId},
            {"volume", om.volume},
            {"center_of_mass",
             {om.centerOfMass.x, om.centerOfMass.y, om.centerOfMass.z}}};
        entry["mass_g"] = om.massKnown ? json(om.massG) : json(nullptr);
        rows.push_back(entry);
      }
      const glm::dvec3& com = am.centerOfMass;
      json out = {
          {"occurrences", rows},
          {"total",
           {{"volume", am.volume},
            {"center_of_mass", {com.x, com.y, com.z}},
            {"mass_g", am.massKnown ? json(am.massG) : json(nullptr)}}}};
      if (!am.skipped.empty())
        out["skipped"] = am.skipped;
      return out;
    }
    const auto& bodies = RequirePartDoc(doc, "mass_properties").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    // The math is shared with the BOM (A4): Measure::MassOfBodies. A single
    // body's Body copy is cheap (the shape is a refcounted OCC handle).
    std::vector<Body> subset;
    if (only >= 0)
      subset.push_back(bodies[only]);
    std::string err;
    const auto mass = Measure::MassOfBodies(only >= 0 ? subset : bodies, &err);
    if (!mass)
      throw std::runtime_error(err);
    if (mass->bodies.empty())
      throw std::runtime_error("no bodies");
    json out = json::array();
    for (size_t i = 0; i < mass->bodies.size(); i++) {
      const Measure::BodyMass& bm = mass->bodies[i];
      json inertia = json::array();
      for (int r = 0; r < 3; r++)
        inertia.push_back(
            {bm.inertia[r][0], bm.inertia[r][1], bm.inertia[r][2]});
      json entry = {{"body", only >= 0 ? static_cast<size_t>(only) : i},
                    {"name", bm.name},
                    {"volume", bm.volume},
                    {"surface_area", bm.surfaceArea},
                    {"center_of_mass",
                     {bm.centerOfMass.x, bm.centerOfMass.y, bm.centerOfMass.z}},
                    {"inertia", inertia},
                    {"material", bm.material}};
      if (bm.massKnown) {
        entry["density"] = bm.density;
        entry["mass_g"] = bm.massG;
      } else {
        entry["mass_g"] = nullptr;
      }
      out.push_back(entry);
    }
    const glm::dvec3& com = mass->centerOfMass;
    return json{
        {"bodies", out},
        {"total",
         {{"volume", mass->volume},
          {"center_of_mass", {com.x, com.y, com.z}},
          {"mass_g", mass->massKnown ? json(mass->massG) : json(nullptr)}}}};
  }
  if (m == "check_geometry") {
    // B-Rep validity per body (OCC BRepCheck_Analyzer) — flags invalid faces/
    // edges/wires and the distinct reasons. Read-only; useful on imported STEP
    // and after booleans. params: {body?: index} (all bodies when omitted).
    //
    // `valid` is strictly "every EXISTING body is a valid B-Rep" — vacuously
    // true over zero bodies. The natural "did my model build?" probe is `ok`,
    // which also demands no feature errored; body_count exposes the vacuous
    // case ("valid over nothing") explicitly.
    Document& part = RequirePartDoc(doc, "check_geometry");
    const auto& bodies = part.GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    json rows = json::array();
    bool allValid = true;
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && static_cast<int>(i) != only)
        continue;
      const Measure::GeometryReport r = Measure::CheckGeometry(bodies[i].shape);
      allValid = allValid && r.valid;
      rows.push_back({{"body", i},
                      {"name", bodies[i].name},
                      {"valid", r.valid},
                      {"bad_faces", r.badFaces},
                      {"bad_edges", r.badEdges},
                      {"bad_wires", r.badWires},
                      {"reasons", r.reasons}});
    }
    json featureErrors = RegenSummaryJson(part)["failed"];
    const bool ok = allValid && featureErrors.empty();
    json out = {{"valid", allValid},
                {"bodies", rows},
                {"body_count", bodies.size()},
                {"feature_errors", std::move(featureErrors)},
                {"ok", ok}};
    // Round-8 retro: floating geometry INSIDE a part warns by default —
    // the ladder's mount plate passed every opt-in gate because nobody
    // thought to ask. `ok` is untouched (disjoint multi-body parts can be
    // legitimate); scripts assert `connected` where it matters.
    if (bodies.size() >= 2 && only < 0) {
      const auto comps = BodyComponentNames(part, 1e-3, 1.0);
      out["body_components"] = comps;
      out["connected"] = comps.size() <= 1;
      if (comps.size() > 1) {
        json warnings = json::array();
        for (size_t i = 1; i < comps.size(); i++) {
          std::string names;
          for (const std::string& n : comps[i])
            names += (names.empty() ? "" : ", ") + n;
          warnings.push_back(
              "bodies [" + names +
              "] touch nothing else in this part — floating geometry inside "
              "a part is usually a mistake (contact_report on this tab lists "
              "the gaps)");
        }
        out["warnings"] = std::move(warnings);
      }
    }
    return out;
  }
  if (m == "measure") {
    // What ONE side of a measurement names. Each of `a` and `b` carries its
    // own {part} / {occurrence}, so a measurement can span two parts or two
    // occurrences — which the GUI has done since assembly measure shipped
    // and the RPC could not, because it resolved a single body set and used
    // it for both refs.
    //
    // NOTE: `interference` has a near-twin of the occurrence resolution
    // below. They are not yet one function; that is a recorded follow-up in
    // docs/inspect/measure-v2.md rather than a refactor smuggled into this
    // change, because interference's version is load-bearing for the
    // contact sweep and shares a prefetched body map with it.
    struct Side {
      std::vector<Body> owned; // world-placed bodies, for an occurrence
      const std::vector<Body>* bodies = nullptr;
      ElementRef ref; // name empty => the whole set
      std::string label;
    };
    auto resolveSide = [&](const json& r, const char* which) {
      auto side = std::make_unique<Side>();
      if (r.contains("occurrence")) {
        if (!state.assembly)
          throw std::runtime_error(
              std::string("measure: ") + which +
              ".occurrence needs an assembly tab (or open the assembly and "
              "pass `occurrence`); use `part` for a part document");
        const json& v = r.at("occurrence");
        const Occurrence* occ = nullptr;
        glm::mat4 world(1.0f);
        std::string label;
        if (v.is_number_unsigned() || v.is_number_integer()) {
          occ = state.assembly->FindById(v.get<uint64_t>());
          if (!occ)
            throw std::runtime_error("measure: occurrence id " + v.dump() +
                                     " not found (see list_occurrences)");
          label = occ->name;
        } else {
          const std::string name = v.get<std::string>();
          for (const Occurrence& o : state.assembly->GetOccurrences())
            if (o.name == name) {
              occ = &o;
              label = o.name;
              break;
            }
          if (!occ && name.find('/') != std::string::npos) {
            // A5: a part INSIDE a sub-assembly, by path.
            auto hit = assembly::ResolvePath(*state.assembly, name,
                                             ui::PartResolverFor(state),
                                             ui::AssemblyResolverFor(state));
            if (!hit)
              throw std::runtime_error(hit.error());
            occ = hit->occ;
            world = hit->world;
            label = name;
          }
          if (!occ)
            throw std::runtime_error(
                "measure: occurrence '" + name +
                "' not found (see list_occurrences; a part inside a "
                "sub-assembly is addressed by path, e.g. \"Gangway/Board "
                "19\")");
        }
        // Every leaf under it, placed in world space — a sub-assembly
        // measures as the one thing it looks like, not as its parts.
        if (label == occ->name) {
          for (const assembly::FlatInstance& inst :
               ui::FlattenActiveAssembly(state)) {
            if (inst.rootOccurrenceId != occ->id)
              continue;
            for (const Body& b : inst.part->GetBodies()) {
              Body nb = b;
              nb.shape = b.shape.Transformed(inst.world);
              side->owned.push_back(std::move(nb));
            }
          }
        } else if (const Document* pd = ui::PartDocById(state, occ->partId)) {
          for (const Body& b : pd->GetBodies()) {
            Body nb = b;
            nb.shape = b.shape.Transformed(world);
            side->owned.push_back(std::move(nb));
          }
        }
        if (side->owned.empty())
          throw std::runtime_error("measure: occurrence '" + label +
                                   "' resolved to no geometry");
        side->bodies = &side->owned;
        side->label = label;
      } else {
        const Document* doc2 =
            r.contains("part")   ? ResolvePartParam(state, r.at("part"))
            : p.contains("part") ? ResolvePartParam(state, p.at("part"))
                                 : &RequirePartDoc(doc, "measure");
        side->bodies = &doc2->GetBodies();
        side->label = "";
      }
      // A whole body: {feature_id} with no `element`. A whole occurrence:
      // no `element` either, and featureId 0 means every body under it.
      side->ref = ElementRef{r.value("feature_id", uint64_t{0}),
                             r.value("element", std::string{})};
      return side;
    };
    // Geometry queries over stable element names (I1). params:
    // {a: {feature_id, element}, b?: {feature_id, element}, part?}.
    // With `b`: minimum distance between the two elements. Without: the
    // element's own measure (face area / edge length). `part` (name or stable
    // id) resolves the elements against that part's bodies without switching
    // the active tab.
    const auto sideA = resolveSide(p.at("a"), "a");
    const Measure::Target ta{sideA->bodies, glm::mat4(1.0f), sideA->ref};
    if (p.contains("b")) {
      const auto sideB = resolveSide(p.at("b"), "b");
      const Measure::Target tb{sideB->bodies, glm::mat4(1.0f), sideB->ref};
      // What to measure between them. Refused, never substituted: a request
      // for an angle answered with a distance is plausible and wrong, and a
      // typo'd kind must not silently become the default.
      const std::string kind = p.value("kind", "distance");
      if (kind != "distance" && kind != "angle" && kind != "both") {
        throw std::runtime_error("measure: unknown kind '" + kind +
                                 "'. Accepted: distance, angle, both.");
      }
      // WHICH distance — minimum (default), maximum (the clearance
      // question), or centre to centre (hole spacing).
      Measure::DistanceKind dk = Measure::DistanceKind::Minimum;
      if (p.contains("distance_kind")) {
        const std::string dks = p.at("distance_kind").get<std::string>();
        if (!Measure::DistanceKindFromString(dks, dk)) {
          std::string names;
          for (const char* n : Measure::DistanceKindNames())
            names += (names.empty() ? "" : ", ") + std::string(n);
          throw std::runtime_error("measure: unknown distance_kind '" + dks +
                                   "'. Accepted: " + names + ".");
        }
      }
      json out;
      if (kind != "angle") {
        const auto d = Measure::DistanceBetween(ta, tb, dk);
        if (!d)
          throw std::runtime_error("measure: an element did not resolve");
        out["distance"] = d->distance;
        out["distance_kind"] = Measure::ToString(dk);
        out["point_a"] = {d->pointA.x, d->pointA.y, d->pointA.z};
        out["point_b"] = {d->pointB.x, d->pointB.y, d->pointB.z};
        // SIGNED, b - a. Which way it is matters: 40 mm in +X and 40 mm in
        // -X are a part and its mirror image.
        const glm::vec3 delta = d->pointB - d->pointA;
        out["delta"] = {delta.x, delta.y, delta.z};
        // Two WHOLE shapes that interpenetrate are 0 apart, which is true
        // and the least useful available fact. Say whether they clash.
        if (const auto ov = Measure::OverlapBetween(ta, tb)) {
          out["overlap_volume"] = ov->volume;
          out["clash"] = ov->volume > 0.0;
        }
      }
      if (kind != "distance") {
        if (ta.Whole() || tb.Whole()) {
          throw std::runtime_error(
              "measure: an angle needs a face or an edge on both sides — a "
              "whole body or occurrence has no single direction to take one "
              "against.");
        }
        const auto ang = Measure::Angle(*ta.bodies, ta.ref, ta.xf, *tb.bodies,
                                        tb.ref, tb.xf);
        if (!ang) {
          // Name WHICH element has no direction, and what does — a bare
          // "could not" leaves the caller guessing which ref to change.
          const bool aOk = Measure::DirectionOf(*ta.bodies, ta.ref).has_value();
          const bool bOk = Measure::DirectionOf(*tb.bodies, tb.ref).has_value();
          const std::string who = !aOk && !bOk ? "neither element"
                                  : !aOk       ? "element a"
                                               : "element b";
          throw std::runtime_error(
              "measure: " + who +
              " defines a direction to take an angle against. A plane, line, "
              "circle, cylinder, cone or torus does; a sphere or a spline "
              "does not.");
        }
        out["angle"] = ang->degrees;
        out["acute"] = ang->acute;
        out["parallel"] = ang->parallel;
        out["perpendicular"] = ang->perpendicular;
        out["basis"] = ang->basis;
      }
      return out;
    }
    if (p.contains("kind") || p.contains("distance_kind")) {
      throw std::runtime_error(
          "measure: `kind` and `distance_kind` choose what to measure BETWEEN "
          "two things — pass `b` as well, or drop them for a's own measure.");
    }
    // A WHOLE body or occurrence on its own reports what a part HAS.
    if (ta.Whole()) {
      const auto d = Measure::DescribeTarget(ta);
      if (!d)
        throw std::runtime_error("measure: that body or occurrence did not "
                                 "resolve to any geometry");
      return json{{"kind", "solid"},
                  {"volume", d->volume},
                  {"surface_area", d->surfaceArea},
                  {"size", {d->size.x, d->size.y, d->size.z}},
                  {"solids", d->solids}};
    }

    const auto e = Measure::Describe(*ta.bodies, ta.ref);
    if (!e)
      throw std::runtime_error("measure: element did not resolve");
    json out = {{"kind", e->kind}, {"shape", e->shape}};
    if (e->kind == "face")
      out["area"] = e->area;
    else
      out["length"] = e->length;
    // A radius wherever one is defined: a circular/arc edge, and now a
    // cylinder/sphere/torus FACE too — `query_geometry` has returned the
    // face radius all along off the same classification, so an agent could
    // read a bore's diameter and the person clicking that bore could not.
    if (e->hasRadius) {
      out["radius"] = e->radius;
      out["diameter"] = e->radius * 2.0;
    }
    if (e->hasAxis)
      out["axis"] = {e->axis.x, e->axis.y, e->axis.z};
    // How a person would name it, so a caller reporting a measurement can
    // say "the top face" instead of "1/face3".
    out["name"] = Measure::HumanName(*ta.bodies, ta.ref, state.ActiveUnits());
    return out;
  }
  if (m == "list_elements") {
    // Stable element names (topological naming) for each body's faces and
    // edges — what Fillet/Chamfer edge refs {feature_id, element} point at
    // (feature_id = the body's created_by). Each face carries its surface
    // kind + area, each edge its curve kind + length (+ radius when
    // circular), so agents can identify bore rims / seams without
    // measure-probing every name. Document-only: works headless.
    // params: {body?: index, part?: name|stable-id (cross-part, no tab
    // switch)}
    const auto& bodies =
        p.contains("part") ? ResolvePartParam(state, p.at("part"))->GetBodies()
                           : RequirePartDoc(doc, "list_elements").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    json out = json::array();
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only))
        continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names) {
        json f = {{"index", idx}, {"name", name}};
        if (const auto fi = Measure::DescribeFaceAt(bodies[i], idx)) {
          f["kind"] = fi->kind;
          f["area"] = fi->area;
          // Centroid: lets an importer match a specific face geometrically
          // (an importer's shell/draft face queries -> our stable face names).
          f["centroid"] = {fi->centroid.x, fi->centroid.y, fi->centroid.z};
          // Outward unit surface normal at the centroid: lets an agent tell a
          // +X wall from a -X wall without a separate select_face round-trip.
          f["normal"] = {fi->normal.x, fi->normal.y, fi->normal.z};
          // Radius/axis of a cylinder, sphere, cone or torus. Computed here
          // since C3 and emitted only by query_geometry until now, so the
          // obvious listing did not carry the obvious number.
          if (fi->hasRadius)
            f["radius"] = fi->radius;
          if (fi->hasAxis)
            f["axis"] = {fi->axis.x, fi->axis.y, fi->axis.z};
        }
        faces.push_back(f);
      }
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames) {
        json e = {{"index", idx}, {"name", name}};
        if (const auto ei = Measure::DescribeEdgeAt(bodies[i], idx)) {
          e["kind"] = ei->kind;
          e["length"] = ei->length;
          if (ei->hasRadius)
            e["radius"] = ei->radius;
          // Midpoint: lets an importer match a specific edge geometrically
          // (an importer's fillet/chamfer edge queries → our stable edge
          // names).
          e["midpoint"] = {ei->midpoint.x, ei->midpoint.y, ei->midpoint.z};
        }
        edges.push_back(e);
      }
      out.push_back({{"body", i},
                     {"name", bodies[i].name},
                     {"created_by", bodies[i].createdBy},
                     {"faces", faces},
                     {"edges", edges}});
    }
    return json{{"bodies", out}};
  }
  return std::nullopt;
}

} // namespace remote
