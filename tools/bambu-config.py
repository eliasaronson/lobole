#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Make the slicer config a print job's Bambu project carries.

export_print_job writes a Bambu Studio PROJECT only from a complete config:
Bambu applies a project's config over its own built-in defaults, so a
partial one slices on a 200 mm bed with a generic start G-code
(docs/tolerances/README.md#print-jobs). Loböle has no slicer presets of its
own; this takes yours.

    tools/bambu-config.py --printer "Bambu Lab P1S 0.4 nozzle" \\
        --process "0.20mm Standard @BBL X1C" \\
        --filament "Bambu PLA Basic @BBL X1C" -o pla.json

It flattens the three system presets from the Bambu Studio install
(following `inherits` and `include`), writes them as --flat-dir if asked,
and has the Bambu CLI build a one-cube project from them, whose
Metadata/project_settings.config is the config. Bambu Studio is found as
$LOBOLE_BAMBU_STUDIO, else `bambu-studio` on PATH, else the nix root
~/.local/state/nix-roots/bambu-studio.

Instead of names, --flat MACHINE PROCESS FILAMENT takes presets already
flattened (Bambu's own `--load-settings` form).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def find_bambu():
    env = os.environ.get("LOBOLE_BAMBU_STUDIO")
    if env:
        return Path(env)
    on_path = shutil.which("bambu-studio")
    if on_path:
        return Path(on_path)
    root = Path.home() / ".local/state/nix-roots/bambu-studio/bin/bambu-studio"
    return root if root.exists() else None


def profiles_dir(binary):
    share = binary.resolve().parent.parent / "share/BambuStudio/profiles/BBL"
    if not share.is_dir():
        sys.exit(f"bambu-config: no system profiles under {share}")
    return share


def index(kind_dir):
    by_name = {}
    for p in kind_dir.glob("*.json"):
        try:
            d = json.loads(p.read_text())
        except (OSError, ValueError):
            continue
        if isinstance(d, dict) and "name" in d:
            by_name[d["name"]] = d
    return by_name


def flatten(name, by_name, seen=()):
    if name not in by_name:
        sys.exit(f"bambu-config: no system preset '{name}'")
    if name in seen:
        sys.exit(f"bambu-config: '{name}' inherits itself")
    d = by_name[name]
    out = {}
    if d.get("inherits"):
        out.update(flatten(d["inherits"], by_name, seen + (name,)))
    for inc in d.get("include", []) or []:
        part = flatten(inc, by_name, seen + (name,))
        for k, v in part.items():
            if k not in ("name", "type", "from", "instantiation", "inherits"):
                out[k] = v
    for k, v in d.items():
        if k not in ("inherits", "include"):
            out[k] = v
    return out


CUBE = """solid cube
""" + "".join(
    " facet normal 0 0 0\n  outer loop\n" +
    "".join(f"   vertex {x} {y} {z}\n" for x, y, z in tri) +
    "  endloop\n endfacet\n"
    for tri in [
        [(0, 0, 0), (0, 10, 0), (10, 10, 0)], [(0, 0, 0), (10, 10, 0), (10, 0, 0)],
        [(0, 0, 10), (10, 0, 10), (10, 10, 10)], [(0, 0, 10), (10, 10, 10), (0, 10, 10)],
        [(0, 0, 0), (10, 0, 0), (10, 0, 10)], [(0, 0, 0), (10, 0, 10), (0, 0, 10)],
        [(0, 10, 0), (0, 10, 10), (10, 10, 10)], [(0, 10, 0), (10, 10, 10), (10, 10, 0)],
        [(0, 0, 0), (0, 0, 10), (0, 10, 10)], [(0, 0, 0), (0, 10, 10), (0, 10, 0)],
        [(10, 0, 0), (10, 10, 0), (10, 10, 10)], [(10, 0, 0), (10, 10, 10), (10, 0, 10)],
    ]) + "endsolid cube\n"


def project_config(binary, machine, process, filament):
    with tempfile.TemporaryDirectory(prefix="bambu-config-") as tmp:
        t = Path(tmp)
        (t / "cube.stl").write_text(CUBE)
        out = t / "out"
        out.mkdir()
        r = subprocess.run(
            [str(binary), "--arrange", "1", "--orient", "0",
             "--load-settings", f"{machine};{process}",
             "--load-filaments", str(filament),
             "--export-3mf", "cube.3mf", "--outputdir", str(out),
             str(t / "cube.stl")],
            capture_output=True, text=True, timeout=300, cwd=tmp)
        made = out / "cube.3mf"
        if not made.exists():
            sys.exit(f"bambu-config: the Bambu CLI made no project "
                     f"(exit {r.returncode}): {r.stderr[-600:]}")
        with zipfile.ZipFile(made) as z:
            return json.loads(z.read("Metadata/project_settings.config"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--printer")
    ap.add_argument("--process")
    ap.add_argument("--filament")
    ap.add_argument("--flat", nargs=3, metavar=("MACHINE", "PROCESS",
                                                "FILAMENT"))
    ap.add_argument("--flat-dir", help="also write the flattened presets here")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    binary = find_bambu()
    if binary is None:
        sys.exit("bambu-config: no Bambu Studio (set LOBOLE_BAMBU_STUDIO)")
    with tempfile.TemporaryDirectory(prefix="bambu-flat-") as tmp:
        if a.flat:
            machine, process, filament = (Path(p).resolve() for p in a.flat)
        else:
            if not (a.printer and a.process and a.filament):
                sys.exit("bambu-config: --printer, --process and --filament, "
                         "or --flat")
            share = profiles_dir(binary)
            flat = (Path(a.flat_dir) if a.flat_dir else Path(tmp)).resolve()
            flat.mkdir(parents=True, exist_ok=True)
            machine = flat / "machine.json"
            process = flat / "process.json"
            filament = flat / "filament.json"
            for path, kind, name in ((machine, "machine", a.printer),
                                     (process, "process", a.process),
                                     (filament, "filament", a.filament)):
                d = flatten(name, index(share / kind))
                path.write_text(json.dumps(d, indent=2) + "\n")
        cfg = project_config(binary, machine, process, filament)
    Path(a.out).write_text(json.dumps(cfg, indent=4) + "\n")
    print(f"{a.out}: {cfg.get('printer_settings_id')} / "
          f"{cfg.get('print_settings_id')} / "
          f"{cfg.get('filament_settings_id')}")


if __name__ == "__main__":
    main()
