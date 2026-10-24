# Keep out of a batch what its rollback cannot undo

## The problem

A `batch` promises that a call which fails rolls everything back, leaving the
document as it was (docs/scripting/rpc.md#a-refused-call-changes-nothing).
The rollback now covers the part's own commands, the project's variables,
tables and templates, and edits to other part tabs. Two kinds of call are
still beyond it, so a batch that fails after one of them leaves half its work
behind and says nothing changed:

- **Calls that create or switch tabs.** After one, the marked tab's history
  is stashed where the rollback does not unwind it, and the new tab stays:
  `add_part`, `add_assembly`, `create_drawing`, `machine_part`,
  `claim_component`, `update_component` with `apply`, `edit_in_context`, the
  CAM verbs given a `job`, the drawing verbs given a `drawing`, and
  `add_fastener`, `add_bearing`, `add_retaining_ring` and their `fit_*`
  forms, which create the standard part's tab and folder.
- **Edits that have no undo history at all**: `set_units`,
  `set_tube_rules`, `set_rollback` (which is saved), `rename_part`, the folder
  verbs (`create_folder`, `move_to_folder`, `rename_folder`,
  `dissolve_folder`), and a part-scope `rename_variable`, whose rewrite of the
  requirements runs outside its command.

`remote_add` and `remote_remove` write the git config, which is outside the
document altogether, and are not flagged either.

Nothing in the GUI uses `batch`; it exists for scripts and agents, which rely
on it being all or nothing.

## The shape of the fix

Every call above gets `NotInBatch` in its `MethodDoc` row, the flag that
already keeps undo, redo and the file verbs out of a batch. A batch naming one
is refused before its first call runs, naming the call and saying to make it
on its own, before or after the batch. A script that builds a part calls
`add_part` and then batches the edits.

A CAM or drawing verb is `NotInBatch` only when it is given a `job` or
`drawing` other than the active one; the same verb on the active tab stays
batchable. That needs the gate to look at the parameter, so the flag's check
(the `NotInBatch` check in `RemoteDispatch.cpp`'s batch handler) gains a per-row
predicate for these rows rather than a second flag.

Giving the no-history edits real undo is not part of this: other CAD programs
often do not undo document settings either, and the flag closes the hole a
batch has. If their missing undo is felt in the GUI, that is its own plan.

## Files

- `src/remote/Remote{Project,Modeling,Assembly,Drawing,Cam,…}.cpp`: the
  `NotInBatch` flags in each domain's `MethodDoc` rows.
- `src/remote/RemoteDispatch.cpp`: the per-row predicate for the CAM and
  drawing verbs.
- `python/lobole/rpc.py`, `api/rpc-surface.json`: regenerated.
- `docs/scripting/rpc.md#batch`: what a batch refuses, and why.

## Verification

- `tests/batch_smoke.py`: a batch naming each flagged call is refused before
  anything runs (revision unchanged), naming the call; the CAM and drawing
  verbs are refused with another `job`/`drawing` and accepted on the active
  one.
- `refused_calls_smoke` already checks that a `NotInBatch` refusal changes
  nothing; it covers the new rows without a new case.
- An audit test: every method whose handler switches or creates a tab, or
  writes outside the undo histories, carries the flag. It lists those
  handlers by what they call (the tab-switching and folder functions), so a
  new one cannot arrive unflagged; it fails on a planted example.
- The headless suite and `tools/ctest -L pygui`.
