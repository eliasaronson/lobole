#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The kernel case corpus (docs/modeling/README.md#kernel-cases).

tests/kernel_cases/ holds small cases whose answers were worked out by hand,
most of them at a coincidence, a tangency or a geometric limit: SolidCraft's
kernel rig, copied unchanged, and solidcore's later cases beside recipes
written here. A case is a command script (recipe.json) and its right answer
(expected.json). This tool turns each recipe into RPC calls at run time
(tools/kernel_cases_translate.py), builds it in a headless instance, grades
it, and holds the whole corpus to tests/kernel_cases/status.json.

    tools/kernel_cases.py vendor --from SOLIDCRAFT [--solidcore SOLIDCORE]
    tools/kernel_cases.py run BINARY [--ladder [--quick] | --area A ...]
                              [-k TEXT ...] [--only FILE]
                              [--shard K/N] [--jobs N] [--write-page]
    tools/kernel_cases.py bless BINARY [--jobs N]
    tools/kernel_cases.py replay BINARY CASE
    tools/kernel_cases.py self-test

`run` exits non-zero when a case's outcome is not the one status.json holds:
a pass that fails, a known failure that passes or fails another way, an
untranslated case that now translates, a case with no row. `bless` rewrites
every row but the `known` ones, and refuses while a failing case has none.
`replay` prints one case's calls and its expected values against what was
measured. `--jobs` instances run side by side, each with smoke_app's
isolation; an instance is stopped by its pid, and one that outlives a
case's budget is a `hang`.
"""
import argparse
import collections
import decimal
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TESTS = os.path.join(ROOT, "tests")
CORPUS = os.path.join(TESTS, "kernel_cases")
SOURCE = os.path.join(CORPUS, "SOURCE")
STATUS = os.path.join(CORPUS, "status.json")
SLOW = os.path.join(CORPUS, "slow.json")
# A case that took longer than this when the rows were blessed is left to
# the whole-corpus run: `run --quick` (the headless suite's ladder) skips it.
SLOW_S = 3.0
PAGE = os.path.join(ROOT, "docs", "modeling", "kernel-cases.md")
sys.path.insert(0, TESTS)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "python"))

import kernel_cases_translate as kt  # noqa: E402
from kernel_cases_translate import Untranslated  # noqa: E402

# The areas taken from SolidCraft's tests/kernel-cases/. The others test its
# own tools (assemblies, sheet-metal, surfaces, plastic, frames,
# construction, history, inspect, continuity).
AREAS = ("ladder", "booleans", "fillets", "shell", "sweeps", "solids",
         "threads", "import", "tessellation", "crashes")
SOLIDCRAFT_URL = "https://github.com/tangency-labs/solidcraft"
SOLIDCORE_URL = "https://github.com/tangency-labs/solidcore"
# The classes a failing case has. `refused` is a clean refusal of a case
# that has an answer (something not built); the others are wrong answers.
CLASSES = ("crash", "hang", "internal", "invalid", "accepted", "refused",
           "volume", "topology")
# Seconds a case may take before its instance is killed and it is a `hang`;
# a case's own timeout_s raises it.
BUDGET_S = 120.0
REPLACE_EVERY = 40

# What expected.json may hold. GRADED keys are compared; UNGRADED ones are
# what nothing here measures, counted on the page; WHOLE ones grade a
# SolidCraft command's own answer, so a case that has one is untranslated.
INFO_KEYS = {"sources", "what", "source", "method", "timeout_s", "tolerance",
             "outcome", "refusal", "valid"}
GRADED_KEYS = {"body_count", "volume_mm3", "area_mm2", "center_of_mass",
               "bbox", "faces", "shells", "watertight"}
UNGRADED_KEYS = {
    "edges": "SolidCraft counts a chain of edges between two faces once, "
             "and no RPC lists the vertices a chain joins at",
    "vertices": "no RPC counts a body's vertices",
    "continuity": "no RPC measures the continuity across a seam",
    "surface_types": "the surface kinds are the other kernel's own names",
    "splines": "no RPC reports a spline face's degree and spans",
    "boundary_edges": "a sheet's free boundary: sheets are not built here",
}
WHOLE_KEYS = ("results", "files", "flat_pattern", "construction", "missing")


class CorpusError(Exception):
    """The corpus itself is wrong (a vendored file edited, an expected.json
    with a key nobody reads): fails the run whatever status.json says."""


# --------------------------------------------------------------- the files

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def read_source():
    with open(SOURCE) as f:
        return json.load(f)


def check_source():
    """Every vendored file is the one SOURCE lists, and no other is there.
    A copied file is never edited: a case that is wrong upstream is fixed
    upstream and vendored again."""
    src = read_source()
    want = src["files"]
    have = set()
    for origin in ("solidcraft",):
        base = os.path.join(CORPUS, origin)
        for d, _, names in os.walk(base):
            for n in names:
                have.add(os.path.relpath(os.path.join(d, n), CORPUS))
    for d, _, names in os.walk(os.path.join(CORPUS, "solidcore")):
        for n in names:
            if n == "expected.json":
                have.add(os.path.relpath(os.path.join(d, n), CORPUS))
    missing = sorted(set(want) - have)
    extra = sorted(have - set(want))
    if missing or extra:
        raise CorpusError(
            f"tests/kernel_cases differs from SOURCE: {len(missing)} listed "
            f"file(s) missing {missing[:3]}, {len(extra)} unlisted {extra[:3]}"
            "; vendor again (tools/kernel_cases.py vendor)")
    changed = [p for p in sorted(want)
               if sha256(os.path.join(CORPUS, p)) != want[p]]
    if changed:
        raise CorpusError(
            f"{len(changed)} vendored file(s) were edited: {changed[:5]}. "
            "A copied case is never changed here; restore it "
            "(tools/kernel_cases.py vendor --from CHECKOUT)")


def git_head(checkout):
    out = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def git_tracked(checkout, sub):
    """The files git tracks under `sub`: a checkout's untracked scratch is
    not the repository's."""
    out = subprocess.run(["git", "-C", checkout, "ls-files", "-z", sub],
                         capture_output=True, check=True)
    return [p for p in out.stdout.decode().split("\0") if p]


