// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

#include <cstdint>
#include <string>
#include <vector>

namespace drawing {
struct ViewProjection; // core/drawing/ViewProjection.h
}

inline constexpr const char* kFormat = "designer-document";
inline constexpr int kVersion =
    167; // v167: a motion study's `clash_deflection_mm` (omitted when the
         //      mesh check derives its deflection per body), the mesh clash
         //      check's tessellation in mm
         //      (docs/assemblies/README.md#motion-studies).
         //      Underlays are stripped from the regen hash already.
         // v165: a requirement's measure kind `section_area` {document:
         //      part, base_plane+offset or origin+normal, of: material|
         //      enclosed, bodies?: [names or feature ids]}, mm2
         //      (docs/requirements/README.md#measures); and six building
         //      materials, brick, roof_tile, plaster, roofing_felt, render
         //      and soil (docs/rendering/materials.md). A build that knows
         //      neither refuses the file.
         // v164: a face appearance override
         //      (docs/rendering/materials.md#a-face-of-its-own): on a
         //      body's creating feature, `face_appearance` [{element,
         //      color?, roughness?, metallic?}] beside `appearance`, keyed by
         //      the face's element name. Stripped from the regen hash.
         // v163: a requirement's measure kind `bearing_bore` {document:
         //      assembly, occurrences: [bearing ids]}, the smallest bore of
         //      those bearings in mm (docs/requirements/README.md#measures).
         //      A build that does not know it refuses the file.
         // v162: saved drawing projections
         //      (docs/drawings/README.md#saved-projections): in the directory
         //      form, beside each view's file, its last projection as
         //      views/<id>-<slug>.projection.json {kind, revision,
         //      input_hash, mode, view_dir, up, sectioned, curve arrays,
         //      cuts, bin_bytes} + .projection.bin (f32 points, index
         //      arrays). Derived, keyed by its input hash; never written by
         //      an autosave or kept in the project's history. The
         //      single-file form carries none.
         // v161: feature templates (docs/modeling/templates.md): a
         //      `Template` feature {template, revision, on?, parameters,
         //      ids [[local, id]], definition}, and the project's template
         //      library under `templates` (a single-part file's envelope;
         //      project.json in the directory form).
         // v160: the project's requirements (docs/requirements/README.md):
         //      `requirements/<id>-<slug>.json` beside project.json, each
         //      {id, name, source?, revision, measure {kind, document, ...},
         //      bound {kind, value|min|max}, evidence?}, and
         //      `requirements_next_id` in project.json; a lone part carries
         //      them as `requirements` in its envelope. A measure or bound
         //      kind this build does not know is refused on load. Saved
         //      assembly `assertions` load as clearance requirements.
         // v159: a saved motion result lists the press fits its exact checks
         //      relieved, `press_fits` [{a, b, a_name, b_name, a_inner?,
         //      b_inner?, fit, interference_mm, max_interference_mm,
         //      volume_mm3}]; omitted when there are none
         //      (docs/tolerances/README.md#press-fits).
         // v158: a CAM operation's `cutter_comp`: off (omitted) | wear --
         //      profile, pocket and drill post their finished wall passes
         //      under G41/G42 with the tool table holding the wear alone
         //      (docs/cam/post.md#cutter-compensation). An unknown value is
         //      refused.
         // v157: an attached occurrence may name the faces it is SEATED on,
         //      `attach_seat` {face, axis: {feature_id, element},
         //      inner_path?} (a fastener: its hole's face and bore); with it
         //      `attach_offset`/`attach_rotation` are in the seat's frame, so
         //      it follows a hole that moves inside its part. Omitted
         //      otherwise; both faces or the load is refused
         //      (docs/parts/README.md#attached-and-mated).
         // v156: two feature types — RetainingRing {standard: din471|din472,
         //      size} (a generated retaining ring) and RetainingRingGroove
         //      {face, from, offset (+_expr), standard?} (its groove cut in a
         //      shaft or bore) (docs/parts/README.md#retaining-rings).
         // v155: sketch constraint refs reach an ellipse's (or elliptical
         //      arc's) axis ends, which 3/4 (major) and 5/6 (minor), and an
         //      ellipse with r_minor > r_major (or r_minor <= 0) is refused
         //      on load (docs/sketching/README.md#ellipses-and-splines).
         // v154: a study or motion result's numeric arrays move out of its
         //      .result.json into a <base>.result.bin beside it (raw
         //      little-endian f32 fields, u16/u32 indices, f64 placements),
         //      each named in the json by {type, offset, count}; the json
         //      keeps the metadata, `input_hash` and `bin_bytes`. A static
         //      or modal body keeps its boundary surface only, with
         //      `node_count`/`tet_count` of the mesh it solved
         //      (docs/simulation/README.md#what-a-saved-result-keeps).
         // v153: every study result is saved beside its study with its
         //      `input_hash`: a static or modal one in a part's or an
         //      assembly's studies/<id>-<slug>.result.json (`kind`,
         //      `maxima`, each body's mesh and `results` or `cases`, a shape
         //      study's `shape`), a motion one in motion/<id>-<slug>
         //      .result.json (docs/simulation/README.md#studies-as-jobs).
         // v152: studies -- `load_cases` [{name, weight, weight_expr?}] and a
         //      force's or pressure's `case`; a topology study's `keep_outs`
         //      [{body} | {box_min, box_max}]; a static study's `shape_from`
         //      {study, threshold? | whole?}. A topology result carries
         //      `cases` [{name, weight, compliance}], `keep_outs` and
         //      `islands` (docs/topopt/README.md#topology-you-can-build-from).
         // v151: drawings -- a view's `detail` {view, center [x, y, z],
         //      radius_mm, label} (docs/drawings/README.md#detail-views); a
         //      note's `style: "fcf"` with its frame in `fcf` {symbol,
         //      tolerance_mm, diameter?, modifier?, datums} and `dim`, the
         //      dimension a frame stands under
         //      (docs/drawings/README.md#feature-control-frames). Material
         //      slugs `aluminum_6061_t6` and `aluminum_6082_t6`.
         // v150: project tables -- `tables` [{name, comment?, columns,
         //      rows [{key, values}]}] in a single-part file's envelope and
         //      in project.json, read by lookup("table", key, "column")
         //      (docs/variables/README.md#tables). A cylinder's or cone's
         //      connector frame takes the surface's seam as X
         //      (docs/assemblies/solver.md#connector-frames), so a stored
         //      mate `rotation` on a round face counts from there.
         // v148: a topology result's `input_hash` ("0x" + 16 hex digits):
         //      what produced it, so a reopened project reuses it for the
         //      same inputs and reads it stale otherwise
         //      (docs/simulation/README.md#studies-as-jobs).
         // v149: Hole `drill_point_deg` (a blind floor's point; absent =
         //      derived) and `thread_depth` (+_expr; a tapped fit's thread
         //      from the mouth; absent = derived)
         //      (docs/modeling/holes-and-threads.md#hole).
         // v147: drawings -- a view's `section` (a section view), a note's
         //      `style` (balloon, datum) and table `bom` (a parts list), and
         //      the title block's `material` and `scale`
         //      (docs/drawings/README.md#section-views).
         // v145: CAM tool type `thread_mill` {tip_flat, neck_diameter,
         //      neck_length} and operation type `thread_mill` {passes}: a
         //      helix milled from the Thread on its face.
         // v143: a part's `print` {process, material} -- how it is printed,
         //      which the fit checks read for the allowance to model
         //      (fdm/petg). Drives no geometry.
         // v141: feature type `Bearing` {designation}: an ISO 15
         //      deep-groove ball bearing as a generated standard part.
         // v140: a Thread's `hand` (right | left; absent = right), and a
         //      modelled thread cuts real geometry.
         // v139: a setup's own `rapid_feed` (mm/min), saved with the setup;
         //      absent = 0, not stated. Only the machine's copy was before.
         // v138: an operation of type `engrave` -- sketch curves followed
         //      on the curve -- and its `width_mm` / `depth_mm`.
         // v137: the sketch `text` entity -- a one-line label regenerated
         //      from its fields (font, cap height, anchor, alignment).
         // v136: an operation's `index_a_deg` -- the rotary indices it is
         //      cut at, one generation per angle (absent = the setup's).
         // v135: a part placement's `configuration_id` names one of the
         //      PART's configurations (it was sub-assemblies only), and
         //      an occurrence's `material` overrides its bodies'; a job
         //      part's `configuration`.
         // v134: a drawing view's `rotation_deg` -- the view turned on the
         //      sheet about its own direction (absent = 0); a dimension
         //      ref's `extreme` (left/right/top/bottom, absent = none).
         // v133: a Derived's `configuration` -- the source configuration the
         //      copy is taken in (absent = the source's active one).
         // v132: how a machine's rotary is INSTALLED (`rotary`: along,
         //      chuck_end, word, top_moves) and what holds a part in it
         //      (`holding`: the chuck and the tailstock); a setup's `held_in`,
         //      `grip_mm`, `tailstock` and `chuck`. A setup held in the
         //      rotary of a machine that does not state its mount is not
         //      posted (docs/cam/rotary.md#how-the-rotary-is-installed).
         // v131: a `Tool`'s `shank_diameter` -- how wide it is above the
         //      flutes. Wider than the cutter makes a cut deeper than the
         //      flutes a collision, refused when the op is made
         //      (docs/cam/setup.md#reach) and found at the move by the
         //      verifier. Written only when stated, so a v130 build reads
         //      a job of plain tools byte-identically.
         // v130: an explode step's `path` -- [a flexible placement], with
         //      `occurrence` the sub-assembly's id for a moving part inside
         //      it, exploded on its own on top of the unit's step
         //      (docs/assemblies/README.md#flexible-sub-assemblies).
         // v129: a FLEXIBLE sub-assembly occurrence (`flexible`,
         //      `inner_poses` {occurrence, position, orientation},
         //      `inner_mate_dofs` {mate, rotation, offset, dx?, dy?,
         //      ball_rotation?}); a mate named by {path, mate} -- a
         //      relation's `mate_a_path`/`mate_b_path`, a motion study's
         //      `driver_path`
         //      (docs/assemblies/README.md#flexible-sub-assemblies).
         // v128: an AxisRef kind `custom` {origin, direction} (revolve,
         //      pattern and datum planes about any axis); a helix's
         //      `start_deg` (+_expr); a motion study's `configuration`
         //      (generators/pes_pump/README.md#how-it-moves).
         // v127: a fixture's optional `seat` -- one of its faces resting on
         //      the stock's top, reaching in over an edge ({face: {feature,
         //      name}, edge: -x|+x|-y|+y, along_mm, over_mm, spin_deg}),
         //      re-solved against the stock on every read
         //      (docs/cam/setup.md#seats).
         // v126: a CAM setup's `fixtures` -- clamps and vises, each a part
         //      placed on the bed ({id, part, name, pos, rot_z, enabled}),
         //      which the verifier treats as somewhere the tool must never
         //      go (docs/cam/setup.md#workholding-fixtures).
         // v125: a `surface_parallel` op reads `stepdown`, ROUGHING in
         //      layers from the stock top (a v124 file's unread stepdown on
         //      one is dropped on load), and surface ops gain
         //      `rest_from_tool`: cut only where that larger cutter could not
         //      reach
         //      (docs/cam/operations.md#rough-a-surface-and-rest-machining).
         // v124: a third `rotary_pattern`, `helical` -- passes that follow
         //      a twist, helices stepped round the part -- and `lead_mm`,
         //      their advance per turn (0 = the part's own twist). A v123
         //      build refuses the pattern name, which is what the bump is for
         //      (docs/cam/rotary.md#patterns-rotary_pattern).
         // v123: a `surface_rotary` op's `rotary_pattern` — `parallel`
         //      (the v122 raster, and the default) or `spiral`, one helix
         //      with A turning while the tool cuts: continuous 4-axis. And
         //      the op reads `stepdown` now, cutting in layers from the
         //      blank inward (a v122 file's unread stepdown on a rotary op
         //      is dropped on load). The machine gains `a_max_deg_min`, the
         //      A axis's top speed. A v122 build would read a spiral as
         //      parallel passes, a different cut, so it is a bump
         //      (docs/cam/rotary.md#continuous-surface_rotary).
         // v122: an `OpType` of `surface_rotary` — 4-axis finishing by
         //      passes ALONG the rotary axis stepping round it,
         //      the part turning between them to present each one to the
         //      spindle (a rotary raster; the spiral is v123). An
         //      unknown op type already refuses the file by name, so a v121
         //      build refuses this one, which is what the bump is for
         //      (docs/cam/rotary.md#verifying-a-turning-part).
         // v121: a `Tool`'s `overall_length` and `collar_diameter` — how far
         //      the cutter sticks out of the collet and how wide the nut is.
         //      Together they turn on the holder check in the verifier, which
         //      finds the collar driven into material the cutter never went
         //      near. Written only when stated, so a v120 build reads a job
         //      of undimensioned tools byte-identically and one WITH them
         //      simply stops checking the holder
         //      (docs/cam/simulation.md#holder-collisions).
         // v120: a `Cope`'s `mitre` list — the members it meets END TO END,
         //      cut on the plane that bisects them, which is the cut a
         //      corner wants and a fishmouth cannot make. A v119 build
         //      reads the key as absent and leaves every corner in the
         //      frame square-cut, which is a tube that does not sit down
         //      on its neighbour (docs/tube/README.md#cope-and-mitre).
         // v119: a `Cope` feature, which cuts a frame member to the members
         //      it MEETS and leaves those standing; and `tube`, the SHOP a
         //      part is bent in — the material, and the bender's die set,
         //      clamp straight, grip length and thinning limit. No size:
         //      each member's Pipe carries its own, because a frame is
         //      several at once. An unknown feature type is refused by name;
         //      a build without the rules reads the key as nothing and
         //      checks a frame against nothing (docs/tube/README.md).
         // v118: two finishing OpTypes, `surface_parallel` and
         //      `surface_waterline`, whose Z varies continuously WITHIN a
         //      pass -- and the three quantities they need, each named for
         //      what it is: `cut_angle_deg` (the raster direction),
         //      `sample_step_mm` (spacing ALONG a pass) and `z_step_mm` (the
         //      Z between waterline laps, which is NOT a stepover). A build
         //      without the types refuses the file by name, which is why it
         //      is a bump (docs/cam/operations.md#3-axis-surfacing).
         // v117: a `Curve3D` of kind `bent_path`: `points` are the CORNERS
         //      of a centreline and `radius` (+ _expr) the former it is
         //      bent round, swept line-arc-line. A build without the kind
         //      refuses the file by name, which is why it is a bump
         //      (docs/modeling/README.md#3d-curves-sweep-loft-and-pipe).
         // v116: a Hem's `offset_start` / `offset_end` (+ _expr): insets
         //      from the ends of the picked edge, as an EdgeFlange has. A
         //      hem running the full length of a pan's wall fuses to the
         //      wall beside it and the part flattens from no blank
         //      (docs/sheetmetal/README.md#features).
         // v115: a drawing view's `configuration` (omitted = the part's
         //      active one) and a drawing note's `table`: none | bends — a
         //      flat pattern on a sheet
         //      (docs/sheetmetal/README.md#dxf-and-drawings).
         // v114: `NormalCut` {sketch_id, body_id?, regions?}: a cut through
         //      a sheet square to the metal wherever the profile lands
         //      (docs/sheetmetal/README.md#features).
         // v113: `CornerRelief` {body_id, style?, size?} and an EdgeFlange's
         //      `corners`: open (default) | closed. Both refuse an unknown
         //      name (docs/sheetmetal/README.md#features).
         // v112: `Hem` {edges, length, kind?, gap?, outside?} and `Jog`
         //      {sketch_id, height, angle_deg?, flip?}; an edge flange over
         //      edges meeting at a corner now MITRES them (bends record the
         //      trim, transiently). An unknown hem `kind` is refused by name
         //      (docs/sheetmetal/README.md#features).
         // v111: `Unfold` {body_id, fixed_face?, bends?} and `Refold`
         //      {unfold_id}: lay chosen bends flat in place, and fold them
         //      back carrying every through-cut across each crease. The
         //      creases they leave carry their parent placement on the body,
         //      transiently — nothing new is written but the two features
         //      (docs/sheetmetal/README.md#how-the-unfolder-works).
         // v110: the BLANK. `FlatPattern` {body_id, fixed_face?} replaces a
         //      sheet body's shape with its flat pattern IN PLACE (a folded
         //      part and its blank occupy the same space, so a second body
         //      would sit inside the first). It is authored SUPPRESSED and
         //      turned on by a configuration, which needed the other half of
         //      this bump: a part configuration's `unsuppressed_features`,
         //      the last word over both suppression lists. Without it a
         //      configuration could only ever REMOVE, so a feature that is
         //      off in the authored model could not be the thing a
         //      configuration is for (docs/sheetmetal/README.md#features).
         // v109: the first two sheet features. `BaseFlange` {sketch_id,
         //      reversed?, regions?} is a closed profile thickened by the
         //      document's sheet-metal thickness; `EdgeFlange` {edges,
         //      length, angle_deg, radius?, position?, length_mode?,
         //      offset_start?, offset_end?, relief?} bends a wall up from a
         //      straight edge of a panel. Neither carries a thickness — that
         //      is what v108's rules are for — and an unknown `position`,
         //      `length_mode` or `relief` is refused naming the accepted set
         //      (docs/sheetmetal/README.md#features).
         // v108: sheet-metal rules in the envelope (`sheet_metal`, absent
         //      when the part is not sheet metal): {material, gauge,
         //      thickness, bend_radius?, k_factor?, relief?, relief_width?,
         //      relief_depth?}. The optional fields are DERIVED from the
         //      material and gauge when absent, so a table correction reaches
         //      the part; a v107 build drops the object and every sheet
         //      feature then refuses for want of a thickness
         //      (docs/sheetmetal/README.md#rules-the-stock-a-part-is-cut-from).
         // v107: a participant role `bonded` (S2): meshed AND glued to the
         //      meshed occurrence where they touch — one mesh spans them,
         //      each keeps its material. A v106 build refuses the file
         //      rather than reading the role as something else.
         // v106: FEM studies on an ASSEMBLY (`studies` in the assembly file,
         //      absent when none): `participants` [{occurrence, role:
         //      meshed|rigid|ignored, mesh_size?}], loads and refinements
         //      carrying `occurrence`, and a fixed load anchored on a CONTACT
         //      (`contact_with`, no `face`) — clamped where that rigid
         //      occurrence touches the meshed one, re-derived at mesh time
         //      (docs/simulation/README.md#assembly-studies).
         // v105: a CLICK relation (a pawl: one DOF that may only travel one
         //      way), and a freewheel's `teeth` and `ride` — riding over
         //      them lifts a named DOF
         //      (generators/watch/README.md#keyless-works-and-winding).
         // v104: a FREEWHEEL relation (one-way clutch): `kind` "freewheel",
         //      `drives` positive|negative and `lead`, the history it keeps.
         // v103: an Import's `units` -- the length unit an STL/BREP was drawn
         //      in (mm|cm|m|in; absent = mm). Refused on STEP, which declares
         //      its own. Bumped so a v102 build refuses the file rather than
         //      importing a metre mesh at 1:1.
         // v102: a cam relation's follower may TURN: `dof_b` "rotation" and
         //      the lift table's second column in DEGREES (mm for a slide).
         //      No new field; bumped so a v101 build refuses the file rather
         //      than reading the degrees as radians.
         // v101: an assembly configuration suppresses MATES and RELATIONS and
         //      holds mate DOFs (`suppressed_mates`, `suppressed_relations`,
         //      `mate_overrides`) -- an overlay the solve applies, so "crown
         //      pulled out" is a configuration. Absent when empty.
         // v100: an assembly holds the CLEARANCES its designer declared, and
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
         //      (generators/README.md#single-cylinder-and-air-cooled-engines).
         // (v89: a drawing view's `quality` — "exact" (curve HLR, every curve
         //      traced to the edge that made it, what a dimension attaches to)
         //      or "fast" (polygonal HLR: orders of magnitude cheaper, and a
         //      LOOK). Exact costs what the SURFACES cost, not what the part
         //      count costs: 16 bored boxes project in 3.1 ms, one 4-turn
         //      helical spring in 6.4 s
         //      (docs/drawings/README.md#projection-hlr).)
         // (v88: relation kinds `chain` (a gear whose ratio keeps its sign — a
         //      chain or belt does not reverse) and `cam` (rotation to offset
         //      through a periodic LIFT TABLE, not a ratio), with the table
         //      in `lift` (docs/assemblies/relations.md#kinds).)
         // (v87: assembly `motion` -- a MotionStudy: one mate DOF driven over a
         //      range in N samples, solved at each and clash-checked, so a
         //      mechanism's motion is a thing the document holds
         //      (docs/assemblies/README.md#motion-studies).)
         // (v86: the `Frame` feature (a named datum frame: explicit, from one
         //      or several elements, or offset from another) and the mate
         //      connector anchor `{frame: <id>}` that mates to it by name
         //      (docs/assemblies/solver.md#connector-frames).)
         // (v85: `Derived.body_names` -- pick a source's bodies by NAME as well
         //      as by their created_by id, because one Import feature creates
         //      every body of a STEP (a camera module arrives as 197)
         //      (docs/modeling/derived.md#the-feature).)
         // (v84: the `Derived` feature -- a snapshot of another part's bodies
         //      (B-Reps as content-addressed document assets, names + element
         //      maps in the feature), placement, `reference` flag, and the
         //      source's chain hash for the stale check
         //      (docs/modeling/derived.md).)
         // (v83: assembly `relations` -- a gear, rack, screw or linear coupling
         //      between two mates' degrees of freedom, dofB = ratio * dofA +
         //      phase. Omitted when none (docs/assemblies/relations.md).
         // (v82: `finish_tool` + `finish_stock` on a profile or pocket -- rough
         //      with the operation's tool leaving the stock, then one
         //      full-depth pass with the finishing tool. Omitted when none
         //      (docs/cam/operations.md#rough-then-finish).)
         // (v81: `lead_arcs` on a profile -- leave the wall on a tangent arc of
         //      the tool's radius at the end of every pass. Omitted when off
         //      (docs/cam/operations.md#profile).
         // (v80: `corner_relief` on a profile or pocket (dogbone | tbone):
         //      clear the inside corners a round cutter leaves, by the tool's
         //      radius. Omitted when none (docs/cam/operations.md#profile).
         // (v79: `peck` on a drill operation -- retract fully between pecks of
         //      this depth, as plain moves. Omitted when 0. A hole wider than
         //      the tool is bored, which needs no key: the tool decides
         //      (docs/cam/operations.md#drill).
         // (v78: `split_output` on a setup -- one program per tool, which most
         //      hobby controllers need because they cannot take a tool change
         //      mid-file. Forces tool order. Omitted when off
         //      (docs/cam/post.md#files).
         // (v77: a setup EMBEDS its machine. `post`, `fit_arcs` and
         //      `has_rotary_axis` were keys of the setup and every new job
         //      re-answered "which machine" with a combo; they are now a
         //      `machine` block -- name, travel, post, arcs, rotary, rapid,
         //      tool library -- that is a copy of a globally stored profile
         //      named by `name` (~/.config/lobole/machines/<slug>.json), so
         //      the file opens on a machine that has never seen the profile
         //      and the panel says which copy it is using when the two differ
         //      (docs/cam/setup.md#machine-profiles). The three old keys are
         //      still read from a file without the block.
         // (v76: fixturing is per PART. A placement can carry its own `frame`,
         //      so one sheet can hold a baffle laid on its face beside a rail
         //      laid on its edge -- each modelled in its assembly orientation,
         //      all cut from one board. Unset means "as the setup says", which
         //      keeps Setup::frame the single-part answer and re-saves an
         //      existing job byte-identically. Optional rather than defaulted
         //      to identity: a default would silently un-clamp a part the
         //      moment you gave it a position
         //      (docs/cam/setup.md#fixturing-frame-and-rotary-index).
         // (v75: a setup says in what ORDER it cuts (`order`:
         //      authored|tool|part). With one part authoring order IS
         //      cutting order; with four, authoring per part means a tool
         //      change per part per tool. Omitted at `authored`, which is
         //      byte-for-byte what this app posted before -- the bump is for
         //      reading, where an older build would cut a file that says
         //      "tool" in the order it was written, which is a different
         //      program (docs/cam/setup.md#cutting-order-and-split-output).
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
         //      (docs/cam/README.md#the-model).
         // (v73: CAM setups are no longer part of the part. They live in a JOB
         //      document of their own (`kind: "job"` -- a parts list plus the
         //      setups that cut them), which is what makes a job about more
         //      than one part: four brackets out of one sheet used to be
         //      inexpressible (docs/cam/README.md#the-model). BREAKING for any
         //      document carrying `cam`: a part's setups are not migrated,
         //      because the job they belong in is a document that did not
         //      exist. No users, so no migration (CLAUDE.md).
         // (v72: an occurrence can RIDE ON another (`attached_to` +
         //      `attach_offset`/`attach_rotation`), so a fastener travels with
         //      the part it is bolted to and cannot be dragged out of its hole
         //      on its own. Composition, not a mate: no solver sees it
         //      (docs/parts/README.md#attached-and-mated). Omitted when free,
         //      so every existing assembly serializes byte-identically.
         // (v71: a Hole records what it is FOR — `fit: {thread, kind, pitch?}`
         //      — and the diameter is DERIVED from it, so a clearance hole
         //      stops being a number somebody looked up once. Additive and
         //      optional: a hole without one serializes exactly as before
         //      (docs/modeling/holes-and-threads.md#hole).
         // (v70: seven fastener standards instead of one (nuts, washers, hex
         //      and countersunk and button heads, a hexalobular drive), the
         //      drive DEFAULTED to the one its standard is made with rather
         //      than to "hex", and the `fastener_callout` drawing dimension.
         //      All three widen what a loader must ACCEPT — a v69 build reads
         //      "iso4017" as an unknown standard — which is what the bump is
         //      for
