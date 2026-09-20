# The setup frame — fixturing is not a 4th axis (S1–S7)

A `front_wall` from `smart_speaker.dsgn` is a 210 × 1122 mm panel, 16 mm
thick **in Y**: it stands upright in the world, exactly as it does in the
assembled cabinet. `Pick top face` on its baffle face refuses —

    pick a level face — a tilted one has no single depth

and the refusal is correct. Of the body's 54 faces only 4 have
`|normal.z| ≥ 0.99`, and they are the 3360 mm² end caps: level, and useless
to touch off on. The two faces anyone would machine are 175 377 mm² and
225 108 mm², normal `(0, ±1, 0)`.

The only way past it today is `Turn a face up`, which sets `Setup::rotationA`
— and `CamPost.cpp:461` then emits `G0 A90`. A plywood baffle on a 3-axis
router gets a file that commands a rotary table it does not have. The other
way past it is to model every part face-up, and a second copy of the part for
every side you intend to cut. Both are wrong for the same reason: **the CAM
job is being made to compensate for a coordinate system the part had no
business changing.**

## What went wrong

`docs/cam/plan.md:174` committed to the right shape:

> **The design commits to this shape now** (frame-per-setup, toolpaths
> generated in setup space, post takes the frame)

C9 shipped the *toolpaths in setup space* half — and it held up well; every
consumer funnels through two functions (`BodiesInSetupFrame`,
`FromSetupSpace`), so sectioning, face regions, stock bounds, playback and
the verifier all already work in whatever frame they are given. What C9 did
not ship is the **frame**. It stored one number instead:

```cpp
// CamJob.h:168
double rotationA = 0.0;
```

and that number is answering two unrelated questions at once:

| question | whose statement | should post |
|---|---|---|
| How is the part clamped on the bed? | fixturing — a CAD fact | nothing |
| What angle must the trunnion move to? | the machine | `G0 A90` |

Everything the user hit follows from the conflation. `BodiesInSetupFrame`
(`CamToolpath.cpp:1188`) builds only a rotation about X, because it was
written to model a trunnion rather than a fixture. `SolveIndexA`
(`CamToolpath.cpp:1211`) refuses any face with an X component — true of a
trunnion, absurd of a clamp. And `Move::a` is set from `rotationA` at
`CamToolpath.cpp:1804`, so choosing an orientation silently chooses a machine.

## The fix

Split the two. Keep `rotationA` for the rotary index it is named after; add
the frame the plan asked for.

```cpp
struct Setup {
  // How the part sits on the bed: the rotation taking SETUP space to MODEL
  // space. A pure fixturing statement — it says nothing about the machine
  // having a 4th axis, and posts nothing.
  glm::dquat frame{1.0, 0.0, 0.0, 0.0};

  // The MACHINE's rotary index, in degrees about X, emitted as an A word.
  // 0 for every 3-axis job — including a job whose frame is rotated.
  double rotationA = 0.0;

  // Whether the machine HAS an A axis. Without it, `rotationA` is not an
  // option the panel offers, and a file can never command one.
  bool hasRotaryAxis = false;
};
```

> `hasRotaryAxis` moved to `Machine` at v77, when the machine became one
> object a setup embeds a copy of (`job-document.md` M4). It is a claim about
> the shop's machine, not about this setup, so that is where it belongs —
> and naming a machine now brings it, which is the point of the move. Read
> every `Setup::hasRotaryAxis` below as `Setup::machine.hasRotaryAxis`.

`SetupFrame(setup)` composes the two into one transform; `BodiesInSetupFrame`
applies its inverse. Because every consumer already goes through those, the
change is one function deep and the 2.5D pipeline is untouched.

### Decisions

**Composition order.** The fixture is a rotation of the part on the table;
the index is a rotation *of the table*. Setup → model is `frame · R_x(A)`,
matching `FromSetupSpace`'s existing direction. Rather than argue the matrix
in prose, S6 pins it with a test: a setup whose **frame** is 90° about X must
produce a byte-identical toolpath to today's setup at `rotationA = 90`,
differing only in whether an `A` word is posted. If the order is wrong that
test fails, and it is the same equivalence `plan.md:480` already asks for.

