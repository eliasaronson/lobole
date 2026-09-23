# Name the mate whose axes are wrong

## Problem

A revolute (or cylindrical, or slider) mate whose two connectors are nowhere
near coaxial turns its part to satisfy itself. The error then surfaces
somewhere else: a loop three joints away fails to close ("loop does not close
(off by 44 mm)") and the message names that loop's mate, not the one that
was wrong.

The solve cannot tell. Every `add_mate` solves once, so by the time any later
solve looks, the part has already been turned into alignment and the axis
angle reads zero. The place a hint belongs in spirit — where
`src/core/AssemblySolve.cpp` reports an unclosed loop — records this in a
comment for that reason.

## Fix

Measure at mate **creation**, before `add_mate`'s own solve, where the
caller's placement is still what it was — the same hook where `add_relation`
already warns about gear axles more than 5 degrees off parallel:

1. In the `add_mate` handler, for the axis-carrying mate types, resolve both
   connectors' world frames with the occurrences at their current placements
   and compute the angle between the axes and the axis-to-axis offset.
2. Above a threshold derived from the parts' sizes (not a fixed millimetre
   value), add a `warning` to the response naming the mate, the angle and the
   offset, and store it on the mate so `list_mates` shows it and a later
   loop-closure error can say "check mate N, which was created N° off axis".
3. Never refuse: a mate made first and positioned by the solve is a
   legitimate way to assemble.

## Files

`src/remote/RemoteAssembly.cpp` (the `add_mate` handler and its `describe`
row), `src/core/AssemblySolve.cpp` (the loop-closure message),
`src/core/Occurrence.h` if the warning is stored on the mate,
`tests/assembly_mates_smoke.py`.

## Verification

- A smoke builds a three-mate loop with one revolute's connectors 30° off
  coaxial: `add_mate` warns naming that mate, and the loop-closure error
  names it too.
- A correctly placed loop produces no warning, and the threshold does not
  fire on a small part or a large one built correctly (a screw and a frame).