def vendor(args):
    files = {}
    src = {"solidcraft": {"repository": SOLIDCRAFT_URL,
                          "commit": git_head(args.source),
                          "path": "tests/kernel-cases",
                          "license": "MIT",
                          "areas": list(AREAS)}}
    dest = os.path.join(CORPUS, "solidcraft")
    shutil.rmtree(dest, ignore_errors=True)
    root = "tests/kernel-cases"
    for rel in git_tracked(args.source, root):
        sub = os.path.relpath(rel, root)
        parts = sub.split("/")
        if parts[0] not in AREAS or len(parts) < 3:
            # An area's own gate files (KNOWN_FAILING, PATHS, skipped.json)
            # are SolidCraft's bookkeeping; ladder.json is the tier order.
            if sub != "ladder/ladder.json":
                continue
        out = os.path.join(dest, sub)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copyfile(os.path.join(args.source, rel), out)
        files[os.path.relpath(out, CORPUS)] = sha256(out)
    if args.solidcore:
        src["solidcore"] = {
            "repository": SOLIDCORE_URL,
            "commit": git_head(args.solidcore),
            "path": "crates/solidcore/tests/kernel_cases",
            "license": "Apache-2.0",
            "what": "the `expected` block of each entry SolidCraft's folder "
                    "does not have; the recipe.json beside it is ours"}
        base = os.path.join(args.solidcore, src["solidcore"]["path"])
        for d, _, names in os.walk(os.path.join(CORPUS, "solidcore")):
            if "recipe.json" not in names:
                continue
            case = os.path.relpath(d, os.path.join(CORPUS, "solidcore"))
            entry = os.path.join(base, case + ".json")
            if not os.path.isfile(entry):
                raise CorpusError(f"solidcore has no entry {case}.json for "
                                  f"the recipe at solidcore/{case}")
            with open(entry) as f:
                expected = json.load(f)["expected"]
            out = os.path.join(d, "expected.json")
            with open(out, "w") as f:
                json.dump(expected, f, indent=1)
                f.write("\n")
            files[os.path.relpath(out, CORPUS)] = sha256(out)
    else:
        old = read_source() if os.path.exists(SOURCE) else {}
        if "solidcore" in old:
            src["solidcore"] = old["solidcore"]
            files.update({p: h for p, h in old["files"].items()
                          if p.startswith("solidcore/")})
    src["files"] = dict(sorted(files.items()))
    with open(SOURCE, "w") as f:
        json.dump(src, f, indent=1)
        f.write("\n")
    print(f"vendored {len(files)} file(s); SOURCE written. Update "
          "tests/kernel_cases/LICENSES.txt if the commit moved.")
    return 0


# --------------------------------------------------------------- the cases

def all_cases():
    """Every case, as its path under tests/kernel_cases, sorted."""
    out = []
    for origin in ("solidcraft", "solidcore"):
        base = os.path.join(CORPUS, origin)
        for d, _, names in os.walk(base):
            if "recipe.json" in names:
                out.append(os.path.relpath(d, CORPUS))
    return sorted(out)


def area_of(case):
    return case.split("/")[1]


def ladder_order():
    """The ladder's cases in tier order, as ladder.json lists them."""
    with open(os.path.join(CORPUS, "solidcraft", "ladder",
                           "ladder.json")) as f:
        tiers = json.load(f)["tiers"]
    return ["solidcraft/" + c for t in tiers for c in t["cases"]
            if c.startswith("ladder/")]


def load_recipe(case):
    """The case's commands with its `base` chain's in front."""
    path = os.path.join(CORPUS, case)
    seen = set()
    chain = []
    while True:
        if path in seen:
            raise CorpusError(f"{case}: its base chain loops")
        seen.add(path)
        with open(os.path.join(path, "recipe.json")) as f:
            recipe = json.load(f)
        unknown = set(recipe) - {"base", "commands"}
        if unknown:
            raise CorpusError(f"{case}: recipe.json has {sorted(unknown)}; "
                              "a recipe is {base?, commands}")
        chain.append(recipe["commands"])
        if "base" not in recipe:
            break
        path = os.path.normpath(os.path.join(path, recipe["base"]))
    return [c for commands in reversed(chain) for c in commands]


