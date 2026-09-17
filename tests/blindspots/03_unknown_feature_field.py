"""Blind spot: a plausible-but-wrong feature field used to be silently
ignored — Box accepted mode:"add", dropped it, and created a separate
body; the failure surfaced three calls later as a null-mass BOM.

Incident: the dock's corner-mooring bracket (v3 build), where the 'mixed
material' BOM row was the first visible symptom.
"""


def run(d, expect):
    # WRONG: Box has no mode. Must error AND name the accepted vocabulary
    # (the full-template principle: don't reveal one constraint per trip) —
    # both the fields this feature holds and where the rest of the type's
    # dialect is written down, since a field left at its DEFAULT does not
    # serialize and so cannot be listed from the feature itself.
    try:
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10, "mode": "add"})
        raise AssertionError("Box mode:'add' must be rejected")
    except RuntimeError as e:
        expect("unknown field 'mode'" in str(e), f"names the field: {e}")
        expect("width" in str(e) and "height" in str(e),
               f"lists the fields it does hold: {e}")
        expect("add_feature" in str(e),
               f"says where the rest of the vocabulary is: {e}")

    # RIGHT: to merge prisms, extrude with mode:"add" (Extrude has one).
    d.call("add_feature", feature={
        "type": "Box", "name": "B", "position": [0, 0, 0],
        "width": 10, "height": 10, "depth": 10})
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "S",
        "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": [0, 0], "c2": [10, 10]}]})
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Riser", "sketch_id": sk["id"],
        "distance": 5, "mode": "add"})
    expect(abs(r["digest"]["delta"]["volume"] - 500.0) < 1e-6,
           f"the add merges into the body: {r['digest']['delta']}")
    expect(len(r["digest"]["bodies"]) == 1,
           f"still ONE body: {r['digest']['bodies']}")