**The leftover degree of freedom.** A face normal fixes two of three
rotational degrees; the spin about the spindle is still free. Take the
**shortest arc from the setup's current frame**, so clicking a face changes
as little as possible and clicking the same face twice is idempotent — then
offer a `Rotate 90°` nudge for when the long axis wants to lie along X. This
is the defaults rule: derived from the face and the frame in hand, never a
literal.

**Rotary stays honest.** `hasRotaryAxis` defaults **false**. `Turn a face up`
and the `A index` field appear only when it is set, and the refusal when it
is not names what to do instead — lay the face up, which is now a real
option. A 3-axis shop can no longer be handed A words by accident.

**One file per setup.** `PostProcess` (`CamPost.cpp:363`) takes one setup and
keeps doing so. A flip job means unclamping the part, so a file boundary is
the honest place for that break; `plan.md`'s multi-setup single file with
`M0` between remains deferred, and is not needed for any of this.

## Phases

### S1 — the frame, as data (~1 session)

`frame` + `hasRotaryAxis` on `Setup`. New `SetupFrame(setup)` returning the
composed transform. Rewrite `BodiesInSetupFrame` (`CamToolpath.cpp:1186`) and
`FromSetupSpace` (`:577`) in terms of it, deleting the hand-built X-rotation
matrices in both. Nothing else in `src/core/cam/` should need an edit — if
something does, that is a consumer that skipped the two funnels and the plan
wants to know about it.

*Gate:* the existing `rotationA` tests (`test_cam.cpp:1548`, `:1624`) pass
**unchanged**. That is the whole point of doing this first.

### S2 — lay this face up (~½ session)

`SolveFaceUp(normal, currentFrame) -> dquat`, shortest-arc, no refusal for
faces with an X component. `CamFacePicked`'s `CamIndexFace` branch
(`CamTool.cpp:287`) splits in two: **Lay this face up** writes `frame`,
**Turn a face up** writes `rotationA` and only exists when `hasRotaryAxis`.
`SolveIndexA` stays exactly as it is, for the branch that still means a
trunnion.

*Gate:* every planar face of `front_wall` solves, including the two that used
to be refused outright.

### S3 — the post stops inventing an axis (~small)

`Move::a` set only when `hasRotaryAxis && rotationA != 0` (`CamToolpath.cpp:1804`);
`CamPost.cpp:461` gated the same way. Mostly already true — the bug was that
the *pick* wrote `rotationA`. The value of doing it explicitly is the test.

*Gate:* a setup with a rotated frame and no rotary posts **zero** `A` words,
in both dialects.

> **Still true, and now one of two things a setup says about position.**
> Since v73 a setup also carries `placements`: where each part sits in the
> stock, in-plane (a position and a spin about Z). The split holds — `frame`
> is how a part is CLAMPED, `rotationA` is what the MACHINE does, and a
> placement is where on the bed — and in-plane is deliberate: anything out of
> plane is what `frame` already says, and two ways to tilt a part is this
> document's own lesson. A job whose parts need DIFFERENT fixturing IS
> expressible since v76: a placement can carry its OWN `frame`, and a part
> without one is clamped the way the setup says. `Setup::frame` is unchanged
> and is still the whole answer for a one-part job — which is every job this
> document was written about ([job-document.md](job-document.md)).

### S4 — many setups, one part (~1 session)

`Document` already holds `std::vector<cam::Setup>` with add/find/remove
(`Document.h:169-174`); the panel only ever drives one. Add setup
add/rename/switch to the CAM panel, and **Flip** — a new setup whose frame is
the active one turned 180°, which is the second side of a flip job derived
rather than typed. Export names the file per setup.

*Gate:* one part, two setups, two frames, two files, no copies of the part.

### S5 — serialization and the version gate (~small)