class Stated:
    """A number as expected.json writes it: its value and the half unit of
    the last digit it states."""

    def __init__(self, dec):
        self.value = float(dec)
        exp = dec.as_tuple().exponent
        self.half = 0.5 * 10.0 ** exp if isinstance(exp, int) else 0.0
        self.fraction = isinstance(exp, int) and exp < 0


def _stated(text):
    return Stated(decimal.Decimal(text))


def load_expected(case):
    with open(os.path.join(CORPUS, case, "expected.json")) as f:
        exp = json.load(f, parse_float=_stated, parse_int=_stated)
    known = INFO_KEYS | GRADED_KEYS | set(UNGRADED_KEYS) | set(WHOLE_KEYS)
    unknown = set(exp) - known
    if unknown:
        raise CorpusError(f"{case}: expected.json has {sorted(unknown)}, "
                          "which nothing here reads; say in kernel_cases.py "
                          "whether it is graded")
    return exp


def plain(v):
    """A Stated tree as plain numbers."""
    if isinstance(v, Stated):
        return v.value if v.fraction else int(v.value)
    if isinstance(v, list):
        return [plain(x) for x in v]
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    return v


# ---------------------------------------------------------------- grading

REL_FLOOR = 1e-9      # no number is held tighter than this, relatively
POSITION_MM = 1e-4    # a position stated as a whole number of mm


def measure_tolerance(stated, rel):
    """The difference a volume or an area may show: the case's own relative
    tolerance, else half a unit in the last digit it states, never tighter
    than 1e-9 relative."""
    if rel is not None:
        return rel * abs(stated.value)
    return max(stated.half, REL_FLOOR * abs(stated.value))


def position_tolerance(stated, abs_mm, box):
    """A coordinate (mm): the case's abs_mm, else half a unit in the last
    digit it states. A box's side stated as a whole number is held to 1e-4
    mm, as the reference parts hold a box; a centre of mass stated as one
    is a rounded number (a thread's is off its axis by microns)."""
    if abs_mm is not None:
        return abs_mm
    if box and not stated.fraction:
        return POSITION_MM
    return max(stated.half, 1e-6)


def _count_ok(want, got):
    if isinstance(want, list):
        return want[0].value <= got <= want[1].value
    return got == want.value


def grade(exp, got):
    """[(quantity, class, expected, measured, ok)] for a built case.
    `got` is what measure() returned."""
    tol = exp.get("tolerance") or {}
    rel = tol["rel"].value if "rel" in tol else None
    abs_mm = tol["abs_mm"].value if "abs_mm" in tol else None
    rows = []

    def row(q, cls, want, have, ok):
        rows.append((q, cls, plain(want), have, bool(ok)))

    if "body_count" in exp:
        row("body_count", "volume", exp["body_count"], got["body_count"],
            _count_ok(exp["body_count"], got["body_count"]))
    for key, mine in (("volume_mm3", "volume"), ("area_mm2", "area")):
        if key not in exp:
            continue
        want, have = exp[key], got.get(mine)
        if have is None:
            row(key, "volume", want, None, False)
        elif isinstance(want, list):
            row(key, "volume", want, have,
                want[0].value <= have <= want[1].value)
        else:
            row(key, "volume", want, have,
                abs(have - want.value) <= measure_tolerance(want, rel))
    if "center_of_mass" in exp:
        want, have = exp["center_of_mass"], got.get("center_of_mass")
        row("center_of_mass", "volume", want, have, have is not None and all(
            abs(h - w.value) <= position_tolerance(w, abs_mm, False)
            for w, h in zip(want, have)))
    if "bbox" in exp:
        want, have = exp["bbox"], got.get("bbox")
        ok = have is not None and all(
            abs(h - w.value) <= position_tolerance(w, abs_mm, True)
            for side, k in (("min", 0), ("max", 1))
            for w, h in zip(want[side], have[k]))
        row("bbox", "volume", [want["min"], want["max"]], have, ok)
    if "faces" in exp:
        row("faces", "topology", exp["faces"], got.get("faces"),
            got.get("faces") is not None
            and _count_ok(exp["faces"], got["faces"]))
    if "shells" in exp:
        row("shells", "topology", exp["shells"], got.get("shells"),
            got.get("shells") is not None
            and _count_ok(exp["shells"], got["shells"]))
    if exp.get("watertight"):
        row("watertight", "invalid", True, got.get("watertight"),
            got.get("watertight") is True)
    return rows


