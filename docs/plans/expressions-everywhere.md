# Expressions everywhere: CAM jobs and drawings

Every number a person or an agent types into a document should be able to be
an expression of the variables, and stay one. Parts (features, studies, the
sheet-metal and tube rules, tolerances, sketch patterns) and assemblies do
(docs/variables/README.md#numbers-that-are-expressions); a CAM job and a
drawing do not.

## The problem

- **Every number in a CAM job is a plain double**: tools, operations, stock,
  setups, machine (about 54). A tool diameter cannot be the variable the bore
  is drilled to.
- **A drawing view's scale and position are plain.** `DrawingView::scaleExpr`
  is serialized but nothing evaluates or sets it.
- **Only parts and assemblies recompute.** `ui::SyncProjectScope`
  (src/ui/VariableActions.cpp) regenerates parts and re-solves assemblies; a
  job or a drawing never sees a variable change.

## The shape of the fix

The type, the visitor and the RPC convention exist (`ExprValue`, `ExprInt`,
`ExprAngle`, `ExprSlot`, `WriteExpr`/`ReadExpr`, `ExprArgs`, the evaluating
`remote::ExprParam`); this stage puts the two remaining document kinds on
them, the way assemblies went on (src/core/AssemblyExprs.cpp).

1. **The numbers become `ExprValue`/`ExprInt`/`ExprAngle`** in
   `src/core/cam/CamJob.h` and `src/core/drawing/DrawingDocument.h`, the
   dead `scaleExpr` replaced by an `ExprValue` scale.
2. **`JobDocument::VisitExprs` and `DrawingDocument::VisitExprs`** hand out
   each slot with its owner ("tool 'T3'", "view 'Front'") and its path in the
   document's JSON, as `AssemblyDocument::VisitExprs` does. Rename
   (`RewriteProjectVarRefs`), the delete refusal (`ui::ProjectVarReferences`)
   and the literals lint walk them.
3. **Scope.** Both resolve against the project's variables; a lone job or
   drawing outside a project against an empty table, so an expression there
   is refused naming the variable it could not find.
4. **Recompute.** `SyncProjectScope` evaluates every open job and drawing
   that holds an expression. A job puts the evaluated values into its input
   keys, so a changed tool diameter marks the toolpath stale, never silently
   current (`ui::MarkJobEdited`, docs/cam/gui.md#undo). A drawing
   re-evaluates its scales and positions and re-projects. A value that cannot
   stand keeps its last good one and becomes that owner's error, reported in
   the `set_variable` response's `regen`.
5. **RPC.** `<field>_expr` beside every number `RemoteCam.cpp` and
   `RemoteDrawings.cpp` take, through `ExprArgs` or `ExprParam` with the
   project's symbols; the getters answer `<field>_expr`; the `describe` rows
   say so; `rpc.py` and `api/rpc-surface.json` are regenerated.
6. **UI.** The CAM panels' and the drawing panel's fields that edit one use
   `ExprBoundDrag`, keeping the text and unbinding on a plain number.
7. **Nothing new arrives unbindable.** A probe per document kind like
   `tests/test_assembly_exprs.cpp`'s and `tests/test_part_exprs.cpp`'s: a
   sentinel through every slot, read back from the file, and every number the
   file holds is a slot or on a short list with its reason.

Format: one `v222` with a golden per kind (`tests/golden/job/`,
`tests/golden/drawing/`).

## Files

`src/core/cam/CamJob.h` and its serializer, `src/core/drawing/DrawingDocument.h`
and its serializer, `src/remote/RemoteCam.cpp`, `src/remote/RemoteDrawings.cpp`,
`src/ui/VariableActions.cpp`, the CAM and drawing panels, new
`tests/test_job_exprs.cpp` / `tests/test_drawing_exprs.cpp` and their smokes,
`docs/cam/`, `docs/drawings/`, `docs/variables/README.md`,
`docs/scripting/rpc.md`, `docs/versioning/format.md`.

## Verified by

- The probe per document kind.
- Through the RPC: a tool diameter from a variable marks the toolpath stale;
  a drawing scale from a variable rescales the view.
- Rename a variable used in a part, an assembly, a job and a study at once:
  every reference follows. Delete it: refused, naming all four.
- A GUI test: a CAM field bound by typing an expression, the variable changed
  in the Variables panel, the field following.
- The headless, GUI and pygui suites; the action matrix, since its corpus
  covers jobs.