//      (docs/parts/README.md#the-model-a-fastener-is-a-part-placed-as-an-occurrence).
// (v69: the Fastener feature — a standard part built from a vendored
//      dimension table (docs/parts/README.md), carrying only what the
//      table cannot give it: which standard, which size, how long
// (v68: cam operation `use_outline` (clear the face's outline,
//      passing over its openings rather than around them);
//      the cam work origin is DERIVED from a named corner
// (`origin_at`) and the post subtracts it; fixed-size stock
// (`stock.mode`/`size`/`place`) — docs/cam/setup.md#work-origin
// (v67: the cam setup's fixture frame + whether the machine has a
// rotary axis (docs/cam/setup.md#fixturing-frame-and-rotary-index))
// (v66: drawing notes; cam tool type/tip angle + the edge-break op)
// (v65: mate connector corner snap (anchor snap_edge2))
// (v64: display units (length/mass/angle) travel with the document)
// (v63: the cam setup remembers the face Z zero was picked from)
// (v62: cam operation `side` written for clearing ops too)
// (v61: expression-driven Box/Sphere/Cylinder dimensions)
// (v57: mate connector inner_path)

// Positions are read and written as doubles: a JSON number is one, and a
// sketch plane at z = 13.4 read through float lands 0.4 um below a face at
// 13.4 (docs/sketching/README.md#plane). A float field takes the narrowing
// where it is assigned.
inline nlohmann::json Vec3ToJson(const glm::dvec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}
inline glm::dvec3 Vec3FromJson(const nlohmann::json& j) {
  return glm::dvec3(j.at(0).get<double>(), j.at(1).get<double>(),
                    j.at(2).get<double>());
}