def stl_watertight(path, weld_mm):
    """A binary STL's mesh, its vertices closer than weld_mm taken as one:
    (watertight, shells). Watertight is every edge used by exactly two
    triangles; shells are the pieces connected through edges (a void that
    touches the outer skin along a line or at a point is still its own)."""
    with open(path, "rb") as f:
        data = f.read()
    (n,) = struct.unpack_from("<I", data, 80)
    cell = max(weld_mm, 1e-9)
    ids = {}
    points = []

    def vertex(p):
        key = tuple(int(math.floor(c / cell)) for c in p)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    k = (key[0] + dx, key[1] + dy, key[2] + dz)
                    for i in ids.get(k, ()):
                        if math.dist(points[i], p) <= weld_mm:
                            return i
        points.append(p)
        ids.setdefault(key, []).append(len(points) - 1)
        return len(points) - 1

    edges = collections.Counter()
    parent = {}
    first = {}

    def find(a):
        while parent.setdefault(a, a) != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in range(n):
        v = struct.unpack_from("<9f", data, 84 + 50 * t + 12)
        tri = [vertex(v[0:3]), vertex(v[3:6]), vertex(v[6:9])]
        if len(set(tri)) < 3:
            continue  # welded to a line or a point: bounds nothing
        find(t)
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edge = (min(tri[a], tri[b]), max(tri[a], tri[b]))
            edges[edge] += 1
            parent[find(t)] = find(first.setdefault(edge, t))
    tight = bool(edges) and all(c == 2 for c in edges.values())
    return tight, len({find(t) for t in parent})


def whole_faces(d):
    """The faces as the cases count them: a whole analytic surface once.
    Two faces that share an edge and lie on one plane, cylinder or sphere
    (the edge smooth, the kind and radius the same) are one; a seam bounds
    one face twice and joins nothing."""
    total = 0
    for body in d.call("list_elements", max_faces=1000000)["bodies"]:
        faces = {f["name"]: f for f in body["faces"]
                 if f.get("area_mm2", 1.0) > 0.0}
        parent = {n: n for n in faces}

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for e in body["edges"]:
            pair = [n for n in e.get("faces", []) if n in faces]
            if len(pair) != 2 or pair[0] == pair[1] or not e.get("smooth"):
                continue
            a, b = faces[pair[0]], faces[pair[1]]
            if a["kind"] != b["kind"] or a["kind"] not in (
                    "plane", "cylinder", "sphere"):
                continue
            if a["kind"] != "plane" and abs(a["radius"] - b["radius"]) > \
                    1e-9 * max(1.0, a["radius"]):
                continue
            parent[find(pair[0])] = find(pair[1])
        total += len({find(n) for n in faces}) + body.get("unnamed_faces", 0)
    return total


def measure(d, exp, scratch, digest):
    """What the case is graded on, from the instance."""
    import reference_parts as rp
    out = dict(rp.measure(d))
    # A body is a solid: two lumps one feature left in one body are two,
    # unless the case counts its `shells`, which is how it says a body has
    # several.
    bodies = (digest or {}).get("bodies", [])
    out["body_count"] = len(bodies) if "shells" in exp else \
        sum(b.get("solids", 1) for b in bodies)
    if not out["bodies"]:
        out["body_count"] = 0
        return out
    if "faces" in exp:
        out["faces"] = whole_faces(d)
    if exp.get("watertight") or "shells" in exp:
        tol = exp.get("tolerance") or {}
        mesh = tol["mesh_mm"].value if "mesh_mm" in tol else 0.01
        weld = tol["weld_mm"].value if "weld_mm" in tol else 1e-4
        path = os.path.join(scratch, "mesh.stl")
        d.call("export_stl", path=path, stl_deflection_mm=mesh)
        out["watertight"], out["shells"] = stl_watertight(path, weld)
    return out


def validity(d):
    """'' when every body is valid and closed, else why not."""
    g = d.call("check_geometry")
    bad = []
    for b in g.get("bodies", []):
        if not b.get("valid", False) or b.get("free_edges", 0):
            bad.append(f"body {b.get('name')!r}: "
                       + ("; ".join(b.get("reasons") or [])
                          or f"{b.get('free_edges', 0)} free edge(s)"))
    for e in g.get("feature_errors", []):
        bad.append(f"feature {e.get('name')!r}: {e.get('error')}")
    return "; ".join(bad)


def measured_message(message):
    from fuzz_design import measured
    return measured(message)


def bodies_of(digest):
    return sorted((b["created_by"], b["solids"], round(b["volume_mm3"], 6),
                   tuple(b["bbox_min"]), tuple(b["bbox_max"]))
                  for b in (digest or {}).get("bodies", []))


class Outcome:
    """A case's result: `status` is "pass", "untranslated: <what>" or
    "fail: <class>"; `detail` says why, and `rows` are the graded
    quantities."""

    def __init__(self, status, detail="", rows=(), seconds=0.0, calls=()):
        self.status = status
        self.detail = detail
        self.rows = list(rows)
        self.seconds = seconds
        self.calls = list(calls)

    def to_json(self):
        return {"status": self.status, "detail": self.detail,
                "rows": self.rows, "seconds": self.seconds}


def judge(exp, run):
    """The outcome of a case from what its run left: `run` is
    {refusal: None | {message, internal, changed}, invalid, measured}."""
    outcome = (exp.get("outcome") or "solid")
    refusal = run.get("refusal")
    if refusal:
        if refusal.get("internal"):
            return "fail: internal", refusal["message"], []
        if not measured_message(refusal["message"]):
            return ("fail: internal", "refused without stating a condition: "
                    f"{refusal['message']!r}", [])
        if refusal.get("changed"):
            return ("fail: invalid", "a refused call changed the part: "
                    + refusal["message"], [])
        if outcome in ("refused", "solid_or_refused"):
            return "pass", "refused: " + refusal["message"], []
        return "fail: refused", refusal["message"], []
    if outcome == "refused":
        return ("fail: accepted", "the case must be refused "
                f"({exp.get('refusal', '')!r}) and it built", [])
    rows = grade(exp, run["measured"])
    if exp.get("valid", True) is not False and run.get("invalid"):
        return "fail: invalid", run["invalid"], rows
    bad = [r for r in rows if not r[4]]
    if not bad:
        return "pass", "", rows
    order = {c: i for i, c in enumerate(CLASSES)}
    cls = min((r[1] for r in bad), key=order.get)
    return (f"fail: {cls}", ", ".join(r[0] for r in bad) + " off", rows)


