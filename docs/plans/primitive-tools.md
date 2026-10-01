# Primitive tools that model the way a person would

## Problem

`Box`, `Cylinder` and `Sphere` are features of their own, placed by absolute
world coordinates. They are the quickest way for an agent to make a solid,
and agents use them heavily. They make worse models for a person to edit:

- **Nothing follows the design.** A boss placed at z = 24.7 on a 24.7 mm
  plate stays at 24.7 when the plate becomes 30, and floats or is buried.
- **The intent is unreadable.** The tree says "Box at (12, -4, 24.7)", not
  "a 10 mm boss centred on the top face".
- **It is not how the rest of the model is built.** Everything else is a
  sketch on a face or plane, constrained, then extruded or revolved.
  Primitives are a second dialect, which a person then has to rebuild to edit.

The sketch-based tools (SolidWorks, Onshape) have no primitives. The tools
that keep them (Fusion, Inventor, FreeCAD) at least place them on a picked
plane or face.

## Fix

### 1. A primitive tool expands into the features a person would draw

The tool keeps its one-call convenience but stores ordinary history:

- **Box:** a Sketch on the reference holding a rectangle (centred or
  cornered), constrained and dimensioned, then an Extrude along the
  reference's normal.
- **Cylinder:** a circle and an Extrude.
- **Sphere:** a semicircle on a plane through the axis, closed by the axis
  line, then a Revolve.

**Every primitive names what it is on.** That is a face, a datum plane, a
sketch plane or a mate connector, by stable name, or one of the named world
planes (Top/Front/Right). There is no bare world position. The sketch
attaches the way a sketch on a face already does: it resolves the exact B-rep
frame at every rebuild. A lost reference is a feature error, never a
fallback to world.

**Sizes and offsets:**

- The width, depth, height, diameter and an offset along the frame's u and v
  are dimensions in the sketch or on the extrude, and accept expressions.
- Defaults derive from the reference (a fraction of the picked face), per the
  defaults rule.
- The sketch comes out fully constrained: DOF 0.

**Interfaces:**

- **RPC:** `add_feature {type: "Box" | "Cylinder" | "Sphere", on, ...}`
  creates the features in one undo step. It returns every created id, with
  the solid-making one (Extrude or Revolve) first. An unknown `on`, or a
  missing one, is refused, naming what is accepted.
- **SDK:** the `box()` / `cylinder()` / `sphere()` helpers take `on`.
- **GUI:** pick a face first; the ghost appears on it and drags in that
  frame, snapping to the face's centre and edges.

The core `BoxFeature`, `CylinderFeature` and `SphereFeature` classes stay as
an internal shortcut for test fixtures, which use them in about 180 files.
They are no longer reachable from the RPC, the SDK or the GUI.

### 2. Guidance

- **The prompt:** the MCP `modeling-workflow` prompt (`WORKFLOW` in
  `tools/designer_mcp.py`) gets a short rule. Model on faces and planes
  from sketches. A primitive is for a fixture, a stand-in or a first rough
  block, and is always placed `on` something.
- **The lint:** the advisory literals lint (docs/plans/literals-lint.md) flags
  a feature whose placement is a world coordinate while a face of the part
  lies on it, and names the face to attach to.

## Files

- `src/core/features`: the expansion, building on the Sketch, Extrude and
  Revolve features.
- `src/remote/RemoteModeling.cpp` (`add_feature`) and its describe rows.
- `tools/designer_sdk.py`, `tools/designer_mcp.py` (the feature formats and
  `WORKFLOW`).
- The GUI primitive tools in `src/ui` (CreateActions and the tool sessions).
- `docs/modeling/README.md`.
- The generators that call `box()`/`cylinder()` (about a dozen), moved to
  `on`.
- The saved form does not change, since only existing feature types are
  stored, so there is no kVersion bump.

## Verification

- Each primitive on a face, a datum plane, a world plane and a connector
  produces a fully constrained sketch and the expected solid volume.
- Editing the referenced face's feature (a taller plate) moves the primitive
  with it.
- Deleting the reference refuses with an error and leaves the body
  unchanged.
- One undo removes all of a primitive's features.
- The defaults GUI suite (`defaults/*`) covers the new tool sessions.
- The generators that use primitives regenerate with unchanged volumes.
- The benchmark's free self-test (`reference.py`) still builds every task.
