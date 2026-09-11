# SDK angles name their unit

## The problem

The units round (docs/units-plan.md) made every RPC parameter that carries an
angle say which unit it takes. The SDK's hand-written layer did not follow:
the generated surface passes RPC names through, but the compositions a script
author actually types have their own keywords, and a bare angle meant three
things there.

- `revolve(angle=)`, `revolve_profile(angle=)`, `pattern(angle=)`,
  `frame(rotate=)` — degrees.
- `bolt_circle(start_angle=)`, `polygon(rotate=)` — radians.
- `arc(start_deg=, end_deg=)` — named.

The app's guard does not cover this. It refuses a bare radians value over a
full turn as "probably degrees", so a degrees number of 6 or more is caught —
but a 5° value read as radians is 286° and passes. Naming the unit is the
defence; the guard only catches the large half.

Two smaller defects sit beside it:

- `pattern`'s `angle` defaults to 360 on a PER-STEP field, so the default
  rotates every copy a full turn back onto the original.
- `Sketch.constrain(value=Param)` sends the number and drops the expression,
  so the constraint stops being parametric. For an `angle` constraint it is
  worse: the number is radians and the expression would evaluate in degrees.

## What to build

All in `tools/designer_sdk.py`.

1. **Every hand-written angle keyword ends `_deg`**: `revolve(angle_deg=)`,
   `revolve_profile(angle_deg=)`, `pattern(step_deg=)`,
   `bolt_circle(start_deg=)`, `polygon(rotate_deg=)`, `frame(rotate_deg=)`.
   The old spelling is REFUSED, naming the new one and saying where the unit
   changed — never accepted, because `bolt_circle(start_angle=0.5)` quietly
   read as degrees is a plausible wrong part. One decorator, `_renamed`, does
   the refusing.
2. **`pattern(step_deg=None)` derives its default from `count`**: 360/count,
   the copies evenly round a full turn.
3. **`constrain` routes a `Param`**: as `value_deg`, or as `value` on a length
   kind, it sends the number AND its expression, whose bases agree. As `value`
   on an `angle` constraint it is refused, naming `value_deg`.

Callers: three om615 generator calls pass `angle=` to `revolve_profile`; none
passes the other old names.

## Verification

`tests/sdk_smoke.py`:

- A signature gate: every public function on an SDK class whose parameter
  names an angle (`angle`, `rotate`, `tilt`, `phase`, …) must end `_deg` or
  `_rad` — the SDK-side twin of `tests/rpc_units_gate.py`, so the next helper
  is caught rather than only these six. It first proves it flags a bare
  `angle`, so it cannot pass by scanning nothing.
- Each old spelling is refused naming its replacement.
- `bolt_circle(start_deg=45)` puts a hole at 45°.
- A circular pattern with no step spreads four copies round the axis.
- `constrain(value=Param)` follows its variable when the variable changes.
