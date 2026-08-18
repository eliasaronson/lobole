# Appearance override — visual look separate from physical material — plan

> Status: **SHIPPED (2026-08-18, format v54).** As planned below, with two
> improvements found during implementation: (1) appearance is STAMPED in a
> Document::Regenerate post-pass keyed on createdBy (no Execute-signature
> plumbing) and STRIPPED from the regen chain hash — a recolor executes
> zero features (appearance_smoke asserts executed == 0), which also makes
> baked-geometry changes unnecessary (the stamp pass re-applies on load);
> (2) a color-only override on a material-less body resolves against a
> neutral dielectric, not DEFAULT's metal. Surface: set_appearance RPC
> (validates 0..1 with a 0..255-slip error), get_state echo, Appearance
> submenu in the body context menu (derived values, one undo per widget
> release). Acceptance passed: the Marsliden dock's barrels are blue
> (scratch/build_dock.py), mass unchanged, contact gates green.
>
> Original plan:
> Written 2026-08-18, triggered by the
> Marsliden floating-dock model (`scratch/build_dock.py`): its 220 L barrels
> are physically plastic but *blue*, and the only lever we have is the
> material slug — so they render white. Adding a `plastic_blue` slug was
> rejected on sight: color is not a material, and a slug per color scales
> like a paint catalogue.

## Why

A material slug currently welds together two unrelated things:

1. **Physics** — density (mass properties, BOM), Young/Poisson/yield (FEM).
2. **Look** — baseColor, metallic/roughness/specular, procedural grain.

Real objects break the weld constantly: blue HDPE barrels, painted steel,
stained pine. The physics side is right per slug; only the look needs a
per-body override. We already half-have the concept: `Body::displayColor`
(src/core/Feature.h) carries an XCAF import color as "a viewport appearance,
NOT a physical material", stamped transiently each regen. This plan promotes
that idea to a first-class, user-settable, persistent override — instead of
multiplying slugs.

## Design

**Data model.** An optional per-creating-feature appearance, mirroring how
`Feature::m_material` already works (persistent on the feature, stamped onto
the bodies it creates during regen):

```cpp
struct Appearance {            // all fields optional = inherit from material
  std::optional<glm::vec3> color;      // sRGB base color
  std::optional<float> roughness;      // 0..1
  std::optional<float> metallic;       // 0..1
};
```

- `Feature::m_appearance` — persistent + serialized, like `m_material`.
- `Body::appearance` — transient, stamped at regen, like `Body::material`
  and `Body::displayColor`.

**Resolve order** (per draw, CPU-side): palette → import `displayColor` →
material → **appearance fields on top**. The merge happens where materials
are already resolved into per-mesh params — `AppActions.cpp` (~2665, part
tab) and `AssemblyActions.cpp` (~987, assembly shared-body cache) — plus the
offline ray tracer's material lookup. No shader change: the mesh push
constant sits at the 128-byte Vulkan floor (wood-grain round), and
`SetMeshMaterial`/`SetMeshColor` already take CPU-resolved values, so an
override just edits what we upload. Grain interaction: overriding `color` on
a wood material keeps the grain pattern (contrast darkens toward the grain
lines from the overridden tone) — that is the *desired* behavior for stained
wood; no special case.

**Not touched.** MassProps, FEM, and BOM read `Body::material` only and stay
that way — appearance must never leak into physics. Drawings are HLR line
art, unaffected.

**Serialization.** New optional object on the feature record next to
`"material"` (SerializeFeatures.cpp ~512/1325); absent = no override. Format
bump v53 → v54 + golden pair `tests/golden/v54-appearance.json/.expect.json`
per policy; no migration (absent field just deserializes to nullopt).
**Baked geometry**: GeometryCache.cpp already round-trips `display_color`;
the resolved appearance must be baked the same way or reopened projects
silently lose the override until first regen — bump the cache version.

**RPC/MCP.** `set_appearance {feature_id, color?: [r,g,b], roughness?,
metallic?}`; calling with no optional fields clears the override (mirrors
`set_material` with `""`). Surface it in `get_state` per body, add the
describe table row (tests enforce one per new RPC), reachable via the MCP
`rpc` tool — no new top-level MCP tool needed at the current 25-tool budget.

**GUI.** Lives with the material picker (FeatureTreePanel context menu /
FeatureEditor): a swatch + two sliders under the material combo, plus a
"reset to material" affordance so the override state is visible and
escapable. Defaults gate (docs/ui/u10): every field defaults to *derived* —
inherited live from the assigned material, never a literal.

## Phases

1. **Core + format** — struct, feature/body plumbing, serialization + golden,
   baked-geometry carry. Headless-testable end to end.
2. **Render merge** — the two viewport resolve sites + ray tracer; verify by
   screenshot against dev-watch.
3. **RPC + GUI** — `set_appearance`, describe row, get_state, picker UI,
   `defaults/*` + smoke coverage.
4. **Dogfood close-out** — `set_appearance` the dock barrels HDPE-blue in
   `scratch/build_dock.py` and drop its "no blue material" note; that script
   is the acceptance test this plan exists to pass.

## Deferred

- **Per-occurrence override** (same part, different colors per assembly
  instance) — needs occurrence-level storage and an instancing-path merge;
  design when a real model demands it.
- **Per-face appearance** (decals, painted faces) — waits on stable face
  refs everywhere patterns exist (toponaming leftovers).
- **Texture/image appearance** — out of scope; procedural-only stays the
  house style.
