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
- The intermediate timer drive is a half-speed kinematic relation. A physical
  intermediate shaft, its bearings and a rerouted timing chain remain to be built.
- The face cam is a faceted inverse roller envelope with 0.5 mm modeling
  allowance. It demonstrates the linkage motion; it is not a production cam
  grind, contact-force solution, or wear model.
- The diaphragm rim is fixed and its centre translates. The flexible membrane,
  return spring, check valves and pneumatic action are not simulated. The roller
  moves with its yoke; its independent bearing rotation is not simulated.
- The alternator has a bored mounting bracket and cast lugs. Its tension-adjusting
  strap, pivot hardware, electrical terminals and detailed windings remain absent.
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

## Measurements, 2026-09-08

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