def whole_key(exp):
    for k in WHOLE_KEYS:
        if k in exp:
            return k
    return None


def run_case(d, case, scratch):
    """Build and grade one case on instance `d`. Raises ConnectionError or
    TimeoutError when the instance died or ran out of time."""
    t0 = time.time()
    exp = load_expected(case)
    commands = load_recipe(case)
    k = whole_key(exp)
    if k == "missing":
        return Outcome("untranslated: the case names a capability its own "
                       "source lacks")
    if k:
        return Outcome(f"untranslated: graded by `{k}`, a SolidCraft "
                       "command's own answer")
    d.call("new_project")
    tr = kt.Translator(d)
    run = {"refusal": None}
    try:
        for i, command in enumerate(commands, 1):
            before = bodies_of(tr.digest)
            try:
                tr.command(command)
            except kt.Refusal as e:
                run["refusal"] = {
                    "message": f"command {i} ({command['command']}): "
                               f"{e.message}",
                    "internal": e.internal}
                break
    except Untranslated as e:
        return Outcome(f"untranslated: {e}", seconds=time.time() - t0,
                       calls=tr.log)
    except kt.Broken as e:
        return Outcome("broken: the translator sent a call the app could "
                       "not read", str(e), seconds=time.time() - t0,
                       calls=tr.log)
    if run["refusal"] is None:
        run["invalid"] = validity(d)
        run["measured"] = measure(d, exp, scratch, tr.digest)
    else:
        # A refused call leaves the bodies as they were (a feature that
        # was added and failed to build holds none).
        state = d.call("get_state")["bodies"]
        now = sorted((b["created_by"], round(b["volume_mm3"], 6))
                     for b in state)
        run["refusal"]["changed"] = now != sorted(
            (b[0], b[2]) for b in before)
    status, detail, rows = judge(exp, run)
    return Outcome(status, detail, rows, time.time() - t0, tr.log)


# ----------------------------------------------------------- the instances

class Instance:
    """One headless app with smoke_app's isolation and a deadline per
    case."""

    def __init__(self, binary):
        from smoke_app import launch_app
        from lobole.client import Client
        self.app = launch_app(binary)
        self.client = Client.wait_for(self.app.port, strict=True)
        self.deadline = None
        outer = self

        class Timed:
            def request(self, method, params=None):
                left = max(1.0, outer.deadline - time.time())
                return outer.client.request(method, params or {},
                                            timeout=left)

            def call(self, method, **params):
                return self.request(method, params)

        self.d = Timed()
        self.scratch = tempfile.mkdtemp(prefix="kernel_cases_")

    def stop(self):
        try:
            self.client.close()
        except Exception:  # noqa: BLE001
            pass
        self.app.terminate()
        try:
            self.app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.app.kill()  # by its pid, which is the Popen's
            self.app.wait(timeout=10)
        shutil.rmtree(self.scratch, ignore_errors=True)

    def log_tail(self, n=15):
        return "\n".join(self.app.log().splitlines()[-n:])


def budget(case):
    exp = load_expected(case)
    own = exp["timeout_s"].value if "timeout_s" in exp else 0.0
    return max(BUDGET_S, min(own, 1800.0))


def worker(binary, cases, out_path):
    """Run `cases` on one instance, replacing it every REPLACE_EVERY cases
    and after a crash or a hang; write {case: outcome} as it goes."""
    results = {}
    inst = None
    used = 0
    try:
        for case in cases:
            if inst is None or used >= REPLACE_EVERY:
                if inst:
                    inst.stop()
                inst = Instance(binary)
                used = 0
            t0 = time.time()
            inst.deadline = t0 + budget(case)
            try:
                outcome = run_case(inst.d, case, inst.scratch)
            except CorpusError:
                raise
            except Exception as e:  # noqa: BLE001 -- classified here
                if not isinstance(e, (ConnectionError, OSError,
                                      TimeoutError)):
                    raise
                hang = isinstance(e, TimeoutError) or "timed out" in str(e)
                outcome = Outcome("fail: hang" if hang else "fail: crash",
                                  f"{e}\n{inst.log_tail()}",
                                  seconds=time.time() - t0)
                inst.stop()
                inst = None
            used += 1
            results[case] = outcome.to_json()
            with open(out_path + ".tmp", "w") as f:
                json.dump(results, f)
            os.replace(out_path + ".tmp", out_path)
    finally:
        if inst:
            inst.stop()
    return 0


def default_jobs():
    try:
        out = subprocess.run([os.path.join(ROOT, "tools", "build-jobs"),
                              "1200"], capture_output=True, text=True,
                             check=True).stdout.strip()
        return max(1, min(4, int(out)))
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 1


