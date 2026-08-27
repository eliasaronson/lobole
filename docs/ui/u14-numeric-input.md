# U14 — One way to enter a number, and it accepts expressions (2026-08-27)

> Status: **Phases 0, 1 and 2 SHIPPED 2026-08-27.** Findings 1-4 are closed
> and gated (four GUI tests plus a source scan); 879 headless and 229 GUI
> tests pass. Phase 0 is what turned this from "add autocomplete" into "the
> feature is dead". Phase 3 (completion) remains — read "What changed on
> contact" at the end before trusting the phase sections, which predicted
> the wrong risk twice and undercounted the raw fields.

The question that started this was small — *is there a common interface for
entering numbers, and could it tab-complete the document's variables?* The
answer to the first half is "two of them, covering 60% of the fields". The
answer to the second half turned out to be blocked on something worse: on
every field that does route through the shared widget, **an expression that
names a variable is silently discarded**.

The variables workstream's own plan promised
"[expressions everywhere a number lives](../variables/plan.md)". This is the
audit of how far that actually reached.

## The three ways a number is entered today

| layer | where | expression support |
|---|---|---|
| `ui::ExprInput` (`ExprDragFloat/Double/Int`, `ExprInputDouble`, `ExprDragFloat2`) | ~20 sites: every sketch dimension, every feature property row via `DragDim`, 3 in Study, 2 in Occurrence | Ctrl+click, evaluate on commit, **result** stored, text discarded — *and it does not work, see below* |
| `FeatureEditor::ExprDim` / `ExprCount` | feature dims with a persisted `*Expr` slot | `fx` button → `InputText`, text **persisted** and re-evaluated each regen. Works. |
| raw `ImGui::Drag*` / `Input*` | **32 sites** | none |

Plus three raw expression cells that own their own buffer and work fine:
`VariablesPanel.cpp:147`, `ConfigurationsPanel.cpp:161`, and `ExprDim`'s own
`##expr` (`FeatureEditor.cpp:240`, `:311`).

The 32 unconverted sites:

| file | n | what |
|---|---|---|
| `CamPanel.cpp` | 14 | Safe Z, stock above/around/below, A index, tool dia/feed/tip angle, top/bottom/stepdown, edge size, stepover, tabs |
| `StudyPanel.cpp` | 4 | direction vector, gravity vector, mode count, max iterations |
| `OccurrencePanel.cpp` | 4 | slide, rotation, position |
| `FeatureEditor.cpp` | 4 | pattern count, curve degree, control points, `DragFloat3Row` |
| `PreviewFeature.cpp` | 3 | section offset, tilt U, tilt V |
| `DrawingPanel.cpp` `SketchToolbar.cpp` `SettingsWindow.cpp` | 3 | view scale, calibration distance, int settings |

## Finding 1 — every variable-referencing expression is dropped

`ExprCommitOnLastItem` (`ExprInput.cpp`) gates on
`ImGui::IsItemDeactivatedAfterEdit()`. That flag is set by exactly one
function, `MarkItemEdited` (`imgui.cpp:4575`, the sole assignment). The
Ctrl+click path, `TempInputScalar` (`imgui_widgets.cpp`), suppresses it while
editing and only releases it at commit:

```cpp
g.LastItemData.ItemFlags |= ImGuiItemFlags_NoMarkEdited;
if (TempInputText(...)) {
    DataTypeApplyFromText(data_buf, data_type, p_data, format, NULL);  // sscanf
    g.LastItemData.ItemFlags &= ~ImGuiItemFlags_NoMarkEdited;
    value_changed = memcmp(&data_backup, p_data, data_type_size) != 0;
    if (value_changed) MarkItemEdited(id);      // <- only if sscanf moved the number
}
```

`DataTypeApplyFromText` is a bare `sscanf` with no arithmetic. An expression
beginning with a letter does not parse, so the value does not change, so the
item is never marked edited, so our evaluator never runs.

| typed into an `ExprDrag*` field | outcome |
|---|---|
| `width/2`, `len`, `sqrt(2)` | **silently dropped**, field reverts |
| `10/2` where the field was `20` | `5` — correct |
| `10/2` where the field was `10` | stays `10` |
| `2*3` where the field was `5` | `6` — correct |

Only arithmetic over literals survives, and only when the leading literal
happens to differ from what is already in the field. The feature exists, is
undocumented in the app, and does not do the one thing it was built for.

