#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Slice the original model's STEP export into the bought parts the
generator imports.

    python3 generators/smart_speaker/extract_bought.py [--port N]
            [--step ~/Downloads/step_files/speaker.step]
            [--out scratch/smart_speaker/bought]

Run this ONCE, and again only when the source STEP changes. The source is
48 MB and 302 products, of which about two hundred are the SMD parts on the
amplifier board; importing it costs 27.5 s and fills the project with tabs
nobody edits. The generator wants three small files instead, so it imports
catalogue geometry in under a second and the run stays worth repeating.

Parts are found three ways, because the export names them three ways.

By NAME where it gives one: the driver's four solids are `W170Sbasket`,
`W170Scone`, `W170Sspider`, `W170Ssusp`.

By SHAPE where it does not: the export named the cabinet `Part 1`, twenty
times over, so the Alpair and the power brick are identified by the volume and
bounding box that only they have. A match that is not unique is refused rather
than guessed at — a wrong solid here is a driver that looks right and is 30 mm
out.

By GROUP for the amplifier, which is not one solid: the two case halves are
`Top` and `Bottom` and the board is `Board~gQ98`. They are exported at their
ASSEMBLY placements, so the module arrives assembled.

**The board's ~200 passives are deliberately left out.** They are inside a
closed case, so they change no clearance, no mass worth counting and nothing
you can see; exporting them costs 109 MB — more than twice the whole source —
and two hundred tabs in a project about a loudspeaker cabinet. The resistor
you want to look at is in the EDA tool, not here. `--with-passives` puts them
back for anyone who disagrees.
"""
import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

from lobole.errors import Refused  # noqa: E402
from lobole.sdk import Batch, Part, SdkError, connect  # noqa: E402

DEFAULT_STEP = Path.home() / "Downloads/step_files/speaker.step"
DEFAULT_OUT = HERE.parent.parent / "scratch/smart_speaker/bought"

# What to pull out, and how to recognise it.
#
# `names` matches the STEP product name. `volume`/`size` identify a solid the
# export left as "Part 1": the volume in mm^3 and the bounding box in mm, both
# +/- `tol` relative. Measured off the export, so a changed design trips the
# uniqueness check rather than silently exporting the wrong body.
WANTED = [
    dict(file="w170s.step", what="Visaton W 170 S woofer",
         names=["W170Sbasket", "W170Scone", "W170Sspider", "W170Ssusp"]),
    dict(file="alpair7ms.step", what="Markaudio Alpair 7 MS full-range",
         volume=324559.0, size=(122.0, 62.1, 122.0)),
    dict(file="psu.step", what="power supply brick",
         volume=205219.9, size=(47.0, 142.0, 38.6)),
    dict(file="amp_module.step", what="ESP32 amplifier: case halves + board",
         group=True),
]
TOL = 0.02
EXPORT_TIMEOUT = 600    # seconds; a 200-solid export is not quick


def _tab(app, row):
    """A Part for a slot row.

    `app.part(name)` is the SDK's way in and it addresses a tab by NAME, which
    an imported STEP does not give you: the source exported thirty solids all
    called "Part 1", and asking for that name returns whichever came first.
    The slot row from `list_parts` carries the id, and `Part` takes it.
    """
    return Part(app, row)


def _bbox_size(part):
    lo, hi = part.bbox()
    return tuple(hi[i] - lo[i] for i in range(3))


def _close(a, b, tol=TOL):
    return abs(a - b) <= tol * max(abs(a), abs(b), 1.0)


def _is_amp(name, with_passives):
    """The amplifier module — see the module docstring on what is left out.

    The case halves are `Top` and `Bottom`; the board and every component the
    EDA tool placed carry a `~`. Nothing in the cabinet is named either way,
    so with `--with-passives` this is a rule and not a list, and it does not
    go stale when the schematic changes.
    """
    stem = name.split(" ")[0]
    if stem in ("Top", "Bottom"):
        return True
    if with_passives:
        return "~" in name
    return stem.startswith("Board~")


def _by_name(parts, names):
    hits = [p for p in parts if p["name"].split(" ")[0] in names]
    missing = set(names) - {p["name"].split(" ")[0] for p in hits}
    if missing:
        raise SdkError(f"no part named {sorted(missing)} in the STEP")
    return hits


def _by_shape(app, parts, volume, size):
    """The one solid with this volume and this bounding box.

    Volume is tested first because it is one round trip and rejects all but a
    handful; only those few pay for a bounding box as well.
    """
    hits = []
    for row in parts:
        part = _tab(app, row)
        try:
            if not _close(part.volume(), volume):
                continue
            got = _bbox_size(part)
            if all(_close(got[i], size[i]) for i in range(3)):
                hits.append(row)
        except (SdkError, Refused):
            # An empty tab (the export has one) answers 'no bodies'; it is
            # not a candidate, and it is not an error worth stopping for.
            continue
    if not hits:
        raise SdkError(
            f"nothing in the STEP has volume {volume:.0f} mm3 and bbox "
            f"{size} — the export changed; re-measure before trusting this")
    if len(hits) > 1:
        raise SdkError(
            f"volume {volume:.0f} mm3 / bbox {size} matches "
            f"{len(hits)} parts ({[h['name'] for h in hits]}) — not unique, "
            "so it is not a way to name one")
    return hits


def _export_group(app, rows, placements, path, tag):
    """A handful of solids in one file, each where the source assembly puts
    them.

    The driver's four solids are only a driver when they are together AND in
    the right places relative to each other, so the transform from the source
    assembly comes with them.
    """
    ids = {r["id"] for r in rows}
    mine = [o for o in placements if o.get("part_id") in ids]
    if not mine:
        raise SdkError(f"{tag}: none of those parts is placed in the assembly")
    asm = app.assembly(f"export {tag}")
    asm.activate()
    # Batch(app), not app.batch(): batch is a Part method, and on App the name
    # resolves to the generated RPC, which wants a `calls` list.
    with Batch(app) as b:
        for o in mine:
            b.add_occurrence(part=o["part_id"], fixed=True,
                             transform=o["transform"])
    asm.activate()
    app.export_step(path=str(path), _timeout=EXPORT_TIMEOUT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--step", default=str(DEFAULT_STEP))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--with-passives", action="store_true",
                    help="include the board's ~200 components (109 MB)")
    args = ap.parse_args()

    src = Path(args.step).expanduser()
    if not src.exists():
        sys.exit(f"no STEP at {src}")
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)

    app = connect(args.port)
    app.new()
    print(f"importing {src} ({src.stat().st_size / 1e6:.0f} MB) ...")
    app.import_step(path=str(src), as_assembly=True)
    parts = [p for p in app.list_parts()["parts"] if p.get("type") == "part"]
    print(f"  {len(parts)} parts")

    src_asm = next(a for a in app.list_parts()["parts"]
                   if a.get("type") == "assembly")
    app.set_active_part(part=src_asm["id"])
    placements = app.list_occurrences()["occurrences"]

    for spec in WANTED:
        if spec.get("group"):
            rows = [r for r in parts if _is_amp(r["name"], args.with_passives)]
            if not rows:
                raise SdkError("no amplifier parts in the STEP — the naming "
                               "changed; see _is_amp")
        elif "names" in spec:
            rows = _by_name(parts, spec["names"])
        else:
            rows = _by_shape(app, parts, spec["volume_mm3"], spec["size"])
        path = out / spec["file"]
        if len(rows) == 1:
            app.set_active_part(part=rows[0]["id"])
            app.export_step(path=str(path), _timeout=EXPORT_TIMEOUT)
        else:
            _export_group(app, rows, placements, path, spec["file"])
        kb = path.stat().st_size / 1024
        print(f"  {spec['file']:<18} {kb:8.0f} kB  {len(rows):3d} solid(s)  "
              f"{spec['what']}")

    print(f"\nwrote {len(WANTED)} files to {out}")
    print("the generator imports these; it never reads the 48 MB source")


if __name__ == "__main__":
    main()