def run_cases(binary, cases, jobs):
    """{case: outcome json}, the cases dealt round the workers."""
    jobs = max(1, min(jobs or default_jobs(), len(cases) or 1))
    out_dir = tempfile.mkdtemp(prefix="kernel_cases_run_")
    procs = []
    try:
        for j in range(jobs):
            mine = cases[j::jobs]
            listing = os.path.join(out_dir, f"cases{j}.json")
            with open(listing, "w") as f:
                json.dump(mine, f)
            out = os.path.join(out_dir, f"out{j}.json")
            procs.append((subprocess.Popen(
                [sys.executable, os.path.abspath(__file__), "worker",
                 binary, listing, out]), mine, out))
        results = {}
        broken = False
        for proc, mine, out in procs:
            code = proc.wait()  # by its pid
            if os.path.exists(out):
                with open(out) as f:
                    results.update(json.load(f))
            if code != 0:
                broken = True
        if broken:
            raise CorpusError("a worker stopped before its cases were done "
                              "(its own message is above)")
        return results
    finally:
        for proc, _, _ in procs:
            if proc.poll() is None:
                proc.terminate()
        shutil.rmtree(out_dir, ignore_errors=True)


def compact(planted):
    """selftest.json with one command and one call a line."""
    lines = []
    for row in planted["rows"]:
        calls = row["calls"]
        body = json.dumps(calls) if isinstance(calls, str) else \
            "[\n    " + ",\n    ".join(json.dumps(c) for c in calls) + "]"
        lines.append(
            '  {"row": ' + json.dumps(row["row"]) + ',\n   "recipe": [\n    '
            + ",\n    ".join(json.dumps(c) for c in row["recipe"])
            + '],\n   "calls": ' + body + "}")
    return '{"rows": [\n' + ",\n".join(lines) + "\n]}\n"


# ------------------------------------------------------------ status.json

def read_status():
    try:
        with open(STATUS) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def write_status(status):
    lines = [f" {json.dumps(k)}: {json.dumps(v, ensure_ascii=False)}"
             for k, v in sorted(status.items())]
    with open(STATUS, "w") as f:
        f.write("{\n" + ",\n".join(lines) + "\n}\n")


def known_row(row):
    """(class, entry) from a row `known: <class> (<entry>)`, the entry a
    doc path with its heading's anchor; None for any other row."""
    if not row.startswith("known: "):
        return None
    rest = row[len("known: "):]
    cls, _, entry = rest.partition(" (")
    if cls not in CLASSES or not entry.endswith(")"):
        raise CorpusError(f"status.json: {row!r} is not `known: <class> "
                          f"(<entry>)` with a class of {list(CLASSES)}")
    return cls, entry[:-1]


def ratchet(status, results):
    """What a run's results say against status.json: a list of sentences,
    empty when every case came out as its row says."""
    bad = []
    for case in sorted(results):
        got = results[case]["status"]
        detail = results[case]["detail"].splitlines()[0][:200] \
            if results[case]["detail"] else ""
        row = status.get(case)
        if got.startswith("broken"):
            bad.append(f"{case}: {got}: {detail}")
            continue
        if row is None:
            bad.append(f"{case}: no row in status.json (it came out "
                       f"{got!r}: {detail})")
            continue
        known = known_row(row)
        if known:
            cls, entry = known
            if got == "pass":
                bad.append(f"{case}: PASSES but its row is {row!r}: remove "
                           f"the row's `known` and the entry it names "
                           f"({entry}), then bless")
            elif got != f"fail: {cls}":
                bad.append(f"{case}: known as {cls}, came out {got!r} "
                           f"({detail})")
        elif row == "pass":
            if got != "pass":
                bad.append(f"{case}: was pass, came out {got!r} ({detail})")
        elif row.startswith("untranslated: "):
            if got != row:
                what = ("now translates and came out" if not
                        got.startswith("untranslated") else "is now")
                bad.append(f"{case}: was {row!r}, {what} {got!r} "
                           f"({detail}); bless it or give it a known row")
        else:
            raise CorpusError(f"status.json: {case} has the row {row!r}; a "
                              "row is pass, untranslated: <what> or known: "
                              "<class> (<entry>)")
    return bad


def check_rows(status, cases):
    """Every vendored case has a row and every row a case."""
    bad = [f"{c}: a case with no row in status.json" for c in cases
           if c not in status]
    bad += [f"{c}: a row in status.json for no case" for c in status
            if c not in set(cases)]
    return bad


# ----------------------------------------------------------------- the page

PAGE_INTRO = """\
# Kernel cases

<!-- Generated by tools/kernel_cases.py run --write-page. Do not edit: the
     `kernel_cases_ladder` test fails when this page differs from what
     tests/kernel_cases/status.json renders. -->

Small cases with answers worked out by hand, most of them at a coincidence,
a tangency or a geometric limit, each built over the RPC from a command
script and held to its answer
([how](README.md#kernel-cases)). This page is what
`tests/kernel_cases/status.json` says of them: a case passes, is a known
failure with the entry that describes it, or is untranslated, naming the
command or parameter Loböle has no counterpart for.

## By area

| area | cases | pass | known | untranslated |
|---|---|---|---|---|
"""


