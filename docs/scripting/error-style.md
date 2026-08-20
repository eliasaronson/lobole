# Error style — decision-complete outcomes

The standard every RPC error is held to (error-audit round, 2026-08-18;
the idea is 10-X-eng's "decision-complete tool outcomes" from the AI-CAD
survey). An agent reads an error ONCE and must be able to act — every
extra round-trip an error forces is a full turn wasted.

A failed call must carry, in one plain-language sentence each:

1. **The measured condition** that failed — not "out of range" but
   "index 99 out of range — there are 3 features (0..2)". Not "no such
   sketch" but "no sketch with id 9999".
2. **The vocabulary to correct it** — the candidates that DO exist
   ("Sketches here: 5 (Outline), 12 (Profile)"), or the accepted values
   ("views: front, back, left, right, top, bottom, iso"), capped at ~10
   with "+N more". The FIRST rejection carries the complete correct
   template; never reveal one constraint per round-trip.
3. **The remedy or discovery call** when candidates don't fit inline —
   "switch tabs with set_active_part (see list_parts)", "get_state lists
   bodies".
4. **What state survived** when it matters — "the document is unchanged".

Banned patterns:
- "(see log)" — the agent cannot read the log. Name the usual cause and
  the check to run instead ("an OpenCASCADE boolean threw, usually on an
  invalid or empty body; run check_geometry").
- Bare identity errors ("no such X", "X failed") with no given value and
  no candidates.
- Collapsed multi-cause errors ("unknown, invalid, or taken") without
  the inputs echoed back.

Helpers: `remote::KnownList` (capped candidate formatting) and
`remote::IndexError` (ranged index errors) in RemoteHelpers.h.
Enforcement: papercuts_smoke asserts the shape of representative errors;
when you improve one, pin it there.
