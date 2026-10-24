#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The action matrix (docs/modeling/README.md#action-matrix): every ordinary
action on every face, edge and body of a fixed corpus of real designs, sent
over the RPC, each outcome held to a committed baseline.

    tools/action_matrix.py run [--sample] [--shard K/N] [--design PAT]
                               [--actions a,b] [--jobs N] [--out DIR]
                               [--determinism] [--bless] [--profile]
                               [--binary B]
    tools/action_matrix.py bless --from DIR    # merge shard outputs
    tools/action_matrix.py replay CASE.json    # one action, judged
    tools/action_matrix.py self-test

The corpus is tests/action_matrix/corpus.json; the baseline
tests/action_matrix/baseline.json, one line per entity: `edge.fillet` a
pass, `!edge.fillet` a fail, `-edge.fillet` a refusal, `edge.fillet*3` three
passes in a family of positional names. A run fails on a pass that is now a
fail or a refusal, a refusal that is now a fail, a design that no longer
builds, and a finding (a crash, a hang, an internal error, a caught signal,
a message that states nothing, an undo that does not undo) the baseline's
`known` table does not list -- or one it lists that no longer reproduces.

A run writes one JSONL per design and a report.md under
scratch/action_matrix/<stamp>/ (or --out). `--bless` rewrites the baseline
from a whole run; `bless --from DIR` from the shard outputs a set of
`--shard K/N --out DIR` runs left there. Each worker owns one headless
instance (smoke_app's isolation, --autosave 0), killed only by its pid.
"""
import argparse
import fnmatch
import glob
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TESTS = os.path.join(ROOT, "tests")
for p in (HERE, TESTS, os.path.join(ROOT, "python")):
    if p not in sys.path:
        sys.path.insert(0, p)

import fuzz_design as fd  # noqa: E402

MATRIX = os.path.join(TESTS, "action_matrix")
CORPUS = os.path.join(MATRIX, "corpus.json")
BASELINE = os.path.join(MATRIX, "baseline.json")

ACTION_TIMEOUT_S = 60.0
SAMPLE_EVERY = 32          # --sample: one (design, action, family) in 32
DETERMINISM_EVERY = 8      # --determinism: one in 8, of the sample's designs
MAX_FACES, MAX_EDGES = 400, 1200   # a body past either is not tried
# A Shell offsets the whole body: on a 195-face gear one takes 30 s and more.
# A body past BIG_BODY_FACES is shelled on SHELLS_ON_BIG_BODY of its planar
# faces, the ones whose names hash lowest, not on every one.
BIG_BODY_FACES, SHELLS_ON_BIG_BODY = 100, 4
REPLACE_EVERY = 25         # designs per instance before it is replaced
SAMPLE_FUZZ_SEEDS = 8      # the sample's fuzz designs: the first eight
SAMPLE_GENERATORS = ("generator/gear",)

ACTIONS = ("edge.fillet", "edge.chamfer", "edge.frame",
           "face.sketch", "face.extrude_add", "face.extrude_cut",
           "face.hole", "face.shell", "face.draft", "face.frame",
           "curved.thread", "curved.frame",
           "body.pattern_linear", "body.pattern_circular", "body.mirror",
           "body.split",
           "face.delete", "face.delete_feature", "face.offset_out",
           "face.offset_in", "face.move", "face.replace")
OUTCOMES = ("pass", "refuse", "fail", "finding")
# A message naming a signal OpenCASCADE caught (OSD::SetSignal turns a
# SIGSEGV inside the kernel into an OSD_SIGSEGV exception, which reads as an
# ordinary failure unless something looks).
SIGNAL = re.compile(r"OSD_SIG|OSD_Exception|SIGSEGV|SIGBUS|SIGFPE|SIGILL")
# ISO 261 coarse threads the app's table holds: designation, major diameter.
ISO_COARSE = (("M1.6", 1.6), ("M2", 2.0), ("M2.5", 2.5), ("M3", 3.0),
              ("M4", 4.0), ("M5", 5.0), ("M6", 6.0), ("M8", 8.0),
              ("M10", 10.0), ("M12", 12.0), ("M16", 16.0), ("M20", 20.0),
              ("M24", 24.0), ("M30", 30.0))


def stable_hash(*parts):
    h = hashlib.sha1("\0".join(str(p) for p in parts).encode()).hexdigest()
    return int(h[:12], 16)


def occ_series(occ):
    """'7.9.3' -> '7.9': the baseline is one kernel series'."""
    return ".".join(str(occ).split(".")[:2])


# --- the corpus ---------------------------------------------------------
class CorpusError(Exception):
    pass


ROW_KEYS = {"design", "kind", "source", "parts", "args"}
KINDS = ("reference", "fuzz", "golden", "generator")


def golden_files():
    return sorted(os.path.relpath(f, ROOT) for f in
                  glob.glob(os.path.join(TESTS, "golden", "v*.json"))
                  if not f.endswith(".expect.json"))


def reference_cases():
    import reference_parts as rp
    return rp.all_cases()


def fuzz_seeds():
    import fuzz_seeds_smoke as fss
    return list(fss.SEEDS)


def load_corpus(path=CORPUS):
    """The corpus rows, checked: each kind's every source is listed (a new
    reference case, golden or seed must join), and nothing else is."""
    with open(path) as f:
        corpus = json.load(f)
    if corpus.get("fuzz_generator") != fd.GENERATOR_VERSION:
        raise CorpusError(
            f"the corpus's fuzz designs are generator "
            f"{corpus.get('fuzz_generator')}, fuzz_design.py is "
            f"{fd.GENERATOR_VERSION}: set fuzz_generator and re-bless the "
            f"baseline (tools/action_matrix.py run --bless)")
    rows = corpus["designs"]
    names = set()
    for r in rows:
        if set(r) - ROW_KEYS or not {"design", "kind", "source"} <= set(r):
            raise CorpusError(f"corpus row {r}: keys are {sorted(ROW_KEYS)}, "
                              "design, kind and source required")
        if r["kind"] not in KINDS:
            raise CorpusError(f"corpus row {r['design']}: kind {r['kind']!r}"
                              f"; accepted: {list(KINDS)}")
        if r["design"] in names:
            raise CorpusError(f"corpus lists {r['design']} twice")
        names.add(r["design"])
    want = ({f"reference/{c}" for c in reference_cases()} |
            {f"fuzz/{s}" for s in fuzz_seeds()} |
            {"golden/" + os.path.basename(g)[:-5] for g in golden_files()})
    have = {r["design"] for r in rows if r["kind"] != "generator"}
    missing, extra = sorted(want - have), sorted(have - want)
    if missing or extra:
        raise CorpusError(
            "tests/action_matrix/corpus.json is out of step with its sources"
            + (f"; not listed: {missing}" if missing else "")
            + (f"; listed but gone: {extra}" if extra else "")
            + ": add or remove the rows (and re-bless)")
    for r in rows:
        if r["kind"] == "generator" and not os.path.isfile(
                os.path.join(ROOT, r["source"])):
            raise CorpusError(f"{r['design']}: no generator {r['source']}")
    return corpus


def sample_designs(rows):
    seeds = set(fuzz_seeds()[:SAMPLE_FUZZ_SEEDS])
    return [r for r in rows if r["kind"] == "reference"
            or (r["kind"] == "fuzz" and int(r["source"]) in seeds)
            or r["design"] in SAMPLE_GENERATORS]


# --- an instance ----------------------------------------------------------
# {method: [calls, seconds]} for every RPC this process sent: each design's
# meta holds its share (`rpc`), and `run --profile` adds them up.
RPC_TIMES = {}


class Instance:
    """One headless app: smoke_app's isolation (scratch XDG dirs,
    LOBOLE_EXIT_WITH_PARENT), autosave off, a per-call timeout."""

    def __init__(self, binary, timeout=ACTION_TIMEOUT_S):
        from smoke_app import launch_app
        from lobole.client import Client
        self.app = launch_app(binary, extra_args=["--autosave", "0"])
        self.client = Client.wait_for(self.app.port, strict=True)
        self.timeout = timeout
        self.port = self.app.port

    def request(self, method, params=None, timeout=None):
        t = time.time()
        try:
            return self.client.request(method, params or {},
                                       timeout=timeout or self.timeout)
        finally:
            # Where a run's time goes, per RPC (`run --profile`): the
            # matrix's own round trips are most of it.
            c = RPC_TIMES.setdefault(method, [0, 0.0])
            c[0] += 1
            c[1] += time.time() - t

    def call(self, method, /, **params):
        return self.request(method, params)

    def stop(self):
        try:
            self.client.close()
        except Exception:  # noqa: BLE001
            pass
        self.app.terminate()  # by its pid, the Popen's
        try:
            self.app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.app.kill()
            self.app.wait(timeout=10)

    def log_tail(self, n=40):
        return "\n".join(self.app.log().splitlines()[-n:])


class PrepareError(Exception):
    """The design did not build or open: it is skipped, with this reason."""


def prepare(inst, row, work):
    """Build or open `row` in `inst`; returns the part tabs to visit (None:
    the active one)."""
    kind, src = row["kind"], row["source"]
    if kind == "reference":
        import reference_parts as rp
        case = rp.load_case(os.path.basename(src))
        if case.get("known_failure"):
            raise PrepareError(f"known_failure: {case['known_failure']}")
        inst.call("new_project")
        try:
            rp.build(inst, case)
        except rp.BuildFailure as e:
            raise PrepareError(f"does not build: {e}")
        return [None]
    if kind == "fuzz":
        build_fuzz(inst, int(src))
        return [None]
    if kind == "golden":
        try:
            inst.call("open_project", path=os.path.join(ROOT, src))
        except Exception as e:  # noqa: BLE001 -- a refusal to open
            raise PrepareError(f"does not open: {e}")
        st = inst.call("get_state")
        if not st["regen"]["ok"]:
            raise PrepareError("regen not clean: " + "; ".join(
                f"{f.get('name')}: {f.get('error')}"
                for f in st["regen"]["failed"])[:300])
        if not st["bodies"]:
            raise PrepareError("no body")
        return [None]
    # A generator, into a new project in this instance, writing into a
    # private directory.
    inst.call("new_project")
    out = os.path.join(work, "gen-" + row["design"].replace("/", "_"))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    cmd = [sys.executable, os.path.join(ROOT, src), "--port", str(inst.port)]
    cmd += [a.replace("{out}", out) for a in row.get("args", [])]
    env = dict(os.environ, LOBOLE_SCRATCH=out,
               PYTHONPATH=os.path.join(ROOT, "python"))
    try:
        r = subprocess.run(cmd, cwd=out, env=env, capture_output=True,
                           text=True, timeout=300)
    except subprocess.TimeoutExpired:
        raise PrepareError(f"{src} took more than 300 s")
    if r.returncode != 0:
        raise PrepareError(f"{src} exited {r.returncode}: "
                           + (r.stderr or r.stdout)[-400:])
    parts = row.get("parts") or [None]
    have = {p["name"] for p in inst.call("list_parts")["parts"]}
    gone = [p for p in parts if p is not None and p not in have]
    if gone:
        raise PrepareError(f"{src} made no part tab {gone}; it made "
                           f"{sorted(have)}")
    return parts


_FUZZ_DESIGNS = {}


def build_fuzz(inst, seed):
    """Seed's design built in a new project. The first time in a process it
    is generated as it builds (generate_and_run's loop, without its end
    checks and identities, which fuzz_seeds_smoke runs); after that the
    steps are replayed (run_design's loop), as a restart does."""
    inst.call("new_project")
    run = fd.Runner(inst)
    before = []
    try:
        if seed in _FUZZ_DESIGNS:
            for step in _FUZZ_DESIGNS[seed]:
                sent, after, limit = run.run_step(step["k"], step, before)
                if limit is None:
                    before = after
            return
        gen = fd.Generator(seed)
        design = []
        step = gen.base()
        n_steps = gen.rng.randint(1, 8)
        last = None
        for k in range(n_steps + 1):
            if k > 0:
                step = gen.next(k, fd.summarise(run.elements(), before), last)
                if step is None:
                    break
            step["k"] = k
            design.append(step)
            sent, after, limit = run.run_step(k, step, before)
            if limit is not None:
                step["limit"] = limit
                continue
            before = after
            last = step
        _FUZZ_DESIGNS[seed] = design
    except fd.Failure as f:
        raise PrepareError(f"fuzz seed {seed} does not build: {f.cls}: "
                           f"{f.message}")


def unit_name(row, part):
    return row["design"] if part is None else f"{row['design']}#{part}"


def units_of(row):
    return [unit_name(row, p) for p in (row.get("parts") or [None])]


def in_shard(unit, shard):
    """Shards split the corpus by a stable hash of each design's name. A
    generator's design is in every shard (see action_in_shard)."""
    if not shard:
        return True
    if unit.startswith("generator/"):
        return True
    k, n = (int(x) for x in shard.split("/"))
    return stable_hash(unit) % n == k


def action_in_shard(unit, key, shard):
    """A generator's parts hold most of the corpus's entities (the gear
    train's wheel alone is ten minutes of actions), so every shard builds
    it and takes the actions on the families or entities that hash to it."""
    if not shard or not unit.startswith("generator/"):
        return True
    k, n = (int(x) for x in shard.split("/"))
    return stable_hash(unit, key) % n == k


# --- entities and the actions on them -------------------------------------
def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _sub(a, b):
    return [x - y for x, y in zip(a, b)]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def face_frame(face):
    """The frame a Sketch with face_ref takes (CADShape::FacePlane): the
    centroid, the outward normal, world Y projected as u unless the normal
    is near Y, then world X."""
    n = face["normal"]
    u = [0.0, 1.0, 0.0] if abs(n[1]) < 0.9 else [1.0, 0.0, 0.0]
    d = _dot(u, n)
    u = [a - d * b for a, b in zip(u, n)]
    m = math.sqrt(_dot(u, u))
    u = [a / m for a in u]
    return face["centroid"], n, u, _cross(n, u)


FALLBACK_U = re.compile(r";U\d+")


def family_of(kind, el):
    """The key an entity is judged by. A name by POSITION can swap with a
    sibling between runs, so siblings share a family and are judged by how
    many of them pass:

    - a name holding a fallback-numbered boundary edge (`;U<k>`) anywhere:
      the number follows the order the kernel hands a face's edges back,
      which follows shape addresses, and so does which of an edge's two
      faces names it first
      (docs/known-bugs.md#a-blend-faces-edge-names-depend-on-what-the-process-did-before).
      Its family is every such name of the feature that made it,
      `edge:<feature>/*;U*`;
    - a piece whose ordinal is all that tells it from its siblings;
    - a feature that names by index (role `positional`)."""
    name = el["name"]
    if FALLBACK_U.search(name):
        return f"{kind}:{name.split('/')[0]}/*;U*"
    piece = el.get("piece") or {}
    if piece.get("ordinal"):
        return f"{kind}:{piece.get('of', name)}[*]"
    if el.get("role") == "positional":
        return f"{kind}:" + re.sub(r"\d+$", "*", name)
    return None


def round125(raw):
    """1, 2 or 5 x 10^n at or below raw > 0: the app's derived defaults are
    rounded so (ui::SizeFromBody, ui::SizeFromFace)."""
    decade = 10.0 ** math.floor(math.log10(raw))
    mant = raw / decade
    return decade * (5.0 if mant >= 5.0 else 2.0 if mant >= 2.0 else 1.0)


def plan_actions(els, state, grow=None):
    """[action] for every body, face and edge of the active part, and the
    entities that could not be tried {unnamed_faces, capped}. An action is
    {entity, family, action, calls, expect, check?}. `grow(refs)` is the
    app's grow_faces, which the direct edits that take a whole feature (Add
    the feature) are planned through; without it they are not planned."""
    features = state.get("features", [])
    order = {f["id"]: i for i, f in enumerate(features)}
    bodies = state.get("bodies", [])
    out, info = [], {"unnamed_faces": 0, "capped_bodies": []}
    # A reference is {feature_id: the body's created_by, element}: bodies
    # that share a created_by (a pattern's copies, a split's pieces) cannot
    # be named alone, nor can an element name two of them both carry.
    by_cb, names = {}, {}
    for b in els.get("bodies", []):
        by_cb.setdefault(b["created_by"], []).append(b)
        for kind in ("face", "edge"):
            for el in b[kind + "s"]:
                names[(kind, el["name"])] = names.get((kind, el["name"]), 0) + 1
    for b in els.get("bodies", []):
        cb = b["created_by"]
        shared = len(by_cb[cb]) > 1
        info["unnamed_faces"] += b.get("unnamed_faces", 0)
        if len(b["faces"]) > MAX_FACES or len(b["edges"]) > MAX_EDGES:
            info["capped_bodies"].append(
                f"{b.get('name')} ({len(b['faces'])} faces, "
                f"{len(b['edges'])} edges)")
            continue
        if b.get("body", -1) >= len(bodies):
            continue
        dg = bodies[b["body"]]
        if shared:
            info["shared_bodies"] = info.get("shared_bodies", 0) + 1
            twins = [o for o in by_cb[cb] if o is not b]
            theirs = {(k, el["name"]) for o in twins for k in ("face", "edge")
                      for el in o[k + "s"]}
            b = dict(b, faces=[f for f in b["faces"]
                               if ("face", f["name"]) not in theirs],
                     edges=[e for e in b["edges"]
                            if ("edge", e["name"]) not in theirs])
        lo, hi = dg["bbox_min"], dg["bbox_max"]
        ext = [h - l for h, l in zip(hi, lo)]
        size, least = max(ext), min(ext)
        faces = {f["name"]: f for f in b["faces"]}
        area = {n: f.get("area_mm2", 0.0) for n, f in faces.items()}

        def add(kind, el, action, calls, expect, check=None):
            if kind == "body":
                if shared:
                    return
                key = f"body:{cb}"
            elif names.get((kind, el["name"]), 0) > 1:
                key = f"{kind}@{b['body']}:{el['name']}"
            else:
                key = f"{kind}:{el['name']}"
            out.append({"entity": key,
                        "family": family_of(kind, el) if kind != "body"
                        else None,
                        "action": action, "calls": calls, "expect": expect,
                        "check": check})

        def ref(el):
            return {"feature_id": cb, "element": el["name"]}

        def feature(f):
            return {"method": "add_feature", "params": {"feature": f}}

        # Edges.
        for e in b["edges"]:
            length = e.get("length_mm", 0.0)
            if length <= 0:
                continue
            circular = e.get("kind") in ("circle", "arc")
            if not e.get("smooth", False):
                lim = [length] + [math.sqrt(area[n]) for n in e.get("faces", [])
                                  if area.get(n, 0) > 0]
                if circular and e.get("radius"):
                    lim.append(e["radius"])
                r = round(0.1 * min(lim), 6)
                if r > 0:
                    for t, a in (("Fillet", "edge.fillet"),
                                 ("Chamfer", "edge.chamfer")):
                        add("edge", e, a, [feature({
                            "type": t, "name": f"AM {t.lower()}",
                            "edges": [ref(e)], "radius": r})], "any")
            if circular:
                add("edge", e, "edge.frame", [feature({
                    "type": "Frame", "name": "AM frame",
                    "kind": "from_element", "elements": [ref(e)]})], "same")
        # Faces.
        planar = [f for f in b["faces"] if f.get("kind") == "plane"
                  and "normal" in f and "centroid" in f]
        shell_on = None
        if len(b["faces"]) > BIG_BODY_FACES:
            shell_on = {f["name"] for f in sorted(
                planar, key=lambda f: stable_hash(f["name"]))[
                    :SHELLS_ON_BIG_BODY]}
            info["shells_sampled"] = info.get("shells_sampled", 0) + len(
                planar) - len(shell_on)
        for f in b["faces"]:
            if shell_on is None or f["name"] in shell_on:
                direct_actions(f, cb, lo, hi, ext, least, add, ref,
                               feature)
        if grow is not None:
            feature_actions(b, cb, ext, shell_on, add, ref, feature, grow)
        for f in b["faces"]:
            kind = f.get("kind")
            if f in planar:
                plane_actions(f, b, faces, cb, size, least, add, ref, feature,
                              shell=shell_on is None or f["name"] in shell_on)
            elif kind == "cylinder" and f.get("radius"):
                dia = 2.0 * f["radius"]
                best = min(ISO_COARSE, key=lambda t: abs(t[1] - dia))
                if abs(best[1] - dia) <= 0.05 * dia:
                    add("face", f, "curved.thread", [feature({
                        "type": "Thread", "name": "AM thread", "face": ref(f),
                        "designation": best[0], "modeled": False})], "same")
                add("face", f, "curved.frame", [feature({
                    "type": "Frame", "name": "AM frame",
                    "kind": "from_element", "elements": [ref(f)]})], "same")
        # The body.
        made = [f.get("made_by") for f in b["faces"]] + [cb]
        newest = max((m for m in made if m in order), key=lambda m: order[m],
                     default=cb)
        bel = {"name": str(cb)}
        if ext[0] > 0:
            add("body", bel, "body.pattern_linear", [feature({
                "type": "Pattern", "name": "AM pattern", "kind": "linear",
                "source_id": newest, "axis": {"kind": "x"}, "count": 2,
                "spacing": round(1.5 * ext[0], 6)})], "any")
        add("body", bel, "body.pattern_circular", [feature({
            "type": "Pattern", "name": "AM pattern", "kind": "circular",
            "source_id": newest, "axis": {"kind": "z"}, "count": 3,
            "angle_deg": 120.0})], "any")
        add("body", bel, "body.mirror", [feature({
            "type": "Pattern", "name": "AM mirror", "kind": "mirror",
            "source_id": newest, "base_plane": "yz"})], "any")
        axis = ext.index(size)
        add("body", bel, "body.split", [feature({
            "type": "Split", "name": "AM split", "body_id": cb,
            "knife": "plane", "base_plane": ("yz", "xz", "xy")[axis],
            "offset": round(0.5 * (lo[axis] + hi[axis]), 6)})], "same")
    return out, info


WORLD = ((0, "yz"), (1, "xz"), (2, "xy"))


def direct_actions(f, cb, lo, hi, ext, least, add, ref, feature):
    """The direct edits of one face (docs/modeling/README.md#direct-edit), at
    the app's derived defaults: deleted alone; offset out and in by a tenth of
    the smaller of how far its inside reaches (inset_mm; a cylinder's radius)
    and the body's thinnest extent; a planar face put onto the parallel plane
    a twentieth of the body past its far extent along the face's normal."""
    add("face", f, "face.delete", [feature({
        "type": "DeleteFace", "name": "AM delete", "body_id": cb,
        "faces": [ref(f)]})], "any")
    kind = f.get("kind")
    reach = (f.get("inset_mm") if kind == "plane" else
             f.get("radius") if kind == "cylinder" else None)
    if reach and reach > 0 and least > 0:
        d = round125(0.1 * min(reach, least))
        for action, dist, expect in (("face.offset_out", d, "add"),
                                     ("face.offset_in", -d, "cut")):
            add("face", f, action, [feature({
                "type": "OffsetFace", "name": "AM offset", "body_id": cb,
                "faces": [ref(f)], "distance_mm": dist})], expect)
    if kind == "plane" and "normal" in f and "centroid" in f:
        c, n, u, v = face_frame(f)
        # How far the body reaches past the face along its normal: the
        # farthest corner of its box.
        corners = [[(hi[i] if (k >> i) & 1 else lo[i]) for i in range(3)]
                   for k in range(8)]
        far = round(max(_dot(_sub(cc, c), n) for cc in corners) +
                    0.05 * max(ext), 6)
        if far > 0:
            o = [c[i] + n[i] * far for i in range(3)]
            pts = [o, [o[i] + u[i] for i in range(3)],
                   [o[i] + v[i] for i in range(3)]]
            add("face", f, "face.replace", [feature({
                "type": "DatumPlane", "name": "AM target",
                "kind": "through_points",
                "points": [[round(x, 6) for x in p] for p in pts]}),
                feature({
                    "type": "ReplaceFace", "name": "AM replace",
                    "body_id": cb, "face": ref(f),
                    "target": {"kind": "plane", "datum_id": {"$ref": 1}}})],
                "any")


def feature_actions(b, cb, ext, shell_on, add, ref, feature, grow):
    """Delete and Move Face of whole features: each face grown to the faces
    of the hole or boss it belongs to (grow_faces, the GUI's Add the
    feature), one row per feature, on its first face by name. A face that
    does not grow (a fillet, a face of the body itself) plans the growth
    alone, which refuses, saying why. The move is a tenth of the body's
    largest extent in the plane of the face the feature sits in."""
    from lobole.errors import Refused
    seen = set()
    for f in b["faces"]:
        if shell_on is not None and f["name"] not in shell_on:
            continue
        try:
            got = grow([ref(f)])
        except Refused:
            add("face", f, "face.delete_feature", [{
                "method": "grow_faces", "params": {"faces": [ref(f)]}}],
                "any")
            continue
        names = sorted(g["element"] for g in got["faces"])
        if tuple(names) in seen or names[0] != f["name"]:
            continue
        seen.add(tuple(names))
        refs = [ref({"name": n}) for n in names]
        add("face", f, "face.delete_feature", [feature({
            "type": "DeleteFace", "name": "AM delete feature", "body_id": cb,
            "faces": refs})], "any")
        hn = got.get("host_normal")
        axes = [a for a in range(3) if hn is None or abs(hn[a]) <= 0.5]
        if not axes:
            continue
        axis = max(axes, key=lambda a: ext[a])
        if ext[axis] <= 0:
            continue
        t = [0.0, 0.0, 0.0]
        t[axis] = round125(0.1 * ext[axis])
        add("face", f, "face.move", [feature({
            "type": "MoveFace", "name": "AM move", "body_id": cb,
            "faces": refs, "translate_mm": t})], "same")


def plane_actions(f, b, faces, cb, size, least, add, ref, feature,
                  shell=True):
    c, n, u, v = face_frame(f)
    p, r0 = f.get("inside_point"), f.get("inset_mm", 0.0)
    if p is not None and r0 and r0 > 0:
        local = [round(_dot(_sub(p, c), u), 6), round(_dot(_sub(p, c), v), 6)]
        plane = {"origin": c, "normal": n, "uAxis": u}

        def sketch(entities):
            return feature({"type": "Sketch", "name": "AM sketch",
                            "plane": plane, "face_ref": ref(f),
                            "entities": entities})
        circle = [{"type": "circle", "center": local,
                   "radius": round(0.5 * r0, 6)}]
        frame = {"origin": c, "normal": n, "uAxis": u, "point": p}
        add("face", f, "face.sketch", [sketch(circle)], "same",
            {"sketch_plane": frame})
        dist = round(0.05 * size, 6)
        if dist > 0:
            add("face", f, "face.extrude_add", [sketch(circle), feature({
                "type": "Extrude", "name": "AM extrude",
                "sketch_id": {"$ref": 1}, "distance": dist, "mode": "add"})],
                "add")
            add("face", f, "face.extrude_cut", [sketch(circle), feature({
                "type": "Extrude", "name": "AM extrude",
                "sketch_id": {"$ref": 1}, "distance": dist, "mode": "cut",
                "reversed": True})], "cut")
        add("face", f, "face.hole", [
            sketch([{"type": "point", "p": local}]), feature({
                "type": "Hole", "name": "AM hole", "hole_kind": "simple",
                "sketch_id": {"$ref": 1}, "diameter": round(r0, 6),
                "through": True})], "cut")
    # The Shell tool's default (ui::SizeFromWall): a tenth of the body's
    # thinnest wall, else of its least extent, rounded down to 1, 2 or 5.
    wall = b.get("thinnest_wall_mm") or least
    t = round125(0.1 * wall) if wall and wall > 0 else 0
    if t > 0 and shell:
        add("face", f, "face.shell", [feature({
            "type": "Shell", "name": "AM shell", "body_id": cb,
            "thickness": t, "open_faces": [ref(f)]})], "cut")
    # Draft: the faces next to it that stand square to it, about the world
    # plane square to it.
    neutral = next((name for i, name in WORLD if abs(abs(n[i]) - 1) < 1e-9),
                   None)
    if neutral:
        side = set()
        for e in b["edges"]:
            if f["name"] not in e.get("faces", []):
                continue
            for other in e["faces"]:
                g = faces.get(other)
                if g is None or other == f["name"]:
                    continue
                if (g.get("kind") == "plane" and "normal" in g and
                        abs(_dot(g["normal"], n)) < 1e-9) or (
                        g.get("kind") == "cylinder" and "axis" in g and
                        abs(abs(_dot(g["axis"], n)) - 1) < 1e-9):
                    side.add(other)
        if side:
            add("face", f, "face.draft", [feature({
                "type": "Draft", "name": "AM draft", "body_id": cb,
                "angle_deg": 2.0, "neutral_plane": neutral,
                "faces": [ref({"name": s}) for s in sorted(side)]})], "any")
    add("face", f, "face.frame", [feature({
        "type": "Frame", "name": "AM frame", "kind": "from_element",
        "elements": [ref(f)]})], "same")


def selected(unit, action, family_or_entity, every):
    return stable_hash(unit, action, family_or_entity) % every == 0


SLOW_S = 5.0  # an action past this is in the baseline's `slow` table
SLOW_MARGIN = 10.0  # ...and gets this many times its seconds before a hang


def slow_times(baseline):
    """{(unit, key, action): seconds} from the baseline's `slow` table (0 for
    a known hang, which keeps the ordinary deadline)."""
    return {(u, *k.rsplit(" ", 1)): secs
            for u, rows in baseline.get("slow", {}).items()
            for k, secs in rows.items()}


def slow_set(baseline):
    return set(slow_times(baseline))


def deadline_for(slow, unit, key, action):
    """An action the baseline timed at s seconds may take SLOW_MARGIN x s
    before it is a hang, never less than ACTION_TIMEOUT_S: a 22 s extrude on
    a loaded machine is not a hang."""
    return max(ACTION_TIMEOUT_S,
               SLOW_MARGIN * slow.get((unit, key, action), 0.0))


def in_scope(unit, action, key, every, actions, slow):
    """Whether a run tries `action` on `key` of `unit`: the --actions
    filter, the sampling hash, and -- when sampling -- not an action the
    baseline timed past SLOW_S, which only the whole sweep spends on."""
    if actions is not None and action not in actions:
        return False
    if every == 1:
        return True
    return selected(unit, action, key, every) and (unit, key, action) \
        not in slow


# --- the classifier --------------------------------------------------------
def bodies_of(src):
    st = src.request("get_state", {})
    return st, [{k: b.get(k) for k in ("created_by", "name", "volume_mm3",
                                       "bbox_min", "bbox_max")}
                for b in st.get("bodies", [])]


def body_digest(bodies, before=()):
    """A result's identity: each body's volume and bbox, a body the action
    made named `new` (its feature's id is the next free one, which a second
    run of the same action does not get)."""
    old = {b["created_by"] for b in before}
    return sorted([str(b["created_by"]) if b["created_by"] in old else "new",
                   b["volume_mm3"], b["bbox_min"], b["bbox_max"]]
                  for b in bodies)


def same_digest(a, b, rel=1e-9):
    if a is None or b is None or len(a) != len(b):
        return a == b
    for x, y in zip(a, b):
        if x[0] != y[0]:
            return False
        scale = max(abs(x[1]), 1.0)
        if abs(x[1] - y[1]) > rel * scale:
            return False
        size = max([abs(v) for v in x[2] + x[3]] + [1.0])
        for k in (2, 3):
            if any(abs(p - q) > max(rel * size, 1e-7)
                   for p, q in zip(x[k], y[k])):
                return False
    return True


def tight_box(src):
    bodies = src.request("mass_properties", {}).get("bodies", [])
    boxes = [(b["bbox_min"], b["bbox_max"]) for b in bodies if "bbox_min" in b]
    if not boxes:
        return None
    return ([min(b[0][i] for b in boxes) for i in range(3)],
            [max(b[1][i] for b in boxes) for i in range(3)])


class Context:
    """The part as it stands before every action: what an action is judged
    against, and what its undo must give back."""

    def __init__(self, src):
        self.refresh(src)

    def refresh(self, src):
        self.state, self.bodies = bodies_of(src)
        self.revision = self.state.get("revision", {}).get("version")
        self.features = len(self.state.get("features", []))
        self._box = None
        self._src = src

    def box(self):
        if self._box is None:
            self._box = tight_box(self._src) or ([0, 0, 0], [0, 0, 0])
        return self._box


def _message(e):
    return str(e)


def outcome(kind, cls="", message="", digest=None):
    return {"outcome": kind, "class": cls, "message": str(message)[:600],
            "digest": digest}


def judge_message(message):
    """A failure's message as a finding, or None when it is one a user can
    act on."""
    if SIGNAL.search(str(message)):
        return "signal"
    if not fd.measured(message):
        return "message"
    return None


def run_action(src, act, ctx):
    """One action on `src` (a client, or the self-test's fake), judged; the
    part is put back with `undo`. Returns (outcome dict, restore) where
    restore says the part is no longer as `ctx` has it ('rebuild') or the
    instance is gone ('restart')."""
    from lobole.errors import NonFiniteError, Refused
    calls = act["calls"]
    batch = len(calls) > 1
    method, params = (("batch", {"calls": calls}) if batch else
                      (calls[0]["method"], calls[0]["params"]))
    # The box a "cut" is held inside is the part's BEFORE the action: taken
    # lazily, the first cut of a unit measured its own result, and every cut
    # after it was judged against that (an Offset Face in that shrinks the
    # box read as growing it).
    if act["expect"] == "cut":
        ctx.box()
    try:
        answer = src.request(method, params)
    except TimeoutError as e:
        return outcome("finding", "hang", f"{method}: {e}"), "restart"
    except NonFiniteError as e:
        return outcome("finding", "nonfinite", e), "rebuild"
    except (ConnectionError, OSError) as e:
        return outcome("finding", "crash", f"{method}: {e}"), "restart"
    except Refused as e:
        return judge_refusal(src, e, batch, ctx)
    try:
        result = judge_accepted(src, act, answer, ctx)
    except TimeoutError as e:
        return outcome("finding", "hang", f"after {method}: {e}"), "restart"
    except NonFiniteError as e:
        return outcome("finding", "nonfinite", e), "rebuild"
    except (ConnectionError, OSError) as e:
        return outcome("finding", "crash", f"after {method}: {e}"), "restart"
    except Refused as e:
        return (outcome("finding", "internal",
                        f"a check after {method} was refused: {e}"),
                "rebuild")
    # Give the part back. The undo answers with the bodies' digest, its
    # feature count and the part's revision (an app before that is asked
    # get_state): a full get_state per action was most of the matrix's own
    # time.
    try:
        ans = src.request("undo", {})
        ans = ans if isinstance(ans, dict) else {}
        if "digest" in ans and "feature_count" in ans:
            after = [{k: b.get(k) for k in ("created_by", "name",
                                           "volume_mm3", "bbox_min",
                                           "bbox_max")}
                     for b in ans["digest"].get("bodies", [])]
            st = {"features": [None] * ans["feature_count"],
                  "revision": ans.get("revision") or {}}
        else:
            st, after = bodies_of(src)
    except TimeoutError as e:
        return outcome("finding", "hang", f"undo: {e}"), "restart"
    except (ConnectionError, OSError) as e:
        return outcome("finding", "crash", f"undo: {e}"), "restart"
    except Refused as e:
        return outcome("finding", "undo", f"undo refused: {e}"), "rebuild"
    try:
        fd.same_bodies(ctx.bodies, after, 1e-9, "undo")
        if len(st.get("features", [])) != ctx.features:
            raise fd.Failure("undo", f"{len(st.get('features', []))} "
                             f"features after the undo, {ctx.features} "
                             "before the action")
    except fd.Failure as f:
        return outcome("finding", "undo", f"the undo did not restore the "
                       f"part: {f.message}", result["digest"]), "rebuild"
    ctx.revision = st.get("revision", {}).get("version")
    return result, None


def judge_refusal(src, e, batch, ctx):
    diag = getattr(e, "diagnostic", None) or {}
    msg = _message(e)
    if diag.get("code") == "internal" or fd._deep_internal(diag):
        return outcome("finding", "internal", msg), "rebuild"
    bad = judge_message(msg)
    if bad:
        return outcome("finding", bad, msg), "rebuild"
    # A refused call changes nothing. A batch's rollback is its edits and
    # their undo, so its revision moves; what it holds must not.
    try:
        st, now = bodies_of(src)
    except (ConnectionError, OSError) as x:
        return outcome("finding", "crash", f"get_state: {x}"), "restart"
    changed = None
    if not batch and st.get("revision", {}).get("version") != ctx.revision:
        changed = (f"the revision moved from {ctx.revision} to "
                   f"{st.get('revision', {}).get('version')}")
    elif len(st.get("features", [])) != ctx.features:
        changed = (f"{len(st.get('features', []))} features after it, "
                   f"{ctx.features} before")
    else:
        try:
            fd.same_bodies(ctx.bodies, now, 1e-9, "refused")
        except fd.Failure as f:
            changed = f.message
    if changed:
        return outcome("fail", "refusal_changed",
                       f"refused ({msg}) but changed the part: {changed}"), \
            "rebuild"
    # A batch's rollback moved the revision (above): the next single call is
    # judged from where it now stands, or its honest refusal reads as one
    # that changed the part.
    ctx.revision = st.get("revision", {}).get("version")
    return outcome("refuse", "", msg), None


def judge_accepted(src, act, answer, ctx):
    answer = answer if isinstance(answer, dict) else {}
    regen = answer.get("regen", {}) or {}
    failed = [f for f in regen.get("failed", [])
              if not f.get("after_earlier_failure")]
    if failed:
        msg = "; ".join(f"{f.get('name')}: {f.get('error') or f.get('message')}"
                        for f in failed)
        for f in failed:
            bad = judge_message(f.get("error") or f.get("message") or "")
            if bad:
                return outcome("finding", bad, msg)
        return outcome("fail", "broken", msg)
    # A warning the part already carried (a design's own Draft that warns)
    # is not this action's.
    had = {(w.get("name"), w.get("warning"))
           for w in (ctx.state.get("regen", {}) or {}).get("warnings", [])}
    warned = [w for w in regen.get("warnings", [])
              if (w.get("name"), w.get("warning")) not in had]
    if warned:
        return outcome("fail", "warned", "; ".join(
            f"{w.get('name')}: {w.get('warning')}" for w in warned))
    geometry = src.request("check_geometry", {})
    st, after = bodies_of(src)
    digest = body_digest(after, ctx.bodies)
    if not geometry.get("valid", True):
        bad = [(b.get("name"), b.get("reasons")) for b in
               geometry.get("bodies", []) if not b.get("valid", True)]
        return outcome("fail", "invalid", f"check_geometry: {bad}", digest)
    part_volume = sum(b["volume_mm3"] for b in ctx.bodies) or 1.0
    delta = sum(b["volume_mm3"] for b in after) - sum(
        b["volume_mm3"] for b in ctx.bodies)
    expect = act["expect"]
    step = {"op": act["action"],
            "expect": expect if expect in ("cut", "add") else "any"}
    boxes = (ctx.box(), tight_box(src)) if expect == "cut" else None
    try:
        fd.check_step(step, ctx.bodies,
                      {"regen": regen, "digest": {
                          "bodies": after, "delta": {"volume_mm3": delta}}},
                      geometry, part_volume, boxes=boxes)
    except fd.Failure as f:
        if f.cls in ("nonfinite", "message"):
            return outcome("finding", f.cls, f.message, digest)
        return outcome("fail", f.cls, f.message, digest)
    # To 1e-5 of the part: the pieces of a split are integrated over new
    # faces, and a spline-bounded body's volume moves in the sixth digit.
    if expect == "same" and abs(delta) > 1e-5 * max(part_volume, 1.0):
        return outcome("fail", "wrong_way", f"a {act['action']} changed the "
                       f"volume by {delta:.6g} mm3", digest)
    check = act.get("check") or {}
    if "sketch_plane" in check:
        want = check["sketch_plane"]
        sk = st.get("features", [])[-1] if st.get("features") else {}
        pl = sk.get("plane", {})
        ok = (sk.get("type") == "Sketch" and pl and
              _dot(pl["normal"], want["normal"]) > 1 - 1e-9 and
              _dot(pl["uAxis"], want["uAxis"]) > 1 - 1e-9 and
              math.dist(pl["origin"], want["origin"]) < 1e-6)
        if not ok:
            return outcome("fail", "wrong_plane",
                           f"the sketch on the face is on {pl}, not the "
                           f"face's plane {want}", digest)
    return outcome("pass", "", "", digest)


# --- one design -------------------------------------------------------------
class Worker:
    """Runs designs on one instance, replacing it every REPLACE_EVERY designs
    and after a crash or a hang."""

    def __init__(self, binary, out, every, actions, fresh=False, shard=""):
        self.binary, self.out, self.every = binary, out, every
        self.shard = shard
        self.actions = actions
        self.times = slow_times(load_baseline())
        self.slow = set(self.times) if every != 1 else set()
        self.fresh = fresh
        self.inst = None
        self.used = 0
        self.work = tempfile.mkdtemp(prefix="action_matrix_")

    def instance(self):
        if self.inst is None or self.used >= REPLACE_EVERY or self.fresh:
            self.stop()
            self.inst = Instance(self.binary)
            self.used = 0
        self.used += 1
        return self.inst

    def restart(self):
        if self.inst is not None:
            try:
                self.inst.app.kill()  # by pid; it is hung or dead
                self.inst.app.wait(timeout=10)
            except Exception:  # noqa: BLE001
                pass
        self.inst = Instance(self.binary)
        self.used = 1
        return self.inst

    def stop(self):
        if self.inst is not None:
            self.inst.stop()
            self.inst = None

    def close(self):
        self.stop()
        shutil.rmtree(self.work, ignore_errors=True)

    def run_design(self, row):
        """Every unit (design, part) of `row`; writes <design>.jsonl and
        returns its meta record."""
        t0 = time.time()
        path = os.path.join(self.out, row["design"].replace("/", "__") + (
            f"@{self.shard.replace('/', 'of')}" if self.shard else "")
            + ".jsonl")
        meta = {"design": row["design"], "units": {}, "skipped": None}
        rows = []
        rpc_before = {m: list(v) for m, v in RPC_TIMES.items()}
        try:
            inst = self.instance()
            t = time.time()
            parts = prepare(inst, row, self.work)
            meta["prepare_s"] = round(time.time() - t, 3)
        except PrepareError as e:
            meta["skipped"] = str(e)
        except (ConnectionError, OSError) as e:
            meta["skipped"] = f"the instance died building it: {e}"
            self.stop()
        else:
            for part in parts:
                unit = unit_name(row, part)
                if not in_shard(unit, self.shard):
                    continue
                try:
                    rows += self.run_unit(row, part, unit, meta)
                except PrepareError as e:
                    meta["units"][unit] = {"skipped": str(e)}
                except (ConnectionError, OSError) as e:
                    meta["units"][unit] = {"skipped": f"instance died: {e}"}
                    self.stop()
        meta["seconds"] = round(time.time() - t0, 3)
        meta["rpc"] = {m: [v[0] - rpc_before.get(m, [0, 0.0])[0],
                           round(v[1] - rpc_before.get(m, [0, 0.0])[1], 3)]
                       for m, v in RPC_TIMES.items()
                       if v[0] != rpc_before.get(m, [0, 0.0])[0]}
        with open(path, "w") as f:
            f.write(json.dumps({"meta": meta}) + "\n")
            for r in rows:
                f.write(json.dumps(r) + "\n")
        return meta

    def run_unit(self, row, part, unit, meta):
        """One part tab: every action undone leaves the document as built,
        so the next tab needs no rebuild."""
        inst = self.inst
        if part is not None:
            inst.call("set_active_part", part=part)
        t = time.time()
        els = inst.request("list_elements", {"max_faces": 1000000,
                                             "walls": True}, timeout=300)
        ctx = Context(inst)
        plan, info = plan_actions(
            els, ctx.state,
            grow=lambda refs: inst.request("grow_faces", {"faces": refs}))
        info["list_s"] = round(time.time() - t, 3)
        named_edges = sum(len(b["edges"]) for b in els.get("bodies", []))
        edges = inst.call("query_geometry", element_type="edge",
                          max_results=0)["count"]
        info["unnamed_edges"] = max(0, edges - named_edges)
        info["entities"] = len({a["entity"] for a in plan})
        info["names"] = sorted({a["entity"] for a in plan})
        meta["units"][unit] = info
        todo = [a for a in plan
                if in_scope(unit, a["action"], a["family"] or a["entity"],
                            self.every, self.actions, self.slow)
                and action_in_shard(unit, a["family"] or a["entity"],
                                    self.shard)]
        out = []
        for act in todo:
            t = time.time()
            self.inst.timeout = deadline_for(
                self.times, unit, act["family"] or act["entity"],
                act["action"])
            result, restore = run_action(self.inst, act, ctx)
            self.inst.timeout = ACTION_TIMEOUT_S
            if restore == "restart":
                result["log"] = self.inst.log_tail()
                self.restart()
            if restore:
                self.reenter(row, part, ctx)
            out.append({"unit": unit, "entity": act["entity"],
                        "family": act["family"], "action": act["action"],
                        **result, "call": act["calls"],
                        "seconds": round(time.time() - t, 3)})
        return out

    def reenter(self, row, part, ctx):
        """The part again as it was: rebuilt, in this instance or a new one."""
        try:
            prepare(self.inst, row, self.work)
        except (ConnectionError, OSError):
            self.restart()
            prepare(self.inst, row, self.work)
        if part is not None:
            self.inst.call("set_active_part", part=part)
        ctx.refresh(self.inst)


def worker_main(args):
    corpus = load_corpus()
    rows = {r["design"]: r for r in corpus["designs"]}
    actions = set(args.actions.split(",")) if args.actions else None
    w = Worker(args.binary, args.out, args.every, actions, shard=args.shard)
    try:
        for name in args.designs.split("\n"):
            if name:
                w.run_design(rows[name])
    finally:
        w.close()
    return 0


# --- reading a run back ----------------------------------------------------
def read_run(out):
    """{design: meta}, [row] from a run directory."""
    metas, rows = {}, []
    for path in sorted(glob.glob(os.path.join(out, "*.jsonl"))):
        with open(path) as f:
            for i, line in enumerate(f):
                rec = json.loads(line)
                if i == 0:
                    m = rec["meta"]
                    have = metas.setdefault(m["design"], m)
                    if have is not m:  # another shard's units of it
                        have["units"].update(m["units"])
                        have["skipped"] = have["skipped"] or m["skipped"]
                else:
                    rows.append(rec)
    return metas, rows


def tokens(results):
    """{unit: {key: token line}} from rows: a plain entity's actions, a
    family's counts. Findings are not in the lines (the `known` table holds
    them)."""
    plain, fam = {}, {}
    for r in results:
        if r["outcome"] == "finding":
            continue
        if r.get("family"):
            c = fam.setdefault(r["unit"], {}).setdefault(
                r["family"], {}).setdefault(r["action"],
                                            {"pass": 0, "fail": 0,
                                             "refuse": 0})
            c[r["outcome"]] += 1
        else:
            plain.setdefault(r["unit"], {}).setdefault(r["entity"], {})[
                r["action"]] = r["outcome"]
    lines = {}
    prefix = {"pass": "", "fail": "!", "refuse": "-"}
    for unit, ents in plain.items():
        for key, acts in ents.items():
            lines.setdefault(unit, {})[key] = " ".join(
                prefix[o] + a for a, o in sorted(acts.items()))
    for unit, fams in fam.items():
        for key, acts in fams.items():
            toks = []
            for a, c in sorted(acts.items()):
                for o in ("pass", "fail", "refuse"):
                    if c[o]:
                        toks.append(f"{prefix[o]}{a}*{c[o]}")
            lines.setdefault(unit, {})[key] = " ".join(toks)
    return lines


def parse_line(line):
    """{action: outcome} for a plain line; {action: {pass, fail, refuse}}
    for a family's."""
    out = {}
    back = {"": "pass", "!": "fail", "-": "refuse"}
    for tok in line.split():
        pre = tok[0] if tok[0] in "!-" else ""
        body = tok[len(pre):]
        if "*" in body:
            a, n = body.rsplit("*", 1)
            out.setdefault(a, {"pass": 0, "fail": 0, "refuse": 0})[
                back[pre]] += int(n)
        else:
            out[body] = back[pre]
    return out


# --- the ratchet --------------------------------------------------------
RANK = {"pass": 2, "refuse": 1, "fail": 0}


def judge(baseline, metas, results, scope, occ):
    """(regressions, improvements, notes, findings). `scope(unit, action,
    family_or_key)` says whether this run would have tried that action."""
    regressions, improvements, notes = [], [], []
    same_kernel = occ_series(occ) == baseline.get("occ")
    designs = baseline.get("designs", {})
    skipped = baseline.get("skipped", {})
    new_lines = tokens(results)
    for name, meta in metas.items():
        units = meta.get("units", {})
        if meta.get("skipped"):
            if name in skipped:
                continue
            if any(u == name or u.startswith(name + "#") for u in designs):
                regressions.append(f"{name}: no longer builds "
                                   f"({meta['skipped']})")
            else:
                notes.append(f"{name}: skipped ({meta['skipped']}), not in "
                             "the baseline's skipped table")
            continue
        if name in skipped:
            improvements.append(f"{name}: builds now (was skipped: "
                                f"{skipped[name]})")
        for unit, info in units.items():
            if info.get("skipped"):
                if unit in designs:
                    regressions.append(f"{unit}: {info['skipped']}")
                continue
            if unit not in designs:
                notes.append(f"{unit}: not in the baseline")
                continue
            old = designs[unit]
            new = new_lines.get(unit, {})
            for key, line in sorted(old.items()):
                was = parse_line(line)
                now = parse_line(new[key]) if key in new else {}
                for action, o in sorted(was.items()):
                    if not scope(unit, action, key):
                        continue
                    if isinstance(o, dict):
                        c = now.get(action, {"pass": 0, "fail": 0,
                                             "refuse": 0})
                        if not isinstance(c, dict):
                            continue
                        if c["pass"] < o["pass"]:
                            regressions.append(
                                f"{unit} {key} {action}: {c['pass']} of the "
                                f"family pass, {o['pass']} did")
                        elif (c["fail"] > o["fail"] and
                              c["pass"] + c["refuse"] < o["pass"] +
                              o["refuse"]):
                            regressions.append(
                                f"{unit} {key} {action}: {c['fail']} of the "
                                f"family fail, {o['fail']} did")
                        elif c != o:
                            improvements.append(
                                f"{unit} {key} {action}: {o} -> {c}")
                        continue
                    if key not in new:
                        notes.append(f"{unit} {key}: gone")
                        break
                    n = now.get(action)
                    if n is None:
                        if o == "pass":
                            regressions.append(
                                f"{unit} {key} {action}: passed, now not "
                                "tried (the entity no longer offers it)")
                        continue
                    if RANK[n] < RANK[o]:
                        regressions.append(f"{unit} {key} {action}: {o} -> "
                                           f"{n}")
                    elif RANK[n] > RANK[o]:
                        improvements.append(f"{unit} {key} {action}: {o} -> "
                                            f"{n}")
            for key in sorted(set(new) - set(old)):
                notes.append(f"{unit} {key}: new ({new[key]})")
    # Findings.
    known = baseline.get("known", [])
    findings = [r for r in results if r["outcome"] == "finding"]
    unmatched = []
    hit = set()
    for r in findings:
        fam = r.get("family") or r["entity"]
        m = next((i for i, k in enumerate(known)
                  if k["unit"] == r["unit"] and k["action"] == r["action"]
                  and k["entity"] in (r["entity"], fam)
                  and k["class"] == r["class"]), None)
        if m is None:
            unmatched.append(r)
        else:
            hit.add(m)
    if not same_kernel:
        # Another kernel series answers differently: only what is never an
        # answer is judged.
        regressions = []
        unmatched = [r for r in unmatched if r["class"] in ("crash", "hang")]
        notes.insert(0, f"the baseline is OpenCASCADE {baseline.get('occ')}"
                     f"'s, this is {occ}: the ratchet is not judged, only "
                     "crashes and hangs")
    else:
        for i, k in enumerate(known):
            if i not in hit and k["unit"] in {
                    u for m in metas.values() for u in m.get("units", {})} \
                    and scope(k["unit"], k["action"], k["entity"]):
                regressions.append(
                    f"known {k['unit']} {k['entity']} {k['action']} "
                    f"({k['class']}) no longer reproduces: remove it from "
                    f"the baseline's known table, its repro "
                    f"({k.get('repro')}) and its entry ({k.get('bug')})")
    for r in unmatched:
        regressions.append(
            f"finding {r['class']}: {r['unit']} {r['entity']} "
            f"{r['action']}: {r['message'][:200]}")
    return regressions, improvements, notes, findings


# --- the baseline file ------------------------------------------------------
def dump_baseline(b):
    """Sorted, one line per entity, so a diff names the entity that moved."""
    out = ["{"]
    out.append(f' "occ": {json.dumps(b["occ"])},')
    out.append(f' "fuzz_generator": {json.dumps(b["fuzz_generator"])},')
    out.append(' "designs": {')
    units = sorted(b["designs"])
    for i, unit in enumerate(units):
        out.append(f'  {json.dumps(unit)}: {{')
        ents = sorted(b["designs"][unit].items())
        for j, (k, v) in enumerate(ents):
            out.append(f'   {json.dumps(k)}: {json.dumps(v)}'
                       + ("," if j < len(ents) - 1 else ""))
        out.append("  }" + ("," if i < len(units) - 1 else ""))
    out.append(" },")
    for key in ("unreferenceable", "skipped", "slow"):
        out.append(f' "{key}": {{')
        items = sorted(b[key].items())
        for j, (k, v) in enumerate(items):
            out.append(f'  {json.dumps(k)}: {json.dumps(v, sort_keys=True)}'
                       + ("," if j < len(items) - 1 else ""))
        out.append(" },")
    out.append(' "known": [')
    known = sorted(b["known"], key=lambda k: (k["unit"], k["entity"],
                                              k["action"]))
    for j, k in enumerate(known):
        out.append("  " + json.dumps(k, sort_keys=True)
                   + ("," if j < len(known) - 1 else ""))
    out.append(" ]")
    out.append("}")
    return "\n".join(out) + "\n"


def load_baseline(path=BASELINE):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return {"occ": None, "fuzz_generator": None, "designs": {},
                "unreferenceable": {}, "skipped": {}, "slow": {}, "known": []}


def bless(out, corpus, occ):
    """The baseline from a whole run in `out` (or several shards' in one
    directory). Refuses a partial one and findings `known` does not list."""
    metas, results = read_run(out)
    want = {r["design"] for r in corpus["designs"]}
    missing = sorted(want - set(metas))
    if missing:
        print(f"bless: {len(missing)} design(s) have no result in {out} "
              f"(a partial run, or a shard missing): {missing[:8]}"
              f"{' ...' if len(missing) > 8 else ''}")
        return 1
    run_meta = os.path.join(out, "run.json")
    for path in [run_meta] + glob.glob(os.path.join(out, "run-*.json")):
        if os.path.exists(path):
            with open(path) as f:
                rm = json.load(f)
            if rm.get("every", 1) != 1 or rm.get("actions"):
                print("bless: refused: that run was sampled or limited to "
                      "some actions; bless a whole one")
                return 1
    old = load_baseline()
    regressions, improvements, notes, findings = judge(
        dict(old, occ=occ_series(occ), designs={}), metas, results,
        lambda *a: True, occ)
    known = old.get("known", [])
    unknown = [r for r in findings if not any(
        k["unit"] == r["unit"] and k["action"] == r["action"] and
        k["entity"] in (r["entity"], r.get("family") or r["entity"]) and
        k["class"] == r["class"] for k in known)]
    if unknown:
        print(f"bless: refused: {len(unknown)} finding(s) the known table "
              "does not list. Each wants a docs/known-bugs.md entry, a "
              "reproduction in tests/action_matrix/open/ and a `known` row "
              "(unit, entity, action, class, bug, repro) -- or a fix:")
        for r in unknown[:40]:
            print(f"  {r['class']}: {r['unit']} {r['entity']} {r['action']}: "
                  f"{r['message'][:160]}")
        return 1
    designs = tokens(results)
    unref, skipped = {}, {}
    for name, meta in metas.items():
        if meta.get("skipped"):
            skipped[name] = meta["skipped"]
            continue
        for unit, info in meta["units"].items():
            if info.get("skipped"):
                skipped[unit] = info["skipped"]
                continue
            designs.setdefault(unit, {})
            u = {"faces": info.get("unnamed_faces", 0),
                 "edges": info.get("unnamed_edges", 0)}
            if info.get("capped_bodies"):
                u["capped"] = info["capped_bodies"]
            if any(u.values()):
                unref[unit] = u
    slow = {}
    for r in results:
        if r.get("seconds", 0) > SLOW_S:
            row = slow.setdefault(r["unit"], {})
            key = f"{r.get('family') or r['entity']} {r['action']}"
            secs = 0.0 if r["class"] == "hang" else round(r["seconds"], 1)
            row[key] = max(row.get(key, 0.0), secs)
    base = {"occ": occ_series(occ), "fuzz_generator": fd.GENERATOR_VERSION,
            "designs": designs, "unreferenceable": unref, "skipped": skipped,
            "slow": slow, "known": known}
    with open(BASELINE, "w") as f:
        f.write(dump_baseline(base))
    n = {o: sum(1 for r in results if r["outcome"] == o) for o in OUTCOMES}
    print(f"bless: wrote {os.path.relpath(BASELINE, ROOT)}: {len(designs)} "
          f"units, {n['pass']} pass, {n['refuse']} refuse, {n['fail']} fail, "
          f"{n['finding']} known finding(s); {len(skipped)} skipped")
    return 0


# --- the report ---------------------------------------------------------
def report(out, metas, results, verdict, seconds):
    regressions, improvements, notes, findings = verdict
    by_action = {}
    by_design = {}
    gists = {}
    for r in results:
        by_action.setdefault(r["action"], {o: 0 for o in OUTCOMES})[
            r["outcome"]] += 1
        design = r["unit"].split("#")[0]
        by_design.setdefault(design, {o: 0 for o in OUTCOMES})[
            r["outcome"]] += 1
        if r["outcome"] != "pass":
            g = fd.gist(r["outcome"] + (f"/{r['class']}" if r["class"] else
                                        ""), r["message"])
            gists.setdefault(g, []).append(r)
    lines = [f"# Action matrix: {len(results)} actions on {len(metas)} "
             f"designs", "", f"{seconds:.0f} s wall.", ""]
    total = {o: sum(1 for r in results if r["outcome"] == o) for o in OUTCOMES}
    lines += ["| " + " | ".join(OUTCOMES) + " |",
              "|" + "---|" * len(OUTCOMES),
              "| " + " | ".join(str(total[o]) for o in OUTCOMES) + " |", ""]
    for title, verdict_list in (("Regressions", regressions),
                                ("Improvements", improvements),
                                ("Notes", notes)):
        if verdict_list:
            lines += [f"## {title}", ""] + [f"- {x}" for x in
                                            verdict_list[:300]] + [""]
    lines += ["## By action", "", "| action | " + " | ".join(OUTCOMES) +
              " | pass rate |", "|---|" + "---|" * (len(OUTCOMES) + 1)]
    for a in sorted(by_action):
        c = by_action[a]
        n = sum(c.values())
        lines.append(f"| {a} | " + " | ".join(str(c[o]) for o in OUTCOMES)
                     + f" | {100 * c['pass'] / n:.0f} % |")
    lines += ["", "## By design", "", "| design | " + " | ".join(OUTCOMES) +
              " |", "|---|" + "---|" * len(OUTCOMES)]
    for d in sorted(by_design):
        c = by_design[d]
        lines.append(f"| {d} | " + " | ".join(str(c[o]) for o in OUTCOMES)
                     + " |")
    skipped = {n: m["skipped"] for n, m in metas.items() if m.get("skipped")}
    if skipped:
        lines += ["", "## Skipped", ""] + [f"- {n}: {s}" for n, s in
                                           sorted(skipped.items())]
    lines += ["", "## Messages by gist", "", "| count | gist | e.g. |",
              "|---|---|---|"]
    for g, rs in sorted(gists.items(), key=lambda kv: -len(kv[1])):
        r = rs[0]
        lines.append(f"| {len(rs)} | {g.replace('|', '/')} | "
                     f"{r['unit']} {r['entity']} {r['action']} |")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(text)
    return text


# --- run ------------------------------------------------------------------
def default_binary():
    return os.path.join(ROOT, "build", "lobole")


def default_jobs():
    try:
        out = subprocess.run([os.path.join(HERE, "build-jobs"), "1200"],
                             capture_output=True, text=True,
                             check=True).stdout.strip()
        return max(1, int(out))
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 1


def select_designs(corpus, args):
    rows = corpus["designs"]
    if args.sample or args.determinism:
        rows = sample_designs(rows)
    if args.shard:
        rows = [r for r in rows if any(in_shard(u, args.shard)
                                       for u in units_of(r))]
    if args.design:
        rows = [r for r in rows if fnmatch.fnmatch(r["design"], args.design)
                or args.design in r["design"]]
    return rows


def ping_occ(binary):
    inst = Instance(binary)
    try:
        return inst.call("ping").get("occ", "")
    finally:
        inst.stop()


def cmd_run(args):
    try:
        corpus = load_corpus()
    except CorpusError as e:
        print(f"action_matrix: {e}")
        return 1
    if args.bless and (args.sample or args.design or args.actions or
                       args.shard or args.determinism):
        print("action_matrix: --bless takes a whole run: no --sample, "
              "--design, --actions or --determinism, and a --shard only "
              "through `bless --from DIR` with every shard's output")
        return 2
    if args.determinism:
        return cmd_determinism(args, corpus)
    rows = select_designs(corpus, args)
    every = SAMPLE_EVERY if args.sample else 1
    out = args.out or os.path.join(ROOT, "scratch", "action_matrix",
                                   time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    tag = f"@{args.shard.replace('/', 'of')}" if args.shard else ""
    for r in rows:
        stale = os.path.join(out, r["design"].replace("/", "__") + tag +
                             ".jsonl")
        if os.path.exists(stale):
            os.remove(stale)
    started = time.time()
    occ = ping_occ(args.binary)
    jobs = max(1, min(args.jobs or default_jobs(), len(rows)))
    # Longest first, round-robin, so one worker does not get every big one.
    weight = {"generator": 3, "golden": 1, "reference": 1, "fuzz": 1}
    ordered = sorted(rows, key=lambda r: -weight[r["kind"]])
    procs = []
    for j in range(jobs):
        mine = [r["design"] for r in ordered[j::jobs]]
        if not mine:
            continue
        cmd = [sys.executable, os.path.abspath(__file__), "_worker",
               "--binary", args.binary, "--out", out, "--every", str(every),
               "--designs", "\n".join(mine)]
        if args.actions:
            cmd += ["--actions", args.actions]
        if args.shard:
            cmd += ["--shard", args.shard]
        procs.append(subprocess.Popen(cmd))
    print(f"action_matrix: {len(rows)} designs on {len(procs)} workers "
          f"(pids {' '.join(str(p.pid) for p in procs)}) into {out}",
          flush=True)
    for p in procs:
        p.wait()  # by pid
    shard = args.shard.replace("/", "of") if args.shard else ""
    with open(os.path.join(out, f"run-{shard}.json" if shard else
                           "run.json"), "w") as f:
        json.dump({"every": every, "actions": args.actions, "occ": occ,
                   "designs": [r["design"] for r in rows],
                   "seconds": round(time.time() - started, 1)}, f)
    metas, results = read_run(out)
    metas = {k: v for k, v in metas.items() if k in {r["design"]
                                                     for r in rows}}
    for m in metas.values():
        m["units"] = {u: i for u, i in m["units"].items()
                      if in_shard(u, args.shard)}
    results = [r for r in results if r["unit"].split("#")[0] in metas
               and in_shard(r["unit"], args.shard)
               and action_in_shard(r["unit"], r.get("family") or r["entity"],
                                   args.shard)]
    lost = sorted({r["design"] for r in rows} - set(metas))
    actions = set(args.actions.split(",")) if args.actions else None
    baseline = load_baseline()
    slow = slow_set(baseline) if every != 1 else set()

    def scope(unit, action, key):
        return in_scope(unit, action, key, every, actions, slow) and \
            action_in_shard(unit, key, args.shard)

    verdict = judge(baseline, metas, results, scope, occ)
    if lost:
        verdict[0].append(f"no result for {lost}: a worker died")
    report(out, metas, results, verdict, time.time() - started)
    if args.profile:
        profile(out, metas, results, time.time() - started)
    n = {o: sum(1 for r in results if r["outcome"] == o) for o in OUTCOMES}
    print(f"action_matrix: {len(results)} actions on {len(metas)} designs in "
          f"{time.time() - started:.1f} s: {n['pass']} pass, {n['refuse']} "
          f"refuse, {n['fail']} fail, {n['finding']} finding(s); report "
          f"{os.path.join(out, 'report.md')}")
    regressions, improvements, notes, findings = verdict
    for title, items in (("improvements (bless to keep)", improvements),
                         ("notes", notes)):
        if items:
            print(f"  {len(items)} {title}:")
            for x in items[:20]:
                print(f"    {x}")
    if args.bless:
        if lost:
            print("action_matrix: not blessed: a worker died")
            return 1
        if regressions:
            print(f"action_matrix: blessing over {len(regressions)} "
                  "regression(s) against the old baseline:")
            for x in regressions[:100]:
                print(f"  - {x}")
        return bless(out, corpus, occ)
    if regressions:
        print(f"action_matrix: FAILED, {len(regressions)} regression(s):")
        for x in regressions[:100]:
            print(f"  - {x}")
        return 1
    return 0


def cmd_bless(args):
    try:
        corpus = load_corpus()
    except CorpusError as e:
        print(f"action_matrix: {e}")
        return 1
    occ = ""
    for path in glob.glob(os.path.join(args.from_dir, "run*.json")):
        with open(path) as f:
            occ = occ or json.load(f).get("occ", "")
    if not occ:
        print(f"bless: {args.from_dir} holds no run*.json: not a run")
        return 1
    return bless(args.from_dir, corpus, occ)


def profile(out, metas, results, seconds):
    """Where a run's time went (`run --profile`): profile.md and
    profile.json beside the report -- per action, per RPC, per design's
    build, and the slowest single actions -- so the next slow class is
    found from a measurement."""
    by_action = {}
    for r in results:
        a = by_action.setdefault(r["action"], {"n": 0, "s": 0.0, "max": 0.0})
        a["n"] += 1
        a["s"] += r.get("seconds", 0.0)
        a["max"] = max(a["max"], r.get("seconds", 0.0))
    rpc = {}
    prepare_s = {}
    design_s = {}
    for d, m in metas.items():
        for method, (n, s_) in (m.get("rpc") or {}).items():
            x = rpc.setdefault(method, [0, 0.0])
            x[0] += n
            x[1] += s_
        if "prepare_s" in m:
            prepare_s[d] = prepare_s.get(d, 0.0) + m["prepare_s"]
        design_s[d] = design_s.get(d, 0.0) + m.get("seconds", 0.0)
    slowest = sorted(results, key=lambda r: -r.get("seconds", 0.0))[:30]
    data = {"wall_s": round(seconds, 1),
            "actions_s": round(sum(a["s"] for a in by_action.values()), 1),
            "prepare_s": round(sum(prepare_s.values()), 1),
            "by_action": by_action, "rpc": rpc, "prepare": prepare_s,
            "designs": design_s,
            "slowest": [{k: r.get(k) for k in ("unit", "entity", "action",
                                                "outcome", "seconds")}
                        for r in slowest]}
    with open(os.path.join(out, "profile.json"), "w") as f:
        json.dump(data, f, indent=1)
    lines = [f"# Action matrix profile", "",
             f"{seconds:.0f} s wall; the actions {data['actions_s']:.0f} s "
             f"and the designs' builds {data['prepare_s']:.0f} s, summed "
             f"over workers.", "", "## By action", "",
             "| action | n | total s | mean ms | max s |", "|---|---|---|---|---|"]
    for a, v in sorted(by_action.items(), key=lambda kv: -kv[1]["s"]):
        lines.append(f"| {a} | {v['n']} | {v['s']:.1f} | "
                     f"{1000 * v['s'] / max(v['n'], 1):.0f} | {v['max']:.1f} |")
    lines += ["", "## By RPC", "", "| method | calls | total s | mean ms |",
              "|---|---|---|---|"]
    for mth, (n, s_) in sorted(rpc.items(), key=lambda kv: -kv[1][1]):
        lines.append(f"| {mth} | {n} | {s_:.1f} | {1000 * s_ / max(n, 1):.1f} |")
    lines += ["", "## Building the designs (slowest 20)", "",
              "| design | s |", "|---|---|"]
    for d, s_ in sorted(prepare_s.items(), key=lambda kv: -kv[1])[:20]:
        lines.append(f"| {d} | {s_:.1f} |")
    lines += ["", "## Slowest actions", "",
              "| unit | entity | action | outcome | s |", "|---|---|---|---|---|"]
    for r in slowest:
        lines.append(f"| {r['unit']} | {r['entity'][:60]} | {r['action']} | "
                     f"{r['outcome']} | {r.get('seconds', 0):.1f} |")
    with open(os.path.join(out, "profile.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"action_matrix: profile {os.path.join(out, 'profile.md')}")


# --- determinism ----------------------------------------------------------
def cmd_determinism(args, corpus):
    """The sample's designs at one action in eight, each design built in two
    fresh instances, and every action run twice in each (add, undo, add):
    the same build must give the same names, outcomes and results. The second
    instance takes the actions in the reverse order, so an action whose answer
    hangs on what ran on the part before it (a kernel call writing onto the
    cached body it was handed) answers differently there."""
    rows = select_designs(corpus, args)
    out = args.out or tempfile.mkdtemp(prefix="action_matrix_det_")
    started = time.time()
    problems = []
    n_actions = 0
    for row in rows:
        runs = []
        for attempt in range(2):
            w = Worker(args.binary, out, DETERMINISM_EVERY, None, fresh=True)
            try:
                inst = w.instance()
                try:
                    parts = prepare(inst, row, w.work)
                except PrepareError as e:
                    runs.append({"skipped": str(e)})
                    continue
                got = {}
                for k, part in enumerate(parts):
                    unit = unit_name(row, part)
                    if k:
                        prepare(inst, row, w.work)
                    if part is not None:
                        inst.call("set_active_part", part=part)
                    els = inst.request("list_elements",
                                       {"max_faces": 1000000,
                                        "walls": True}, timeout=300)
                    ctx = Context(inst)
                    plan, _ = plan_actions(
                        els, ctx.state,
                        grow=lambda refs: inst.request(
                            "grow_faces", {"faces": refs}))
                    names = sorted({a["entity"] for a in plan})
                    seen = {}
                    for act in (plan if attempt == 0 else plan[::-1]):
                        key = act["family"] or act["entity"]
                        if not in_scope(unit, act["action"], key,
                                        DETERMINISM_EVERY, None, w.slow):
                            continue
                        twice = []
                        for _ in range(2):
                            res, restore = run_action(w.inst, act, ctx)
                            twice.append((res["outcome"], res["class"],
                                          res["digest"]))
                            if restore == "restart":
                                w.restart()
                            if restore:
                                w.reenter(row, part, ctx)
                        n_actions += 1
                        if not (twice[0][:2] == twice[1][:2] and
                                same_digest(twice[0][2], twice[1][2])):
                            problems.append(
                                f"{unit} {act['entity']} {act['action']}: "
                                f"run twice in one instance, {twice[0][:2]} "
                                f"then {twice[1][:2]}")
                        seen.setdefault((key, act["action"]), []).append(
                            twice[0])
                    got[unit] = (names, seen)
                runs.append(got)
            except (ConnectionError, OSError) as e:
                runs.append({"skipped": f"instance died: {e}"})
            finally:
                w.close()
        a, b = runs
        if "skipped" in a or "skipped" in b:
            if a.get("skipped") != b.get("skipped"):
                problems.append(f"{row['design']}: {a.get('skipped')} vs "
                                f"{b.get('skipped')}")
            continue
        for unit in a:
            na, sa = a[unit]
            nb, sb = b.get(unit, ([], {}))
            if na != nb:
                problems.append(
                    f"{unit}: the entity names differ between two fresh "
                    f"instances: only in the first {sorted(set(na) - set(nb))[:5]}"
                    f", only in the second {sorted(set(nb) - set(na))[:5]}")
            for key in sorted(set(sa) | set(sb)):
                xa = sorted(sa.get(key, []), key=lambda t: (t[0], t[1]))
                xb = sorted(sb.get(key, []), key=lambda t: (t[0], t[1]))
                if [t[:2] for t in xa] != [t[:2] for t in xb] or not all(
                        same_digest(p[2], q[2]) for p, q in zip(xa, xb)):
                    problems.append(
                        f"{unit} {key[0]} {key[1]}: two fresh instances "
                        f"answered {[t[:2] for t in xa]} and "
                        f"{[t[:2] for t in xb]}")
    secs = time.time() - started
    if problems:
        print(f"action_matrix determinism: FAILED ({n_actions} actions on "
              f"{len(rows)} designs, {secs:.0f} s):")
        for p in problems[:60]:
            print(f"  - {p}")
        return 1
    print(f"action_matrix determinism: {n_actions} actions on {len(rows)} "
          f"designs agree across two fresh instances, in either order, and "
          f"when run twice "
          f"({secs:.0f} s)")
    return 0


# --- one action again ------------------------------------------------------
def cmd_replay(args):
    """A reproduction (tests/action_matrix/open/*.json, or a run's row):
    the design built in a fresh instance, the one action sent and judged."""
    with open(args.case) as f:
        case = json.load(f)
    corpus = load_corpus()
    rows = {r["design"]: r for r in corpus["designs"]}
    design, _, part = case["unit"].partition("#")
    row = rows[design]
    inst = Instance(args.binary, timeout=args.timeout)
    work = tempfile.mkdtemp(prefix="action_matrix_replay_")
    try:
        prepare(inst, row, work)
        if part:
            inst.call("set_active_part", part=part)
        ctx = Context(inst)
        act = {"entity": case["entity"], "family": case.get("family"),
               "action": case["action"], "calls": case["call"],
               "expect": case.get("expect") or EXPECT[case["action"]],
               "check": case.get("check")}
        t = time.time()
        result, restore = run_action(inst, act, ctx)
        print(json.dumps({"unit": case["unit"], "entity": case["entity"],
                          "action": case["action"], **result,
                          "seconds": round(time.time() - t, 3)}, indent=1))
        if restore == "restart":
            print(inst.log_tail(150))
        want = case.get("expect_outcome")
        if want:
            return 0 if (result["outcome"], result["class"]) == tuple(
                want) else 1
        return 0
    finally:
        inst.stop()
        shutil.rmtree(work, ignore_errors=True)


EXPECT = {"edge.fillet": "any", "edge.chamfer": "any", "edge.frame": "same",
          "face.sketch": "same", "face.extrude_add": "add",
          "face.extrude_cut": "cut", "face.hole": "cut", "face.shell": "cut",
          "face.draft": "any", "face.frame": "same", "curved.thread": "same",
          "curved.frame": "same", "body.pattern_linear": "any",
          "body.pattern_circular": "any", "body.mirror": "any",
          "body.split": "same", "face.delete": "any",
          "face.delete_feature": "any", "face.offset_out": "add",
          "face.offset_in": "cut", "face.move": "same", "face.replace": "any"}


# --- the self-test --------------------------------------------------------
class Fake:
    """An answer source for the classifier: `script` maps a method to a
    function (params, state) -> answer, raising to refuse."""

    def __init__(self, script, bodies):
        self.script = script
        self.state = {"bodies": [dict(b) for b in bodies],
                      "features": [{"id": 1, "type": "Box"}],
                      "revision": {"version": 1}, "regen": {"ok": True}}
        self.calls = []

    def request(self, method, params=None):
        self.calls.append(method)
        if method in self.script:
            return self.script[method](params or {}, self.state)
        if method == "get_state":
            return json.loads(json.dumps(self.state))
        if method == "check_geometry":
            return {"valid": True, "bodies": []}
        if method == "mass_properties":
            return {"bodies": [{"bbox_min": b["bbox_min"],
                                "bbox_max": b["bbox_max"]}
                               for b in self.state["bodies"]]}
        if method == "undo":
            self.state["features"] = self.state["features"][:1]
            self.state["bodies"] = [dict(BOX)]
            self.state["revision"]["version"] += 1
            return self.undo_answer()
        raise AssertionError(f"the fake has no {method}")

    def undo_answer(self):
        """What the app's undo answers: the bodies' digest, the feature
        count and the revision."""
        return {"digest": {"bodies": [dict(b) for b in self.state["bodies"]]},
                "feature_count": len(self.state["features"]),
                "revision": dict(self.state["revision"])}


BOX = {"created_by": 1, "name": "Box", "volume_mm3": 1000.0,
       "bbox_min": [0.0, 0.0, 0.0], "bbox_max": [10.0, 10.0, 10.0]}


def self_test():
    from lobole.errors import Refused
    problems = []
    fillet = {"entity": "edge:1/e1", "family": None, "action": "edge.fillet",
              "expect": "any", "calls": [{"method": "add_feature",
                                          "params": {"feature": {}}}]}

    def accepted(failed=(), volume=999.0):
        def answer(params, state):
            state["features"].append({"id": 2, "type": "Fillet"})
            state["bodies"] = [dict(BOX, volume_mm3=volume)]
            state["revision"]["version"] += 1
            return {"regen": {"ok": not failed, "failed": list(failed),
                              "warnings": []}}
        return answer

    def expect(name, script, want, cls=None, undo=None):
        fake = Fake(script, [BOX])
        if undo:
            fake.script["undo"] = undo
        ctx = Context(fake)
        got, _ = run_action(fake, fillet, ctx)
        if got["outcome"] != want or (cls and got["class"] != cls):
            problems.append(f"{name}: judged {got['outcome']}/{got['class']} "
                            f"({got['message'][:80]}), want {want}/{cls}")

    expect("a pass", {"add_feature": accepted()}, "pass")

    def refuse_and_move(params, state):
        state["revision"]["version"] += 1
        raise Refused("fillet radius must be smaller than the edge it "
                      "rounds", {"code": "invalid_argument"})
    expect("a refusal that changed the revision", {
        "add_feature": refuse_and_move}, "fail", "refusal_changed")

    def refuse(params, state):
        raise Refused("fillet radius must be smaller than the edge it "
                      "rounds", {"code": "invalid_argument"})
    expect("a refusal", {"add_feature": refuse}, "refuse")
    expect("a broken feature", {"add_feature": accepted(
        [{"name": "F", "error": "the fillet failed on 1 contour(s) "
          "(radius too large for the geometry?)"}])}, "fail", "broken")
    expect("a caught OSD_SIGSEGV", {"add_feature": accepted(
        [{"name": "F", "error": "the fillet failed: OSD_SIGSEGV Access "
          "violation"}])}, "finding", "signal")
    expect("a message that states nothing", {"add_feature": accepted(
        [{"name": "F", "error": "failed:"}])}, "finding", "message")

    def bad_undo(params, state):
        state["features"] = state["features"][:1]
        state["bodies"] = [dict(BOX, volume_mm3=990.0)]
        return {}
    expect("an undo that did not restore", {"add_feature": accepted()},
           "finding", "undo", undo=bad_undo)

    def bad_undo_digest(params, state):
        state["features"] = state["features"][:1]
        state["bodies"] = [dict(BOX, volume_mm3=990.0)]
        return {"digest": {"bodies": [dict(b) for b in state["bodies"]]},
                "feature_count": 1, "revision": dict(state["revision"])}
    expect("an undo whose own digest says it did not restore",
           {"add_feature": accepted()}, "finding", "undo",
           undo=bad_undo_digest)

    def cut_adds(params, state):
        state["features"].append({"id": 2})
        state["bodies"] = [dict(BOX, volume_mm3=1010.0)]
        return {"regen": {"ok": True, "failed": [], "warnings": []}}
    fake = Fake({"add_feature": cut_adds}, [BOX])
    got, _ = run_action(fake, dict(fillet, expect="cut", action="face.hole"),
                        Context(fake))
    if (got["outcome"], got["class"]) != ("fail", "wrong_way"):
        problems.append(f"a hole that added: judged {got}")

    # The ratchet: two ordinal pieces that swapped names between runs pass
    # as a family; a pass that became a refusal does not.
    fam = "edge:5/f[*]"
    base = {"occ": "7.9", "designs": {"d": {
        fam: "edge.fillet*1 !edge.fillet*1",
        "edge:5/g": "edge.chamfer edge.fillet"}}, "skipped": {}, "known": []}
    metas = {"d": {"design": "d", "units": {"d": {}}, "skipped": None}}

    def row(entity, action, o, family=None):
        return {"unit": "d", "entity": entity, "family": family,
                "action": action, "outcome": o, "class": "", "message": ""}
    swapped = [row("edge:5/f;1", "edge.fillet", "fail", fam),
               row("edge:5/f;2", "edge.fillet", "pass", fam),
               row("edge:5/g", "edge.fillet", "pass"),
               row("edge:5/g", "edge.chamfer", "pass")]
    reg = judge(base, metas, swapped, lambda *a: True, "7.9.3")[0]
    if reg:
        problems.append(f"a swapped pair of ordinal pieces was judged a "
                        f"regression: {reg}")
    worse = swapped[:3] + [row("edge:5/g", "edge.chamfer", "refuse")]
    reg = judge(base, metas, worse, lambda *a: True, "7.9.3")[0]
    if len(reg) != 1 or "pass -> refuse" not in reg[0]:
        problems.append(f"a pass that became a refusal: {reg}")
    lost = [row("edge:5/f;1", "edge.fillet", "fail", fam),
            row("edge:5/f;2", "edge.fillet", "fail", fam)] + swapped[2:]
    reg = judge(base, metas, lost, lambda *a: True, "7.9.3")[0]
    if len(reg) != 1 or "family pass" not in reg[0]:
        problems.append(f"a family that lost its pass: {reg}")
    finding = dict(row("edge:5/g", "edge.fillet", "finding"), cls="signal")
    finding["class"] = "signal"
    reg = judge(base, metas, swapped[:2] + [finding, swapped[3]],
                lambda *a: True, "7.9.3")[0]
    if not any(r.startswith("finding signal") for r in reg):
        problems.append(f"an unknown finding was not a failure: {reg}")
    # A line round trip.
    toks = tokens(swapped)
    if parse_line(toks["d"][fam]) != {"edge.fillet": {"pass": 1, "fail": 1,
                                                      "refuse": 0}}:
        problems.append(f"family line {toks['d'][fam]!r} does not parse back")
    if problems:
        print("action_matrix self-test: FAILED")
        for p in problems:
            print("  - " + p)
        return 1
    print("action_matrix self-test: a pass, a refusal, a refusal that moved "
          "the revision, a broken feature, a caught OSD_SIGSEGV, a silent "
          "message, a hole that added and an undo that did not undo were each "
          "judged right; swapped ordinal pieces are no regression, a lost "
          "pass and an unknown finding are")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--binary", default=default_binary())
    r.add_argument("--sample", action="store_true")
    r.add_argument("--shard", default="")
    r.add_argument("--design", default="")
    r.add_argument("--actions", default="")
    r.add_argument("--jobs", type=int, default=0)
    r.add_argument("--out", default="")
    r.add_argument("--determinism", action="store_true")
    r.add_argument("--bless", action="store_true")
    r.add_argument("--profile", action="store_true")
    b = sub.add_parser("bless")
    b.add_argument("--from", dest="from_dir", required=True)
    sub.add_parser("self-test")
    rp_ = sub.add_parser("replay")
    rp_.add_argument("case", help="a JSON reproduction, or one JSONL row")
    rp_.add_argument("--binary", default=default_binary())
    rp_.add_argument("--timeout", type=float, default=ACTION_TIMEOUT_S)
    w = sub.add_parser("_worker")
    w.add_argument("--binary", required=True)
    w.add_argument("--out", required=True)
    w.add_argument("--every", type=int, default=1)
    w.add_argument("--designs", required=True)
    w.add_argument("--actions", default="")
    w.add_argument("--shard", default="")
    args = ap.parse_args()
    if hasattr(args, "binary"):
        args.binary = os.path.abspath(args.binary)
    if args.cmd == "run":
        if args.actions:
            bad = sorted(set(args.actions.split(",")) - set(ACTIONS))
            if bad:
                ap.error(f"unknown action(s) {bad}; accepted: "
                         f"{', '.join(ACTIONS)}")
        if args.shard and not re.fullmatch(r"\d+/\d+", args.shard):
            ap.error("--shard is K/N")
        return cmd_run(args)
    if args.cmd == "bless":
        return cmd_bless(args)
    if args.cmd == "self-test":
        return self_test()
    if args.cmd == "replay":
        return cmd_replay(args)
    return worker_main(args)


if __name__ == "__main__":
    sys.exit(main())