def render_page(status):
    by_area = collections.defaultdict(collections.Counter)
    reasons = collections.Counter()
    known = []
    for case, row in sorted(status.items()):
        area = area_of(case)
        k = known_row(row)
        if k:
            by_area[area]["known"] += 1
            known.append((case, k[0], k[1]))
        elif row == "pass":
            by_area[area]["pass"] += 1
        else:
            by_area[area]["untranslated"] += 1
            reasons[row[len("untranslated: "):]] += 1
    lines = [PAGE_INTRO]
    order = [a for a in AREAS if a in by_area] + sorted(
        a for a in by_area if a not in AREAS)
    total = collections.Counter()
    for a in order:
        c = by_area[a]
        total.update(c)
        lines.append(f"| {a} | {sum(c.values())} | {c['pass']} | "
                     f"{c['known']} | {c['untranslated']} |\n")
    lines.append(f"| **all** | {sum(total.values())} | {total['pass']} | "
                 f"{total['known']} | {total['untranslated']} |\n")
    lines.append("\n## Known failures\n\n")
    if known:
        lines.append("| case | class | entry |\n|---|---|---|\n")
        for case, cls, entry in known:
            link = os.path.relpath(os.path.join(ROOT, entry.split("#")[0]),
                                   os.path.dirname(PAGE))
            anchor = "#" + entry.split("#")[1] if "#" in entry else ""
            lines.append(f"| {case} | {cls} | [{entry}]({link}{anchor}) |\n")
    else:
        lines.append("None.\n")
    lines.append("\n## Untranslated\n\n")
    if reasons:
        lines.append("| what has no counterpart | cases |\n|---|---|\n")
        for what, n in sorted(reasons.items(), key=lambda x: (-x[1], x[0])):
            lines.append(f"| {what.replace('|', chr(92) + '|')} | {n} |\n")
    else:
        lines.append("None.\n")
    lines.append("\n## Not graded\n\nKeys of an `expected.json` that "
                 "nothing here measures; a case that has one is graded on "
                 "its others.\n\n")
    for key, why in UNGRADED_KEYS.items():
        lines.append(f"- `{key}`: {why}.\n")
    return "".join(lines)


def check_page(status, write):
    page = render_page(status)
    if write:
        with open(PAGE, "w") as f:
            f.write(page)
        print(f"wrote {os.path.relpath(PAGE, ROOT)}")
        return True
    try:
        with open(PAGE) as f:
            committed = f.read()
    except OSError:
        committed = ""
    if committed != page:
        print(f"{os.path.relpath(PAGE, ROOT)} differs from what status.json "
              "renders: run tools/kernel_cases.py run BINARY --ladder "
              "--write-page and commit it with the rows")
        return False
    return True


# -------------------------------------------------------------- commands

def select(args):
    cases = all_cases()
    if getattr(args, "ladder", False):
        order = ladder_order()
        missing = set(order) - set(cases)
        if missing:
            raise CorpusError(f"ladder.json lists {sorted(missing)[:3]}, "
                              "which are not vendored")
        cases = order
    areas = getattr(args, "area", None)
    if areas:
        unknown = set(areas) - set(AREAS)
        if unknown:
            raise CorpusError(f"no area {sorted(unknown)}; the areas are "
                              f"{list(AREAS)}")
        cases = [c for c in cases if area_of(c) in areas]
    if getattr(args, "only", None):
        with open(args.only) as f:
            only = json.load(f)
        only = set(only if isinstance(only, list) else
                   [c for c, r in only.items()
                    if not r["status"].startswith(("pass", "untranslated"))])
        cases = [c for c in cases if c in only]
    for k in getattr(args, "k", None) or []:
        cases = [c for c in cases if k in c]
    if getattr(args, "quick", False):
        with open(SLOW) as f:
            slow = set(json.load(f))
        cases = [c for c in cases if c not in slow]
    shard = getattr(args, "shard", None)
    if shard:
        k, n = (int(x) for x in shard.split("/"))
        if not 0 <= k < n:
            raise CorpusError(f"--shard {shard}: want K/N with 0 <= K < N")
        # By a hash of the name, so a new case moves no other.
        cases = [c for c in cases
                 if int(hashlib.sha256(c.encode()).hexdigest(), 16) % n == k]
    return cases


def print_outcomes(results, verbose):
    counts = collections.Counter()
    for case in sorted(results):
        r = results[case]
        head = r["status"].split(":")[0]
        counts[head] += 1
        if verbose or head in ("fail", "broken"):
            first = r["detail"].splitlines()[0][:300] if r["detail"] else ""
            print(f"{case}: {r['status']} ({r['seconds']:.1f} s)"
                  + (f": {first}" if first else ""))
    return counts


