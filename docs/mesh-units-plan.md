# Mesh formats cross a unit boundary

## The problem

Storage is millimetres. Two mesh boundaries ignored that.

- **glTF out.** glTF's unit is the metre. `export_visual` converted the up
  axis (Z-up to glTF's Y-up) but not the length, and `export_motion`'s own
  writer did the same, so a 10 mm part opened 10 m across in every glTF
  viewer. A probe of a 10 x 20 x 30 mm box read back POSITION bounds of
  10/30/20.
- **STL in.** An STL has no unit field at all; a BREP is OpenCASCADE's
  unitless native form. Both were read as millimetres with no way to say
  otherwise, so a mesh drawn in metres (the common case from Blender and game
  tools) arrived 1000x too small, and one drawn in inches 25.4x too small —
  a part that loads, looks fine, and is wrong.

STEP is not affected: it declares its unit (`SI_UNIT(.MILLI.,.METRE.)` on
export, converted by the reader on import). STL export writes millimetres,
which is what every slicer assumes. OBJ/PLY stay millimetres and Z-up: neither
format defines a unit or an up axis.

## What to build

1. **glTF in metres.** `MeshExport.cpp` gives the converter that already swaps
   the axes its length pair too (`SetInputLengthUnit(0.001)`,
   `SetOutputLengthUnit(1.0)`), so OpenCASCADE scales the vertices. The
   animated writer (`GlbAnimation.cpp`) puts `scale 0.001` on the root node
   that already carries the axis rotation — everything under it stays mm.
2. **An Import's `units` (v103).** `ImportFeature::lengthUnit`, serialized as
   `units` (`mm` | `cm` | `m` | `in`, the `units::Length` parsers), absent for
   mm. Execute scales the STL/BREP shape once as it is read. It is REFUSED on
   a STEP import, which declares its own unit, and an unknown string is
   refused naming the accepted ones — never read as mm.
3. **A warning that names the fix.** An STL/BREP import under 1 mm across
   warns, naming metres and inches and the `units` field. Not a refusal: a
   genuinely tiny part is legitimate.
4. **Every entry point.** `import_step {units}` (refused with `as_assembly`,
   which reads STEP), `add_feature` / `modify_feature` through the feature
   JSON, the MCP `import_step` tool, and a "File units" combo in the Import
   feature's editor for STL/BREP. A `.STL` extension imports too — the
   extension check was case-sensitive.
5. The feature-field gate on `add_feature` accepts a documented key whose
   value is the default and so does not serialize back (`units: "mm"`,
   `keep_origin: false`); an UNdocumented key is still refused.

## Verification

- `test_motion.cpp`: the animated GLB's root carries the 0.001 scale.
- `visual_export_smoke.py`: an exported 10 x 6 x 4 mm block spans
  0.010 x 0.006 x 0.004.
- `test_cadkernel.cpp`: an STL read as inches is 25.4x per side (and loads
  from a `.STL` name); a metre-drawn mesh read as mm warns and, said as
  metres, is its real size; a STEP import refuses `units`.
- `headless_smoke.py`: `import_step(units="cm")`, the refusals over the RPC,
  and `units: "mm"` accepted by `add_feature`.
- `tests/golden/v103-import-stl-units.json`: `units` survives save/load.
