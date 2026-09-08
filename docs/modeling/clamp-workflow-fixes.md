# Clamp workflow fixes

Implement in this order, with regression gates and conventional commits per stage.

1. Motion diagnostics: preserve unknown exact-contact pairs per sample; expose
   occurrence identities and reasons in JSON and the text table; distinguish
   errors, cancellation, unchecked collision, unknown collision and actual clashes.
2. Drawing/SDK ergonomics: typed projected references and persistent datum-plane
   helpers; expression-backed notes resolved identically by UI/export/validation;
   reject unresolved notes and report note overlaps/bounds using shared layout.
3. Threaded interfaces: retain manufacturing tap-drill geometry and authoritative
   thread designations; derive screw pitch from that specification; introduce
   explicit, bounded engagement checks rather than suppressing entire part pairs.
   A wrong axis, insufficient engagement or non-thread collision must still fail.
4. Migrate the clamp, verify edits and exports, rerun the regressions and visually
   inspect the final drawings. Record remaining limitations and measured timings.

Avoid broad collision exclusions and automatic retries that turn failed kernel
checks into passes. Preserve the user's open document and unrelated render.png.