Confirmed on the running app (`properties/dimension_field_takes_an_expression`,
2026-08-27): `5*8` into a width of 2 gives 40, then `sqrt(100)` into that same
field leaves it at **40**. Three lines of evidence agree — the source chain
above, the gate, and the observation that prompted the audit.

**There is no test on this path.** The sketch expression tests
(`GuiTestsSketch.cpp:1504`) go through the inline dimension editor's own
`InputText`; the assembly one (`GuiTestsAssembly.cpp:645`) through `##expr`
after `fx`. The drag path has never been exercised.

## Finding 2 — expressions silently truncate at 31 characters

`TempInputScalar`'s buffer is `char data_buf[32]` on the stack, and
`imgui_widgets.cpp:4104` refuses an insert that would exceed `BufCapacity`.
Past 31 characters the keystrokes are dropped with no feedback.
`base_thickness*2+clearance` is 26.

Confirmed (`properties/dimension_expression_is_not_truncated`): a 33-character
`1+1+...+1` summing to 17 commits as **16** — its 31-character prefix.

## Finding 3 — the mirrored buffer is one frame stale

`g_buf` is only refreshed while `IsItemActive()`. A paste-then-commit landing
in a single frame commits the previous frame's text.

## Finding 4 — on the 32 raw fields, Ctrl+click accepts a wrong number

Ctrl+click is stock ImGui and works on *every* drag widget in the app, ours or
not. On an unconverted field it runs the same bare `sscanf`: typing `10/2`
yields **10**, silently. Same class as the A2-sheet-loading-as-A4 case behind
the "refuse, don't substitute" rule — a plausible wrong value accepted without
complaint — just in the GUI rather than a parser.

## Finding 5 — nothing tells you any of this exists

There is no visual cue that a field takes text at all. The only discoverable
path is `fx`, which exists solely on `ExprDim` rows. CAM, Study, Occurrence,
Drawing and Settings have nothing.

## Plan

Ordered so correctness lands before convenience. The original order — put
autocomplete on the `##expr` fields first, because it was the easy half — was
wrong: it would have shipped completion into a third of the fields while the
other two thirds silently ate whatever you typed. Autocompleting into a field
that then discards the input is worse than no autocomplete.

### Phase 0 — the gate that should already exist

A GUI test that Ctrl+clicks a Properties dimension, types `width/2`, and
asserts the value. It fails on today's build. That is the confirmation and the
regression gate in one, and it wants a sibling for the 31-char case.

### Phase 1 — `ExprDrag*` owns its buffer

`ImGuiSliderFlags_NoInput` on the `DragScalar`, plus our own `InputText` on
Ctrl+click, sharing one internal helper with `ExprDim`. Once we own the buffer
we no longer depend on ImGui's `NoMarkEdited` / `value_changed` bookkeeping,
and findings 1, 2 and 3 all close together.

Known risk, on the critical path: the test engine's `ItemInput`
(`imgui_te_context.cpp:2837`) *is* MouseMove + Ctrl+click + type, and 28 call
sites across `src/app/guitests/` and `tests/` use it. Our swap costs a frame
before the `InputText` is active, so typing can start too early. Mitigation, in
order of preference: keep the same ImGui ID and `SetKeyboardFocusHere`; else a
yield; else a small `ItemInput` shim in `GuiTestsCommon`.

### Phase 2 — convert the raw sites — SHIPPED

**34, not 32**: the source scan found two `ImGui::DragScalar` calls (pipe wall
thickness, thread length) that the hand grep had missed because it enumerated
the friendly spellings. The gate earned its place before it had ever guarded
anything.

Five new wrappers rather than two: `ExprDragFloat3`, `ExprInputInt`,
`ExprInputFloat`, `ExprInputFloat3`, and a format argument on `ExprDragInt`
(the autosave-interval field displays `%d s`).

The `Input*` family keeps ImGui's widget and only adds the evaluation — those
are text fields already, with a 64-byte buffer, so they have neither the drop
nor the truncation. Only the drags needed the stand-in. `InputScalar` with
step buttons hides its text field inside `PushID(label)` + `InputText("")`, and
`InputScalarN` nests one more level per component, so the commit hook takes an
explicit id rather than "the last item".

The `Drag*N` rows ARE ours now (a stand-in has to replace the drag, not sit
beside it), and they are wrapped in `BeginGroup`/`EndGroup` exactly as ImGui
wraps its own: `EndGroup` forwards the active component's status to the row,
which is what `HandleEdit` queries to snapshot for undo. Without it a
three-component row silently stops being undoable — gated by
`properties/vector_row_takes_an_expression`, which edits and then undoes.

