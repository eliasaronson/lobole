#pragma once

// Internal (non-public) shared helpers for the Serialize* domain translation
// units carved out of Serialization.cpp. Only the serialize TUs include this;
// consumers use core/Serialization.h. The tiny value/ref converters + the
// format constants live here (inline, global scope so both DocumentIO code and
// the file-local helpers resolve them unqualified); the heavier file/envelope
// helpers stay defined in Serialization.cpp.

#include "core/ElementMap.h" // ElementRef

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

inline constexpr const char* kFormat = "designer-document";
inline constexpr int kVersion =
    100; // v100: an assembly holds the CLEARANCES its designer declared, and
         //      re-checks them. "prop tip to arm is at least 2 mm" lived in a
         //      python verify() where nobody who opened the document could see
         //      it and no edit could break it. Absent when none.
         // v99: a tolerance's `ref2` -- with it the tolerance is on the
         //      DISTANCE between two elements (a shoulder-to-seat length, a
         //      slot width) rather than on one face's size. Explicit
         //      lower/upper only: an ISO class states how a shaft or a bore
         //      may vary and says nothing about a gap.
         // v98: `tolerances` -- a permitted variation recorded ON a measured
         //      element ("this bore is H7"). Design intent only: it moves no
         //      surface, so it sits outside the regen chain hash and a
         //      tolerance-only edit rebuilds nothing.
         // v97: an Import may KEEP the file's own coordinates
         //      (`keep_origin`). Absent = centred on the combined bounding
         //      box, which is what every file before this did, silently.
         // v96: a CAM profile's `drilled_bores` -- cutting INSIDE a level face
         //      cuts every opening in it, and this is the diameter up to which
         //      a round one is left to the DRILL. Width could not say it: a
         //      2.2 mm milled slot is narrower than a 3.2 mm drilled hole.
         //      Absent (0) cuts every opening.
         // v95: opt-in bounded thread engagement and thread-driven screw pitch.
         // v94: generated component ownership and source fingerprints.
         // v93: versioned engineering catalog references and embedded records.
         // v92: a CAM tool's `chipload_min`/`chipload_max` -- what THIS cutter
         //      wants per tooth, when somebody has a vendor's chart for it.
         //      Absent (the default) falls back to a scaling rule off the
         //      diameter, and every finding CITES which of the two it used:
         //      cutting data is empirical and published as vendor copyright,
         //      so there is no standard to vendor the way ISO fastener
         //      geometry could be. Written only when stated, so a job of
         //      uncharted tools re-saves byte-identically.
         // (v91: a CAM setup's `reach_offset` -- WHERE THE BED IS ON THE SHEET
         //      -- and `stock_max` as a work origin. The machine's reach box
         //      was travel-sized with its corner nailed to the stock's, which
         //      is an assumption for any sheet longer than the bed: it decided
         //      what got clipped and it MOVED when the part was turned round,
         //      so spinning a 1122 mm baffle end-for-end dropped seven
         //      operations off a machine that could reach every one of them.
         //      Zero is the old behaviour. `stock_max` exists because the near
         //      corner of a slid sheet is the end hanging in the air, which is
         //      a datum nobody can touch off on.
         // (v90: a drawing view's `hidden` — occurrence ids left OFF that view.
         //      A per-SHEET choice, not a model change, and the difference
         //      between a dimensionable drawing of a swept-surface model and
         //      no drawing at all: the single-cylinder engine is 47 s a view
         //      and 0.0 s without its two valve springs
         //      (docs/dogfood-friction-plan.md, F4).
         // (v89: a drawing view's `quality` — "exact" (curve HLR, every curve
         //      traced to the edge that made it, what a dimension attaches to)
         //      or "fast" (polygonal HLR: orders of magnitude cheaper, and a
         //      LOOK). Exact costs what the SURFACES cost, not what the part
         //      count costs: 16 bored boxes project in 3.1 ms, one 4-turn
         //      helical spring in 6.4 s
         //      (docs/agent-scale-followup-plan.md, W3).)
         // (v88: relation kinds `chain` (a gear whose ratio keeps its sign — a
         //      chain or belt does not reverse) and `cam` (rotation to offset
         //      through a periodic LIFT TABLE, not a ratio), with the table
         //      in `lift` (docs/agent-scale-plan.md, G4).)
         // (v87: assembly `motion` -- a MotionStudy: one mate DOF driven over a
         //      range in N samples, solved at each and clash-checked, so a
         //      mechanism's motion is a thing the document holds
         //      (docs/agent-scale-plan.md, G2).)
         // (v86: the `Frame` feature (a named datum frame: explicit, from one
         //      or several elements, or offset from another) and the mate
         //      connector anchor `{frame: <id>}` that mates to it by name
         //      (docs/agent-scale-plan.md, G0).)
         // (v85: `Derived.body_names` -- pick a source's bodies by NAME as well
         //      as by their created_by id, because one Import feature creates
         //      every body of a STEP (a camera module arrives as 197)
         //      (docs/modeling/derived-followup-plan.md, I3).)
         // (v84: the `Derived` feature -- a snapshot of another part's bodies
         //      (B-Reps as content-addressed document assets, names + element
         //      maps in the feature), placement, `reference` flag, and the
         //      source's chain hash for the stale check
         //      (docs/modeling/derived-plan.md).)
         // (v83: assembly `relations` -- a gear, rack, screw or linear coupling
         //      between two mates' degrees of freedom, dofB = ratio * dofA +
         //      phase. Omitted when none (docs/assemblies/relations-plan.md).
         // (v82: `finish_tool` + `finish_stock` on a profile or pocket -- rough
         //      with the operation's tool leaving the stock, then one
         //      full-depth pass with the finishing tool. Omitted when none
         //      (docs/cam/router-round.md, R6).)
         // (v81: `lead_arcs` on a profile -- leave the wall on a tangent arc of
         //      the tool's radius at the end of every pass. Omitted when off
         //      (docs/cam/router-round.md, R5).
         // (v80: `corner_relief` on a profile or pocket (dogbone | tbone):
         //      clear the inside corners a round cutter leaves, by the tool's
         //      radius. Omitted when none (docs/cam/router-round.md, R4).
         // (v79: `peck` on a drill operation -- retract fully between pecks of
         //      this depth, as plain moves. Omitted when 0. A hole wider than
         //      the tool is bored, which needs no key: the tool decides
         //      (docs/cam/router-round.md, R3).
         // (v78: `split_output` on a setup -- one program per tool, which most
         //      hobby controllers need because they cannot take a tool change
         //      mid-file. Forces tool order. Omitted when off
         //      (docs/cam/router-round.md, R2).
         // (v77: a setup EMBEDS its machine. `post`, `fit_arcs` and
         //      `has_rotary_axis` were keys of the setup and every new job
         //      re-answered "which machine" with a combo; they are now a
         //      `machine` block -- name, travel, post, arcs, rotary, rapid,
         //      tool library -- that is a copy of a globally stored profile
         //      named by `name` (~/.config/designer/machines/<slug>.json), so
         //      the file opens on a machine that has never seen the profile
         //      and the panel says which copy it is using when the two differ
         //      (docs/cam/job-document.md, round two). The three old keys are
         //      still read from a file without the block.
         // (v76: fixturing is per PART. A placement can carry its own `frame`,
         //      so one sheet can hold a baffle laid on its face beside a rail
         //      laid on its edge -- each modelled in its assembly orientation,
         //      all cut from one board. Unset means "as the setup says", which
         //      keeps Setup::frame the single-part answer and re-saves an
         //      existing job byte-identically. Optional rather than defaulted
         //      to identity: a default would silently un-clamp a part the
         //      moment you gave it a position (docs/cam/job-document.md).
         // (v75: a setup says in what ORDER it cuts (`order`:
         //      authored|tool|part). With one part authoring order IS
         //      cutting order; with four, authoring per part means a tool
         //      change per part per tool. Omitted at `authored`, which is
         //      byte-for-byte what this app posted before -- the bump is for
         //      reading, where an older build would cut a file that says
         //      "tool" in the order it was written, which is a different
         //      program (docs/cam/job-document.md J5).
         // (v74: a job holds MORE THAN ONE part, which is what the job document
         //      was for. A machining selection names the part it is on
         //      (`part` beside `feature_id`/`element`), because two parts in
         //      one job can own the same element name; and a setup carries
         //      `placements`, where each part sits in the stock -- in-plane
         //      only, since out-of-plane is what the fixturing frame says.
         //      Both omitted at their defaults, so a one-part job written by
         //      v73 re-saves byte-identically. The bump is for the other
         //      direction: a v73 build reads a qualified target and drops the
         //      qualifier, which silently machines the wrong part
         //      (docs/cam/job-document.md).
         // (v73: CAM setups are no longer part of the part. They live in a JOB
         //      document of their own (`kind: "job"` -- a parts list plus the
         //      setups that cut them), which is what makes a job about more
         //      than one part: four brackets out of one sheet used to be
         //      inexpressible (docs/cam/job-document.md). BREAKING for any
         //      document carrying `cam`: a part's setups are not migrated,
         //      because the job they belong in is a document that did not
         //      exist. No users, so no migration (CLAUDE.md).
         // (v72: an occurrence can RIDE ON another (`attached_to` +
         //      `attach_offset`/`attach_rotation`), so a fastener travels with
         //      the part it is bolted to and cannot be dragged out of its hole
         //      on its own. Composition, not a mate: no solver sees it
         //      (docs/parts/f1-loose-ends.md). Omitted when free, so every
         //      existing assembly serializes byte-identically.
         // (v71: a Hole records what it is FOR — `fit: {thread, kind, pitch?}`
         //      — and the diameter is DERIVED from it, so a clearance hole
         //      stops being a number somebody looked up once. Additive and
         //      optional: a hole without one serializes exactly as before
         //      (docs/modeling/hole-standards.md).
         // (v70: seven fastener standards instead of one (nuts, washers, hex
         //      and countersunk and button heads, a hexalobular drive), the
         //      drive DEFAULTED to the one its standard is made with rather
         //      than to "hex", and the `fastener_callout` drawing dimension.
         //      All three widen what a loader must ACCEPT — a v69 build reads
         //      "iso4017" as an unknown standard — which is what the bump is
         //      for (docs/parts/plan.md, phases 4-5).
         // (v69: the Fastener feature — a standard part built from a vendored
         //      dimension table (docs/parts/plan.md), carrying only what the
         //      table cannot give it: which standard, which size, how long
         // (v68: cam operation `use_outline` (clear the face's outline,
         //      passing over its openings rather than around them);
         //      the cam work origin is DERIVED from a named corner
         // (`origin_at`) and the post subtracts it; fixed-size stock
         // (`stock.mode`/`size`/`place`) — docs/cam/stock-and-origin.md
         // (v67: the cam setup's fixture frame + whether the machine has a
         // rotary axis (docs/cam/setup-frame.md))
         // (v66: drawing notes; cam tool type/tip angle + the edge-break op)
         // (v65: mate connector corner snap (anchor snap_edge2))
         // (v64: display units (length/mass/angle) travel with the document)
         // (v63: the cam setup remembers the face Z zero was picked from)
         // (v62: cam operation `side` written for clearing ops too)
         // (v61: expression-driven Box/Sphere/Cylinder dimensions)
         // (v57: mate connector inner_path)

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}
inline glm::vec3 Vec3FromJson(const nlohmann::json& j) {
  return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(),
                   j.at(2).get<float>());
}

// Quaternion as [x, y, z, w] (glm stores (w,x,y,z); JSON order matches the
// a2-design occurrence schema).
inline nlohmann::json QuatToJson(const glm::quat& q) {
  return nlohmann::json::array({q.x, q.y, q.z, q.w});
}
inline glm::quat QuatFromJson(const nlohmann::json& j) {
  return glm::quat(j.at(3).get<float>(), j.at(0).get<float>(),
                   j.at(1).get<float>(), j.at(2).get<float>()); // (w,x,y,z)
}

inline nlohmann::json Vec2ToJson(const glm::vec2& v) {
  return nlohmann::json::array({v.x, v.y});
}
inline glm::vec2 Vec2FromJson(const nlohmann::json& j) {
  return glm::vec2(j.at(0).get<float>(), j.at(1).get<float>());
}

inline nlohmann::json ElementRefJson(const ElementRef& ref) {
  return nlohmann::json{{"feature_id", ref.featureId}, {"element", ref.name}};
}
inline ElementRef ElementRefFromJson(const nlohmann::json& j) {
  return ElementRef{j.value("feature_id", uint64_t{0}),
                    j.value("element", std::string{})};
}
