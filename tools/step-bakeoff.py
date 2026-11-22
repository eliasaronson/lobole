#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""How well the STEP reader does on files other programs wrote
(docs/interchange/README.md#the-step-bake-off). An occasional audit, like
tools/bench.py: how an OpenCASCADE upgrade or a reader patch is measured.

    tools/step-bakeoff.py fetch MANIFEST        the pinned files, into the cache
    tools/step-bakeoff.py run BINARY (MANIFEST | --dir DIR) [--json OUT]

A MANIFEST is JSON: {"corpus": name, "files": [{"url", "sha256",
"reference"?: {"volume_mm3", "solids"?}}]}. `fetch` downloads each file
into ~/.cache/lobole/step-bakeoff/<sha256>.step, never the repository
(some corpora are not ours to redistribute), and refuses one whose bytes
are not the pinned SHA-256. `run` imports each file (`import_step` with
`keep_origin`) in a headless instance of its own and reports per file:
opened, the import's warning, valid and closed (`check_geometry`), the
volume against the reference keyed by the file's SHA-256, and seconds.
`--dir` runs every .step/.stp/.igs/.iges under a directory instead, with
no reference.
"""
import argparse
import glob
import hashlib
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "python"))
CACHE = os.path.join(os.path.expanduser("~"), ".cache", "lobole",
                     "step-bakeoff")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def cached(entry):
    ext = os.path.splitext(entry["url"].split("?")[0])[1].lower() or ".step"
    return os.path.join(CACHE, entry["sha256"] + ext)


def cmd_fetch(args):
    with open(args.manifest) as f:
        manifest = json.load(f)
    os.makedirs(CACHE, exist_ok=True)
    bad = 0
    for e in manifest["files"]:
        out = cached(e)
        if os.path.exists(out) and sha256(out) == e["sha256"]:
            continue
        tmp = out + ".part"
        req = urllib.request.Request(e["url"],
                                     headers={"User-Agent": "step-bakeoff"})
        with urllib.request.urlopen(req, timeout=120) as r, \
                open(tmp, "wb") as f:
            f.write(r.read())
        if sha256(tmp) != e["sha256"]:
            os.remove(tmp)
            print(f"refused {e['url']}: not the pinned SHA-256")
            bad += 1
            continue
        os.replace(tmp, out)
    print(f"fetched {len(manifest['files']) - bad} of "
          f"{len(manifest['files'])} into {CACHE}")
    return 1 if bad else 0


def import_one(binary, path):
    """One file in a fresh headless instance: what the import said and
    what it brought in."""
    from smoke_app import launch_app
    from lobole.client import Client
    app = launch_app(binary)
    out = {"file": path, "sha256": sha256(path)}
    try:
        d = Client.wait_for(app.port, strict=True)
        t0 = time.time()
        try:
            r = d.call("import_step", path=path, keep_origin=True)
        except Exception as e:  # noqa: BLE001 -- the reader said no
            out.update(opened=False, error=str(e),
                       seconds=time.time() - t0)
            return out
        out["seconds"] = time.time() - t0
        bodies = (r.get("digest") or {}).get("bodies", [])
        out.update(opened=bool(bodies), warning=r.get("warning", ""),
                   bodies=len(bodies),
                   solids=sum(b.get("solids", 0) for b in bodies),
                   volume_mm3=sum(b.get("volume_mm3", 0.0) for b in bodies))
        g = d.call("check_geometry")
        out["valid"] = bool(g.get("valid"))
        out["closed"] = all(b.get("free_edges", 0) == 0
                            for b in g.get("bodies", []))
        return out
    finally:
        app.terminate()  # by its pid: the Popen's
        try:
            app.wait(timeout=10)
        except Exception:  # noqa: BLE001
            app.kill()
            app.wait(timeout=10)


def cmd_run(args):
    refs = {}
    if args.manifest:
        with open(args.manifest) as f:
            manifest = json.load(f)
        files = []
        for e in manifest["files"]:
            p = cached(e)
            if not os.path.exists(p):
                print(f"missing {p}: run `fetch` first")
                return 2
            files.append(p)
            if "reference" in e:
                refs[e["sha256"]] = e["reference"]
    else:
        files = sorted(p for p in glob.glob(os.path.join(args.dir, "**", "*"),
                                            recursive=True)
                       if os.path.splitext(p)[1].lower()
                       in (".step", ".stp", ".igs", ".iges"))
    rows = []
    for p in files:
        row = import_one(os.path.abspath(args.binary), p)
        ref = refs.get(row["sha256"])
        if ref and row.get("opened"):
            want = ref["volume_mm3"]
            row["volume_rel"] = abs(row["volume_mm3"] - want) / max(
                abs(want), 1e-12)
        rows.append(row)
        print(f"{os.path.basename(p)[:48]:48s} "
              f"{'opened' if row.get('opened') else 'FAILED':6s} "
              f"{'valid' if row.get('valid') else 'invalid':7s} "
              f"{'closed' if row.get('closed') else 'open':6s} "
              f"{row.get('volume_mm3', 0.0):14.4f} "
              + (f"rel {row['volume_rel']:.1e} " if "volume_rel" in row
                 else "")
              + f"{row.get('seconds', 0.0):6.2f} s"
              + (f"  {row['warning'][:60]}" if row.get("warning") else ""))
    n = len(rows)
    print(f"step-bakeoff: {n} file(s): "
          f"{sum(bool(r.get('opened')) for r in rows)} opened, "
          f"{sum(bool(r.get('valid')) for r in rows)} valid, "
          f"{sum(bool(r.get('closed')) for r in rows)} closed")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("manifest")
    r = sub.add_parser("run")
    r.add_argument("binary")
    g = r.add_mutually_exclusive_group(required=True)
    g.add_argument("manifest", nargs="?")
    g.add_argument("--dir")
    r.add_argument("--json")
    args = ap.parse_args()
    return cmd_fetch(args) if args.cmd == "fetch" else cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())