### Phase 3 — completion

`ui/ExprComplete.{h,cpp}`: take the live buffer and cursor, find the
identifier prefix under the caret, and offer

- document and project variables from `doc.Symbols()` — already the merged
  table (`EvaluateParameterTable` does `out = *base`, then overlays document
  params), with `ProjectSymbols()->count(name)` distinguishing scope for a
  badge,
- the builtin functions (`sin cos tan sqrt abs min max floor ceil`),
- the unit suffixes (`mm cm m in ft deg rad`).

Rows read `width   = 42.5`. Tab or click accepts, arrows move, Esc dismisses;
Enter commits the field as typed unless a row is explicitly highlighted.

Because Phase 1 landed first, this lights up the drag fields and the `##expr`
fields from one implementation.

**Click-to-pick.** ImGui deactivates an active `InputText` on any outside
click, same frame, unconditionally (`imgui_widgets.cpp:4696`) — no flag
suppresses it. That is only data loss if ImGui owns the buffer, and after
Phase 1 it does not. So: on the frame the field deactivates, test the click
against the previous frame's cached row rects; a hit is a pick, not a commit —
splice the name into our buffer, set a refocus flag, suppress the commit. The
next frame calls `SetKeyboardFocusHere()`, which re-activates from our buffer,
and a one-shot `CallbackAlways` places the caret after the inserted name.
Public API throughout.

Keyboard-only acceptance was considered and rejected: shipping a hint that
explains a limitation is worse than removing the limitation, and here the
limitation is removable.

## Open decision

**An affordance for expression entry.** Ctrl+click is invisible, and after
Phase 2 it will be live on every numeric field in the app. Options: a faint
`fx` glyph at the field's right edge on hover; extending the real `fx` button
to non-`ExprDim` fields; or leaving it undiscoverable. Deferred to Phase 2,
when there is something worth discovering.

## Deliberately out of scope

`ExprInput` keeps its evaluate-and-discard semantics. After Phase 2 CAM's
Safe Z will *accept* `stock_top + 5` and store the number, not the text. Giving
CAM operations persisted `*Expr` slots is a format bump and its own workstream
— worth doing, not worth coupling to this.

## What changed on contact

**The predicted risk did not happen; a different one did.** Phase 1 named the
test engine's `ItemInput` path as the thing that could turn a small change into
a slog — 28 call sites, and a frame of delay before our stand-in takes focus.
It cost nothing: `SetKeyboardFocusHere()` armed over a few frames was enough,
no shim, no yields, and all 228 GUI tests passed first time.

What did bite was **ImGui's single `InputTextState`, keyed by item id**. On
re-activation it restores the text *it* holds, straight over the seed we had
just written — so a field edited once reopens holding the previous expression,
and the next keystroke appends to it. The fix is the documented one,
`ReloadUserBufAndSelectAll()` while the stand-in is arming.

**The gate that caught it did not exist an hour earlier.** The first two tests
both passed with the bug present, because `ItemInputValue` clears the field
with Ctrl+A before typing — it can never see a field that opens holding the
wrong text. Only `dimension_field_opens_selected`, which does `ItemInput` and
then bare `KeyChars`, walks the path a person walks. That the failure was
order-dependent (`146` = `1+1+…+1` from the previous test with `30` appended to
its last term) is what made it worth chasing rather than reruning.

Worth keeping: a GUI test that types through `ItemInputValue` is testing the
commit, not the field. If what you changed is how the field *opens*, the test
has to open it and type.

**Phase 1 split in two, and the first half is worth remembering.** Before
taking the buffer over, a ~10-line change closed finding 1 on its own: stop
asking `IsItemDeactivatedAfterEdit()` and decide from our own tracking that
the field just committed. That is all it took to make every dropped expression
work — the buffer takeover was needed for findings 2 and 3 and for Phase 3's
completion popup, not for correctness. `ExprInputDouble` still uses that path
(it is an ImGui `InputScalar` with its own 64-byte buffer, so it has neither
the drop nor the truncation); it joins the shared field in Phase 3, when
completion needs it to.

**Phase 2 found two more fields than the audit did.** The convention gate is
a `[convention]` scan in `tests/test_conventions.cpp`, and the first time it
ran it failed on two `ImGui::DragScalar` calls no one had counted — the hand
grep had listed the friendly spellings (`DragFloat`, `InputInt`, …) and missed
the general one. A scanning test that finds something on its first run is the
argument for scanning tests.