// Quaternion as [x, y, z, w] (glm stores (w,x,y,z); JSON order matches the
// a2-design occurrence schema).
inline nlohmann::json QuatToJson(const glm::dquat& q) {
  return nlohmann::json::array({q.x, q.y, q.z, q.w});
}
inline glm::dquat QuatFromJson(const nlohmann::json& j) {
  return glm::dquat(j.at(3).get<double>(), j.at(0).get<double>(),
                    j.at(1).get<double>(), j.at(2).get<double>()); // (w,x,y,z)
}

inline nlohmann::json Vec2ToJson(const glm::dvec2& v) {
  return nlohmann::json::array({v.x, v.y});
}
inline glm::dvec2 Vec2FromJson(const nlohmann::json& j) {
  return glm::dvec2(j.at(0).get<double>(), j.at(1).get<double>());
}

inline nlohmann::json ElementRefJson(const ElementRef& ref) {
  return nlohmann::json{{"feature_id", ref.featureId}, {"element", ref.name}};
}
inline ElementRef ElementRefFromJson(const nlohmann::json& j) {
  return ElementRef{j.value("feature_id", uint64_t{0}),
                    j.value("element", std::string{})};
}

// ---- A result's numeric arrays (v154) --------------------------------------
// A study or motion result keeps its metadata in <base>.result.json and its
// numeric arrays in <base>.result.bin beside it
// (docs/simulation/README.md#what-a-saved-result-keeps). In memory the
// bytes ride in the json as a binary value under kResultData, which the
// save splits off and the load puts back, so (de)serializing a result is
// still one json. Each array is named in the json by a reference
// {type, offset, count}: `count` elements of `type` at byte `offset`,
// little-endian. Types: f32 (a field: derived bulk data is float), f64 (a
// placement: the model is double), u16 and u32 (indices; u16 when every
// value fits), i32 (indices that may be -1: a cut's triangles on no face).
inline constexpr const char* kResultData = "data";

