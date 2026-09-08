# OM615 front accessories — measured model and iteration results

The front drive now has a continuous three-pulley V-belt, a four-blade pitched
steel fan with bolted hub flanges, an alternator with separate fixed and rotating
subassemblies, and an offset hollow vacuum-pump casting. The pump has mounting
and cover screws, a rocker pivot, roller and axle, diaphragm centre/rod, and a
two-lobe axial face cam. The timing cover includes its open mounting register.
All accessory motion is coupled to the engine's one driving crank DOF.

## Sources and limits

The supplied Mercedes workshop manual, *Service Manual Engines 615, 616,
617.91*, operation 13-335 (PDF page 450), lists 9.5 and 12.5 mm V-belt sections.
This model selects 9.5 mm; the resulting 811.42 mm pitch length is **derived from
estimated pulley geometry**, not an identified Mercedes belt part number.
Operation 05-412 (PDF pages 298–300) shows the intermediate shaft and injection
timer arrangement. The supplied engine, alternator and vacuum-pump photographs
provide envelope and construction references. They are not calibrated drawings.

Pulley centres/diameters, fan dimensions, pump casting stations, cam law and
2.4 mm diaphragm stroke are engineering estimates. This is a clearance-checked
kinematic model, not OEM manufacturing geometry. In particular:

- The belt occupies its running envelope; individual belt material points do not
  circulate in the animation. Its pulley ratios use pitch radii and assume no slip.
- The intermediate timer drive is a half-speed kinematic relation. The cam now
  has an integral shaft supported by a flanged bush in the timing-cover pocket.
  Its intermediate sprocket and the rerouted timing chain remain to be built.
- The face cam is a faceted inverse roller envelope with 0.5 mm modeling
  allowance. It demonstrates the linkage motion; it is not a production cam
  grind, contact-force solution, or wear model.
- The diaphragm rim is fixed and its centre translates. The flexible membrane,
  return spring, check valves and pneumatic action are not simulated. The roller
  moves with its yoke; its independent bearing rotation is not simulated.
- The alternator now has bolted lower and upper supports, an arcuate adjustment
  slot, and twelve copper stator winding packs in matching slots. The packs
  represent bundles rather than individual insulated wire turns. Electrical
  terminals and detailed rotor field windings remain absent.
- The original engine's broad casting shapes still need refinement. Adding these
  accessories does not establish the accuracy of the rest of the engine.

## Assembly and SDK choices

Nested assemblies are rigid in the current solver. The alternator therefore
uses a **stator assembly** and a **rotor assembly**, connected by a revolute in
the parent engine. This exposes a real turning DOF without pretending that a
rigid child assembly supports internal motion. Flexible subassemblies with
exported shaft connectors would make this substantially more ergonomic.

The standing sweep checks top-level occurrence pairs. It cannot certify parts
inside the same rigid child, so `check_subassemblies` also checks each alternator
child internally during local/full validation.

The `belt`, `fan` and `vacuum-pump` component keys support isolated generation and
owned staged updates. `vacuum-pump` denotes the casting, not the entire mechanism.
Mounting datums belong in the part builders, including the block and timing
cover builders. Adding them later during assembly broke the ownership model:
rebuilding the supposedly complete part could omit its assembly interfaces.
Shared accessory mounting inputs participate in the assembly fingerprint.

```sh
# Start a private instance; use its port, never the human's port 8799.
python3 generators/om615.py --port PORT --component fan --out /tmp/fan
python3 generators/om615.py --port PORT --project /tmp/engine/om615.dsgn \
  --update-component fan --stage preview --out /tmp/fan-update
python3 generators/om615.py --port PORT --stage full --out /tmp/engine
```

Preview checks strict solids and feature status, but does not run collisions.
Local adds standing checks. Full is the release gate, including exact sampled
motion. It is not continuous collision detection. Keep the default 49 samples:
in a diagnostic lower-engine fixture, 90-degree pose jumps selected a different
slider-crank branch; 15-degree steps retained the intended branch and passed.

## Original front-drive measurements, 2026-09-08

Final engine: **97 parts, 284 top-level occurrences, one driving DOF**. All strict
part checks passed. The two alternator subassemblies passed internally.

