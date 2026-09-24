"""How each part of the engine LOOKS, apart from what it is made of.

A clean factory finish, as the reference photographs show a new OM615: the
iron block and head and the covers bolted to them painted a dark grey, the
cam cover, intake manifold, vacuum pump and alternator castings bare
aluminium, the exhaust manifold dark cast iron, belts, hoses and seals black
rubber, the fan and the pulleys painted black. Fasteners, internals and
anything not listed keep their material's own look.

`set_appearance` is visual only — mass, the BOM and FEM keep the material —
and recolouring rebuilds no geometry. Colours are LINEAR, as the renderer
lights them: a grey that reads 0.5 in sRGB is about 0.2 here.
"""

FINISHES = {
    # Grey paint over iron: dark, semi-matte, a little metallic sheen.
    "paint": dict(color=[0.13, 0.14, 0.15], roughness=0.55, metallic=0.25),
    # Bare sand-cast aluminium: light, dull, metallic.
    "aluminium": dict(color=[0.52, 0.53, 0.54], roughness=0.5,
                      metallic=0.9),
    # As-cast iron that has been hot: near black, rough.
    "cast iron": dict(color=[0.045, 0.042, 0.04], roughness=0.8,
                      metallic=0.55),
    "rubber": dict(color=[0.015, 0.015, 0.016], roughness=0.85,
                   metallic=0.0),
    # Black paint on steel parts that are not cast: fan, pulleys.
    "black": dict(color=[0.025, 0.025, 0.028], roughness=0.45,
                  metallic=0.3),
}

# Part name -> finish. Exact names, so a renamed part falls back to its
# material's look rather than picking up a stranger's.
PART_FINISH = {
    "block": "paint",
    "cylinder head": "paint",
    "timing cover": "paint",
    "sump": "paint",
    "water pump": "paint",
    "engine mount left": "paint",
    "engine mount right": "paint",
    "alternator mounting bracket": "paint",
    "alternator adjustment bracket": "paint",
    "rear seal carrier": "paint",
    "cam cover": "aluminium",
    "intake manifold": "aluminium",
    "vacuum pump casting": "aluminium",
    "vacuum pump valve cover": "aluminium",
    "alternator front casting": "aluminium",
    "alternator rear casting": "aluminium",
    "thermostat housing": "aluminium",
    "exhaust manifold": "cast iron",
    "fan belt": "rubber",
    "timing cover gasket": "rubber",
    "sump gasket": "rubber",
    "cooling fan": "black",
    "fan hub spacer": "black",
    "water pump pulley": "black",
    "alternator pulley": "black",
    "crank damper": "black",
    "oil filler cap": "black",
}


def finish(part):
    """Give `part` its look from the table, if it has one; returns it."""
    family = PART_FINISH.get(part.name)
    if family is None:
        return part
    part.activate()
    for i in range(len(part.bodies())):
        part.app.set_appearance(body=i, **FINISHES[family])
    return part


def apply_all(app):
    """Finish every part of the engine that the table names."""
    for slot in app.parts():
        if slot["name"] in PART_FINISH:
            finish(app.part(slot["id"], create=False))