def cmd_run(args):
    check_source()
    status = read_status()
    cases = select(args)
    if not cases:
        print("no case matches")
        return 2
    t0 = time.time()
    results = run_cases(args.binary, cases, args.jobs)
    counts = print_outcomes(results, args.verbose)
    partial = bool(args.k or args.only)
    bad = ratchet(status, results)
    if not partial:
        bad += check_rows(status, all_cases())
    for line in bad:
        print("RATCHET: " + line)
    print(f"kernel_cases: {len(results)} case(s) in {time.time() - t0:.0f} "
          f"s: {counts['pass']} pass, {counts['fail']} fail, "
          f"{counts['untranslated']} untranslated; {len(bad)} against "
          "status.json")
    if args.results:
        with open(args.results, "w") as f:
            json.dump(results, f, indent=1)
    ok = not bad
    if args.write_page or args.ladder:
        ok = check_page(status, args.write_page) and ok
    return 0 if ok else 1


def cmd_bless(args):
    check_source()
    status = read_status()
    cases = all_cases()
    results = run_cases(args.binary, cases, args.jobs)
    print_outcomes(results, False)
    new = {}
    unrowed = []
    for case in cases:
        got = results[case]["status"]
        row = status.get(case, "")
        if row.startswith("known: "):
            new[case] = row  # by hand; the run says whether it still holds
        elif got.startswith(("fail: ", "broken")):
            first = results[case]["detail"].splitlines()[:1]
            unrowed.append(f"{case}: {got} ({''.join(first)[:200]})")
        else:
            new[case] = got
    if unrowed:
        for line in unrowed:
            print("NO ROW: " + line)
        print(f"bless: {len(unrowed)} failing case(s) have no `known` row; "
              "give each one (and its entry) by hand, then bless. Nothing "
              "was written.")
        return 1
    write_status(new)
    slow = sorted(c for c in cases if results[c]["seconds"] > SLOW_S)
    with open(SLOW, "w") as f:
        f.write("[\n" + ",\n".join(" " + json.dumps(c) for c in slow)
                + "\n]\n")
    bad = ratchet(new, results)
    for line in bad:
        print("RATCHET: " + line)
    check_page(new, True)
    print(f"bless: {len(new)} row(s) written")
    return 1 if bad else 0


def cmd_replay(args):
    check_source()
    matches = [c for c in all_cases() if args.case in c]
    if len(matches) != 1:
        exact = [c for c in matches if c.endswith("/" + args.case)
                 or c == args.case]
        if len(exact) != 1:
            print(f"{args.case!r} names {len(matches)} cases: {matches[:8]}")
            return 2
        matches = exact
    case = matches[0]
    exp = load_expected(case)
    print(f"{case}\n  what: {plain(exp).get('what')}\n  method: "
          f"{plain(exp).get('method')}")
    inst = Instance(args.binary)
    try:
        inst.deadline = time.time() + budget(case)
        outcome = run_case(inst.d, case, inst.scratch)
    finally:
        if args.keep:
            print(f"instance left running on port {inst.app.port}, pid "
                  f"{inst.app._proc.pid}")
        else:
            inst.stop()
    for step in outcome.calls:
        print("  " + json.dumps(step))
    for q, cls, want, have, ok in outcome.rows:
        print(f"  {'ok  ' if ok else 'FAIL'} {q:15s} expected {want}\n"
              f"       {'':15s} measured {have}")
    print(f"{outcome.status} ({outcome.seconds:.1f} s)"
          + (f": {outcome.detail}" if outcome.detail else ""))
    print(f"status.json: {read_status().get(case)!r}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("vendor")
    v.add_argument("--from", dest="source", required=True,
                   help="a SolidCraft checkout")
    v.add_argument("--solidcore", help="a solidcore checkout")
    r = sub.add_parser("run")
    r.add_argument("binary")
    r.add_argument("--ladder", action="store_true",
                   help="the ladder's tiers only, in tier order")
    r.add_argument("--quick", action="store_true",
                   help="skip the cases slow.json lists")
    r.add_argument("--area", action="append")
    r.add_argument("-k", action="append", default=[],
                   help="only cases whose path has this text; repeatable")
    r.add_argument("--only", help="a JSON list of cases, or an earlier "
                   "--results file: the cases that failed in it")
    r.add_argument("--shard", help="K/N: the cases whose name hashes to K")
    r.add_argument("--jobs", type=int, default=0)
    r.add_argument("--write-page", action="store_true")
    r.add_argument("--verbose", "-v", action="store_true")
    r.add_argument("--results", help="write every outcome to this file")
    b = sub.add_parser("bless")
    b.add_argument("binary")
    b.add_argument("--jobs", type=int, default=0)
    p = sub.add_parser("replay")
    p.add_argument("binary")
    p.add_argument("case")
    p.add_argument("--keep", action="store_true",
                   help="leave the instance running, the part open")
    sub.add_parser("self-test")
    w = sub.add_parser("worker")  # run_cases' own
    w.add_argument("binary")
    w.add_argument("listing")
    w.add_argument("out")
    args = ap.parse_args()
    try:
        if args.cmd == "vendor":
            return vendor(args)
        if args.cmd == "run":
            return cmd_run(args)
        if args.cmd == "bless":
            return cmd_bless(args)
        if args.cmd == "replay":
            return cmd_replay(args)
        if args.cmd == "worker":
            with open(args.listing) as f:
                return worker(args.binary, json.load(f), args.out)
        import kernel_cases_selftest
        return kernel_cases_selftest.main()
    except CorpusError as e:
        print(f"kernel_cases: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