`frame` written only when it is not identity and `has_rotary_axis` only when
true, so a plain 3-axis job re-saves byte-identically — the same rule
`SerializeCam.cpp:108` already applies to `rotation_a`. Reuse
`QuatToJson`/`QuatFromJson`. Then, per CLAUDE.md: **kVersion 66 → 67**, a
golden at v67, *and* a row in `docs/versioning/format.md`'s history table.
Both are gated by `test_golden.cpp`.

### S6 — the gates (~½ session)

Headless (`tests/test_cam.cpp`):
- frame ≡ rotationA equivalence, per the decision above.
- a framed job verifies: `CamVerify.cpp:528` already frames its bodies, so
  this should pass on arrival — and if it does not, S1 got the order wrong.
- `SolveFaceUp` idempotence: solving for a face already up is identity.

GUI (`src/app/guitests/GuiTestsCam.cpp`): `:322` sets `rotationA` directly —
give it a `frame` twin. Then the regression that started this: **pick the big
face of a vertical panel and get an operation**, which is the one case no
existing test covers because every fixture part is modelled face-up.

Dogfood: `front_wall` from `~/Documents/smart_speaker.dsgn`, the real part.

### S7 — the docs that are now wrong (~small)

- `plan.md`'s C9 as-built note says the design shape held. It half-held, and
  the half that did not is why this document exists.
- `gui.md:129` lists "a second setup for flip jobs" as *later*; S4 lands it.
- `dogfood-round.md:317` tells the operator the file is in **model**
  coordinates. With a frame it is in **setup-frame** coordinates, and that is
  the number they check before the first plunge — it must not stay stale.

## Not this

**Continuous 4-axis** (simultaneous XYZA) is untouched and stays deferred for
the reasons `plan.md:177` gives. **Work offsets** (G54…) stay out: the file
remains in setup-frame coordinates with the operator setting the offset, per
the existing documented convention — S7 only corrects which frame that
sentence names.

## As built

All seven phases shipped, 2026-08-30, format **v67**.

**S1 found a fourth copy of the transform.** The plan said an edit outside the
two funnels would mean a consumer that skipped them, and there was one:
`CamPlayback.cpp` built its own `SetupToModel` mat4 from `rotationA` for the
renderers. It now converts `cam::SetupToModel`, and the three hand-rolled
X-rotation matrices are one function.

> Superseded at v73/J6: a job tab draws in SETUP space, so `CamPlayback.cpp`
> converts nothing at all now and `FromSetupSpace` was deleted outright
> (`CamToolpath.h`). `SetupToModel` survives in `CamSetup.cpp` and is reached
> only from `BodiesInSetupFrame`. The lesson held; the file named did not.

**The composition order was right, and is now pinned.** `frame · R_x(A)`:
`cam: a fixture frame cuts what a rotary index would` generates the same
toolpath from a 90° frame and from `rotationA = 90` and compares every move.

**S3 changed behaviour, and two existing tests said so.** Declaring
`hasRotaryAxis` is now required for an A word, so the C9 tests that set only
`rotationA = 90` stopped posting one. Updating them was the point rather than
a cost: a rotary job must state that it has a rotary.

**A stale cache the plan did not predict.** `CamTool.cpp`'s region cache keyed
on `documentVersion / setupId / rotationA / ref` — "everything the answer
depends on", as its comment claimed. Laying a face up re-sections every body
and moved none of those, so the ghost and hover preview served a region from
the previous orientation. The key now includes `frame`.

**Deserialising an old rotary job.** `has_rotary_axis` defaults to
`rotation_a != 0` rather than to false, so a pre-v67 file that indexed a table
keeps posting its A word. Defaulting to false would have silently dropped it.

**The GUI gate is the case the whole suite dodged.** Every CAM fixture in
`GuiTestsCam.cpp` was modelled face-up, which is exactly why nothing caught
this. `cam/upright_panel_lays_flat` builds a 210 × 16 × 400 panel standing on
edge, checks that Z zero refuses its big face, lays that face up, and checks
the same face then takes a Z zero, an operation, and a toolpath with no A word
in it.