| Check or phase | Measured result |
| --- | --- |
| Full generation | 86.27 s |
| Front/rear generation, including accessories | 18.51 s |
| Standing exact sweep | 890 checked, 39,296 bounds-cleared, zero clashes; 16.19 s |
| Motion, 49 poses over 720 degrees | 21,322 exact pair checks, zero clashes; 226.23 s |
| Animation export | 284 nodes × 49 keyframes, 20.6 MiB; 2.44 s |
| Total including validation/export | 336.10 s |
| Alternator, two crank turns | 3.294118 turns, expected 2 × 56/34 |
| Fan, two crank turns | 2.545455 turns, expected 2 × 56/44 |
| Vacuum timer, two crank turns | 1.000000 turns |
| Diaphragm centre travel | 2.399994 mm |

Of the 21,322 motion checks, 5,513 reused cached exact contact results and
15,809 executed narrow-phase work.

The existing four 83.6 mm piston strokes, eight 8 mm valve lifts and 1-3-4-2
firing order still pass. Rotation/travel gates read occurrence poses, rather
than merely asserting relation definitions.

The new `om615_accessories_smoke` regression builds real accessory B-Reps and
checks belt tangency, strict validity, and a staged fan material change:

| Iteration operation | Measured result |
| --- | --- |
| Isolated belt + fan + vacuum parts | 26.61 s |
| Staged fan recolour | 0.239 s; native `geometry_reused == true` |
| Unchanged fan | 0.007 s; builder was not called |

The recolour retains part, feature and occurrence identities and makes the old
validation report stale. Its changed fingerprint still runs the fan builder in
staging; apply adopts that staged B-Rep without regenerating the target again.
Only the unchanged-fingerprint path skips the builder entirely. Preview remains explicitly unchecked for motion.
These timings are single runs on this workstation, not controlled cross-version
speedup claims. Earlier full-engine studies used fewer parts and checks.

## Friction found

1. A closed periodic pipe returned positive volume and no feature error, but
   OCC marked three faces unorientable. `Part.check()` alone did not catch it.
   The accessory builders now require strict `check_geometry()` results; the
   cam uses lofted sectors. A future SDK strict-check convenience would help.
2. A seam-only union could lose belt material. Tangent overlaps plus an analytic
   volume check now guard the complete belt, not just its bounding box.
3. A cam made from roller-centre heights interfered with the finite roller.
   The new inverse envelope accounts for its radius, width and prescribed yoke
   poses. No collision pair is excluded to make this pass.
4. Subassembly connector/DOF exposure and nested validation coverage require
   explicit workarounds described above.
5. Exact full motion still dominates release time. Staging makes a fan iteration
   subsecond; running a whole engine sweep after every shape attempt would erase
   that benefit. No speculative kernel/GPU optimization was added in this pass.

Review artifacts from the measured run live in `/tmp/om615-front-drive/`:
`om615.dsgn`, `om615.glb`, validation and timing JSON, front/three-quarter images,
and pump interior/exterior views. The interior view removes the timer cam and
covers to expose the yoke; it is an inspection view, not a second mechanism.

After source formatting, the archived project's generator provenance stamps
were refreshed to the final source fingerprints. Every feature, B-Rep and
assembly file remained byte-identical; `provenance-refresh.json` records this.

The saved engine was reopened through the normal CLI and `--update-component fan --stage preview` reported `reuse`; preview passed with motion unchecked.

The headless suite initially passed 1,146/1,147 tests. `motion_smoke` polled
`has_result` after starting an asynchronous rerun, which could return the prior
synchronous result before the new job completed. The test now waits for both
`has_result` and `not running`; the registry assertion remains in place.
The corrected motion smoke test passed 20 consecutive runs; the accessory test
also passed again with an explicit assertion that the new material landed.

## Mounting correction after visual review, 2026-09-08

The previous pump flange touched the timing cover, but only a 7 mm ring hung
from its side. Its rear opening was exposed. The alternator bracket also touched
its lower lug but had no pivot hardware or upper support. These were incomplete
connections despite the passing interference reports.

The correction adds:

- A hollow lofted side pocket, fused into the timing cover, with a shared core
  opening into the main chain chamber. A timer shaft and flanged bearing bush
  now extend into this pocket. The six pump flange screws clamp to the pocket.