class ResultArrayWriter {
public:
  nlohmann::json F32(const std::vector<double>& v);
  nlohmann::json F32(const std::vector<glm::dvec3>& v); // x, y, z each
  nlohmann::json F64(const std::vector<double>& v);
  // Indices: u16, u32, or i32 when one is negative.
  nlohmann::json Index(const std::vector<int>& v);
  // The bytes, as the binary value the json carries under kResultData.
  nlohmann::json Take();

private:
  nlohmann::json Append(const char* type, const void* data, size_t count,
                        size_t size);
  std::vector<std::uint8_t> m_bytes;
};

class ResultArrayReader {
public:
  // `owner` is the result's json: its kResultData binary. `what` names the
  // result in a refusal ("static result").
  ResultArrayReader(const nlohmann::json& owner, std::string what);
  // `ref` must be a reference of the matching kind; `name` is the field,
  // for the refusal. Reals reads f32 or f64; Indices u16, u32 or i32.
  std::vector<double> Reals(const nlohmann::json& ref, const char* name) const;
  std::vector<glm::dvec3> Vec3s(const nlohmann::json& ref,
                                const char* name) const;
  std::vector<int> Indices(const nlohmann::json& ref, const char* name) const;

private:
  const std::uint8_t* Locate(const nlohmann::json& ref, const char* name,
                             std::string& type, size_t& count) const;
  const std::vector<std::uint8_t>* m_bytes = nullptr;
  std::string m_what;
};

// A drawing view's saved projection (v162,
// docs/drawings/README.md#saved-projections) as one json whose arrays ride
// under kResultData, like a study's result: the save splits them into
// <id>-<slug>.projection.bin. FromJson throws on a malformed one; the
// loader drops it with a warning rather than refusing the drawing.
nlohmann::json ViewProjectionToJson(const drawing::ViewProjection& p);
drawing::ViewProjection ViewProjectionFromJson(const nlohmann::json& j,
                                               const std::string& what);