- Lower and upper alternator brackets, four M8 block attachment screws and
  washers, two M10 bolt/washer/nut stacks, a cast upper block boss and an
  adjustment slot concentric with the lower pivot.
- Twelve copper winding packs, corresponding stator slots, and rear-casting
  clearance for the winding end turns. The stator subassembly has 15 leaves.

The new gate checks 28 actual contacting pairs: flange/case, bush/case,
block/brackets, brackets/casting, bolt heads/washers, washers/supports and
nuts/washers. It also measures the timing-cover side section through the pump
centre: **1,930.15 mm², one connected region**, rather than accepting a thin
flange ring. This checks geometry and mounting continuity, not bolt preload,
thread strength, bearing life or casting-process suitability.

`om615_mounting_smoke` passes the real lower-engine fixture and proves that a
missing pivot bolt and the former thin-ring design are refused. Both this test
and `om615_accessories_smoke` passed. The complete engine remains the motion
gate because the lower fixture omits the chain guide: the initial new pocket
interfered with that guide, and extending the common chain-case core corrected
it. No collision pair was excluded.

Final assembly: **104 parts, 302 top-level occurrences**.

| Gate | Result |
| --- | --- |
| Full build before the local cover correction | 115.1 s |
| Exact standing sweep after correction | 947 candidate checks, zero clashes; 17.29 s |
| Exact motion, 49 poses over 720° | 21,910 checks, zero clashes; 229.50 s |
| Motion cache on that run | 5,513 reused, 16,397 executed |
| Immediate repeat in the same headless session | 21,910 reused, zero executed; 6.38 s wall clock |
| Explicit mounting contact measurements | All 28 passed; 7.32 s |
| Staged cover apply | Native `geometry_reused: true`, 26 cached features, 5 datum executions |

The cover correction used `--project ... --update-component timing-cover
--stage full`; it did not rebuild the rest of the engine. Preview still skips
standing and mounting contact work. Local/full perform both.

Artifacts are in `/tmp/om615-mounted-drive/`: accepted `om615.dsgn`, updated
`om615.glb`, mounting and validation JSON, and mounting/winding close-ups.
The export was reproduced in a disposable renderer copy and compared with the
saved exact motion poses: zero positional difference; quaternion agreement
within floating-point rounding. The accepted project's study remains exact and
collision-enabled. `animation-verification.json` records that comparison and
the four measured piston strokes.

## Connected mounting supports

A later visual review exposed a false positive in the mounting checks: each
alternator bracket was one valid CAD body containing **two solids**. The lower
lug-seating cutter crossed the support arm; the upper slot crossed both strap
edges. Separate islands could touch the block and alternator and pass both
pairwise contact tests. Those tests did not establish a continuous load path.

The lower seating cut now starts at the lug edge, preserving the arm. The upper
strap encloses both ends of its adjustment slot. Both builders require
`part.check(solids=1)`. This optional SDK check counts solids across non-reference
bodies and reports body names and feature IDs on failure; it does not replace
strict B-Rep validity checks or assume all CAD parts must contain one solid.

`check_mounting_structure` runs during engine preview as well as local/full
validation. It checks both brackets' solid count, sampled material sections,
foot-hole axes against mounting datums, bolt seating/alignment, block screw
engagement (8–15 mm), and full axial engagement of each pivot nut. The sampled
section minima are 80 mm² for the lower support and 24 mm² per upper-strap
section region. These are model design guards, not load-derived strength limits
or a global minimum-wall measurement. Nut engagement uses geometric extents;
thread preload, thread stripping and fatigue are not evaluated.

The preview gates do not run the standing collision sweep or motion study.
Local/full still add the existing 28 contact checks, enclosed-pump-pocket check,
child-assembly sweeps and whole-engine clearance checks. The mounting regression
also deliberately severs an arm, leaves a thin connected bridge, and shortens a
block screw, requiring each defect to be rejected for its specific reason.

On the corrected 104-part / 302-occurrence engine, the preview mounting checks
measured 1.25 s (12 section samples and six bolt engagement checks). The model
built in 107.9 s. At world Y=-240 the repaired lower arm has a 217.38 mm² section;
the previously cut arm had no section there. Block screws engage 13.4 mm and
both pivot bolts engage the full 8.4 mm nut thickness. A rendered rear/side view
was inspected to confirm the lower arm reaches the eye and the upper slot is
enclosed.
