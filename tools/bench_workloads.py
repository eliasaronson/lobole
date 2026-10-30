# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The performance harness's workloads and fixtures (tools/bench.py).

A WORKLOAD is a row: its instance kind, its fixture, an untimed setup, a
timed body, the reset between repeats, its repeats, and the assertions that
prove it measured what its name says. The body times its own calls
(`ctx.timed`), so a check inside it costs nothing; `ctx.note` keeps what the
app reported (regen counts, log phases) beside the times, and a phase the app
stopped reporting is kept as missing, never as zero.

A FIXTURE is a project a generator builds, once, into
~/.cache/lobole/bench/fixtures/<name>-<hash>/: the hash is over the
generator's own files, the generators it imports, generators/_lib.py and its
recipe, so editing any of them builds it again. Most recipes are the
generator's own test.json (generators/README.md#the-big-generators-as-tests);
the chassis has none and is declared here.

docs/performance/README.md#the-workloads says why each one is here.
"""
import dataclasses
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from typing import Callable, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "generators")
sys.path.insert(0, HERE)
import generator_tests  # noqa: E402


# --------------------------------------------------------------- fixtures --

def _imports(path):
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return set()
    return set(re.findall(r"^\s*(?:from|import)\s+([A-Za-z_]\w*)", text,
                          re.M))


def _module_files():
    """{module name: generators file} for every .py under generators/."""
    found = {}
    for dirpath, _, files in os.walk(GEN):
        for f in files:
            if f.endswith(".py"):
                found.setdefault(f[:-3], os.path.join(dirpath, f))
    return found


@dataclasses.dataclass
class Fixture:
    name: str
    project: str          # the saved project, relative to the output dir
    decl: dict            # test.json-shaped: steps, markers, instance, ...
    set: str = "default"  # default | big
    probe: Optional[Callable] = None  # (client, project) -> dict, at build

    def files(self):
        """The files the hash covers, relative to generators/: the
        generator's own (its directory, or its one file), every generators
        module they import, transitively, the scripts its steps name, and
        _lib.py."""
        mods = _module_files()
        root = os.path.join(GEN, self.name)
        todo = []
        if os.path.isdir(root):
            for dirpath, _, fs in os.walk(root):
                todo += [os.path.join(dirpath, f) for f in fs
                         if not f.endswith((".pyc",))]
        for step in self.decl["steps"]:
            for a in step:
                if a.startswith("{generators}/"):
                    todo.append(os.path.join(GEN, a[len("{generators}/"):]))
        todo.append(os.path.join(GEN, "_lib.py"))
        seen = set()
        while todo:
            p = os.path.normpath(todo.pop())
            if p in seen or not os.path.isfile(p):
                continue
            seen.add(p)
            if p.endswith(".py"):
                for m in _imports(p):
                    if m in mods:
                        todo.append(mods[m])
        return sorted(os.path.relpath(p, GEN) for p in seen)

    def hash(self):
        h = hashlib.sha256()
        h.update(json.dumps({"steps": self.decl["steps"],
                             "project": self.project},
                            sort_keys=True).encode())
        for rel in self.files():
            h.update(rel.encode() + b"\0")
            with open(os.path.join(GEN, rel), "rb") as f:
                h.update(f.read())
            h.update(b"\0")
        return h.hexdigest()[:12]


def fixture_command(fx, port, out, python):
    return generator_tests.command(fx.decl, port, out, python)


def _probe_cam(client, project):
    """The board job's move counts, per operation, as the fixture's build
    made them: cam/chess-board asserts the binary under test makes the
    same."""
    client.request("open_project", {"path": project}, timeout=600)
    return {"cam_moves": cam_moves(client, cam_toolpath_all(client))}


def _declared():
    tests = generator_tests.load()
    fx = {}

    def from_test(name, project, set_="default", probe=None):
        fx[name] = Fixture(name, project, tests[name], set_, probe)

    # --stage preview saves the project it checked as om615-unchecked.lob:
    # "unchecked" is the motion it did not run, not the build.
    from_test("om615", "om615/om615-unchecked.lob")
    from_test("engine", "engine/engine.lob")
    from_test("hook", "hook/hook.lob")
    from_test("hinge_box", "hinge_box/hinge_box.lob")
    from_test("chess", "chess/chess_only.lob", probe=_probe_cam)
    fx["chassis"] = Fixture("chassis", "chassis/chassis.lob", {
        "steps": [["{python}", "-u", "{generators}/chassis/chassis.py",
                   "--port", "{port}", "--out", "{out}/chassis",
                   "--save", "{out}/chassis/chassis.lob"]],
        "markers": ["project saved to"],
        "instance": "headless",
        "budget_s": 900,
    })
    from_test("clock", "clock/clock.lob", "big")
    from_test("planetary", "planetary/planetary.lob", "big")
    return fx


FIXTURES = _declared()


# ------------------------------------------------------------ the helpers --

def get_state(ctx):
    return ctx.call("get_state")


def regen_executed(ctx):
    r = get_state(ctx).get("regen", {})
    return r.get("executed"), r.get("cached")


SYNC_RE = re.compile(r"(Document|Job|Assembly) synced: .*? in (\d+(?:\.\d+)?)"
                     r" ms \[([^\]]*)\]")
PAIR_RE = re.compile(r"([a-z][a-z ]*?) (\d+(?:\.\d+)?)")


def sync_phases(text):
    """The phases of the last "... synced ... in N ms [a N, b N]" line in
    `text`, in ms, keyed "<kind> sync" and "<kind> sync: <phase>"; {} when
    there is none."""
    found = list(SYNC_RE.finditer(text))
    if not found:
        return {}
    m = found[-1]
    kind = m.group(1).lower()
    out = {f"{kind}_sync_ms": float(m.group(2))}
    for k, v in PAIR_RE.findall(m.group(3)):
        out[f"{kind}_sync_{k.strip().replace(' ', '_')}_ms"] = float(v)
    return out


def cam_toolpath_all(client):
    """cam_toolpath of every setup of the open job, {setup: result}."""
    setups = client.request("list_cam_setups", {}, timeout=600)
    out = {}
    for s in setups.get("setups", []):
        sid = s.get("id", s.get("name"))
        out[str(sid)] = client.request("cam_toolpath", {"setup": sid},
                                       timeout=1800)
    return out


def cam_moves(client, results):
    return {sid: [op.get("moves") for op in r.get("operations", [])]
            for sid, r in results.items()}


# -------------------------------------------------------------- workloads --

@dataclasses.dataclass
class Workload:
    name: str
    doc: str
    kind: str                       # headless | offscreen
    body: Callable
    setup: Callable = lambda ctx: None
    reset: Optional[Callable] = None
    finish: Optional[Callable] = None
    fixture: Optional[str] = None
    repeats: int = 5
    inputs: tuple = ()
    set: str = "default"
    launches: bool = False          # the body launches its own instance
    fresh_instance: bool = False    # one instance per repeat
    estimate_s: float = 60.0        # rough, for --dry-run before a history
    setup_s: float = 10.0


def _new_project(ctx):
    ctx.call("new_project")


# startup ------------------------------------------------------------------

def _startup(kind):
    def body(ctx):
        ping = ctx.launch_timed(kind)
        ctx.expect(ping.get("mode") == kind,
                   f"the instance launched is {ping.get('mode')}, not {kind}")
    return Workload(
        f"startup/{kind}", f"launch --{kind} to the first ping answered",
        kind, body, launches=True, estimate_s=10, setup_s=0)


# open -----------------------------------------------------------------------

# Rough seconds for a whole open/rebuild row (warm-up and five repeats), for
# --dry-run before this machine has a history of it. Rough on purpose: the
# history replaces them after the first run.
REBUILD_S = {"om615": 300, "engine": 90, "clock": 900, "planetary": 400}

def _open_baked(fx):
    def setup(ctx):
        ctx.working_copy()
        ctx.call("new_project")

    def body(ctx):
        r = ctx.timed("open", "open_project", path=ctx.state["copy"])
        rebuilt = r.get("rebuilt_parts")
        ctx.expect(rebuilt == 0,
                   f"a baked open rebuilt {rebuilt} part(s) "
                   f"({r.get('rebuild_reason')})")

    return Workload(
        f"open/baked/{fx}", f"open_project of {fx}, baked by this build",
        "headless", body, setup, reset=_new_project, fixture=fx,
        set=FIXTURES[fx].set, estimate_s=REBUILD_S.get(fx, 30) // 10 + 10,
        setup_s=10)


def _strip_geometry(src, dst):
    """A copy of the project at `src` with every part's saved geometry
    removed (`<part dir>/geometry/`, docs/versioning/README.md#baked-geometry).
    Returns the names of the parts that had one: an empty part bakes
    nothing, so it has nothing to rebuild."""
    shutil.copytree(src, dst, symlinks=True)
    index = os.path.join(dst, "project.json")
    if not os.path.isfile(index):
        # A one-part project is the part's own directory.
        geo = os.path.join(dst, "geometry")
        if os.path.isdir(geo):
            shutil.rmtree(geo)
            return [os.path.basename(src)]
        return []
    with open(index) as f:
        docs = json.load(f).get("documents", [])
    names = []
    for d in docs:
        geo = os.path.join(dst, d.get("dir", ""), "geometry")
        if d.get("type") == "part" and os.path.isdir(geo):
            shutil.rmtree(geo)
            names.append(d["name"])
    return sorted(names)


def _open_rebuild(fx):
    def setup(ctx):
        if "stripped" not in ctx.state:
            src = ctx.working_copy()
            dst = os.path.join(ctx.work, "rebuild-" + os.path.basename(src))
            ctx.state["removed"] = _strip_geometry(src, dst)
            ctx.state["stripped"] = dst
        ctx.expect(bool(ctx.state["removed"]),
                   "the project has no part with saved geometry to remove")
        ctx.call("new_project")

    def body(ctx):
        r = ctx.timed("open", "open_project", path=ctx.state["stripped"])
        # open_project answers a count, and the reason when they share one.
        rebuilt, want = r.get("rebuilt_parts"), len(ctx.state["removed"])
        ctx.note("rebuilt_parts", rebuilt)
        ctx.expect(rebuilt == want,
                   f"{rebuilt} part(s) rebuilt, not every one of the {want} "
                   f"whose geometry was removed ({ctx.state['removed']})")
        ctx.expect(r.get("rebuild_reason") == "no saved geometry",
                   f"the rebuild's reason is {r.get('rebuild_reason')!r}, "
                   "not \"no saved geometry\"")

    return Workload(
        f"open/rebuild/{fx}",
        f"open_project of {fx} with every part's geometry/ removed",
        "headless", body, setup, reset=_new_project, fixture=fx,
        set=FIXTURES[fx].set, estimate_s=REBUILD_S.get(fx, 30), setup_s=10)


# history ------------------------------------------------------------------

HEAD_PART = "cylinder head"


def _history_setup(ctx):
    """Open the head and evaluate it once, untimed. A baked open installs
    the tip's bodies and nothing else, so the first return to the opened
    state rebuilds from the start by design
    (docs/versioning/README.md#baked-geometry); what is measured is the
    cache a session has, not the load."""
    ctx.call("open_project", path=ctx.working_copy())
    ctx.call("set_active_part", part=HEAD_PART)
    ctx.state["edit"] = _head_edit(ctx)
    ctx.state["edits"] = 0
    ctx.call("set_rollback", index=0)
    ctx.call("set_rollback", index=-1)


def _head_edit(ctx):
    """The edit: the second Extrude's distance, 1 % longer, so everything
    after it executes and its undo has a whole chain to find in the cache.
    Not a variable edit: no feature of the generated head reads a variable
    (its two part parameters drive nothing), and an edit nothing reads
    executes nothing."""
    feats = get_state(ctx)["features"]
    extrudes = [f for f in feats if f.get("type") == "Extrude"
                and isinstance(f.get("distance"), (int, float))
                and f.get("end_mode", "blind") == "blind"]
    if len(extrudes) < 2:
        raise RuntimeError(f"{HEAD_PART} has no second blind Extrude to edit")
    f = dict(extrudes[1])
    spec = {k: v for k, v in f.items() if k not in (
        "error", "warning", "status", "index", "regen", "derived",
        "error_after_earlier_failure")}
    return {"id": f["id"], "spec": spec, "distance": f["distance"]}


def _history_body(ctx):
    def returned(what):
        ex, _ = regen_executed(ctx)
        ctx.note(f"{what}_executed", ex)
        ctx.expect(ex == 0, f"{what} executed {ex} feature(s), not 0: the "
                   "regen cache missed")

    ctx.timed("rollback_start", "set_rollback", index=0)
    ctx.timed("rollback_back", "set_rollback", index=-1)
    returned("rollback_back")
    # A distance no repeat has used, so the edit executes every time and
    # leaves the same amount behind it.
    e = ctx.state["edit"]
    ctx.state["edits"] += 1
    spec = dict(e["spec"], distance=round(
        e["distance"] * (1 + 0.01 * ctx.state["edits"]), 6))
    ctx.timed("edit", "modify_feature", feature_id=e["id"], feature=spec)
    ex, _ = regen_executed(ctx)
    ctx.note("edit_executed", ex)
    ctx.expect(ex is not None and ex > 0,
               f"the edit executed {ex} feature(s): it measured nothing")
    ctx.timed("undo", "undo")
    returned("undo")
    ctx.timed("redo", "redo")
    returned("redo")


# boolean --------------------------------------------------------------------

def _plate_n(ctx):
    return 20 if ctx.scale == "full" else 3


def _plate_calls(n, pitch=5.0, dia=2.0):
    """A plate with an n x n Hole pattern from one point sketch, and beside
    it a plate of n*n separate Cylinder cuts: one batch."""
    size = n * pitch
    plane = {"origin": [0, 0, 3], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}
    calls = [
        {"method": "add_feature", "params": {"feature": {
            "type": "Box", "name": "Patterned", "on": "Top", "width": size,
            "depth": size, "height": 3, "centered": False, "offset": [0, 0],
            "mode": "new_body"}}},
        {"method": "add_feature", "params": {"feature": {
            "type": "Sketch", "name": "Hole centres", "plane": plane,
            "entities": [{"type": "point",
                          "p": [pitch / 2 + i * pitch, pitch / 2 + j * pitch]}
                         for i in range(n) for j in range(n)]}}},
        {"method": "add_feature", "params": {"feature": {
            "type": "Hole", "name": "Holes", "sketch_id": {"$ref": 2},
            "diameter": dia, "through": True}}},
        {"method": "add_feature", "params": {"feature": {
            "type": "Box", "name": "Separate", "on": "Top", "width": size,
            "depth": size, "height": 3, "centered": False,
            "offset": [size + 10, 0], "mode": "new_body"}}},
    ]
    for i in range(n):
        for j in range(n):
            calls.append({"method": "add_feature", "params": {"feature": {
                "type": "Cylinder", "name": f"Cut {i},{j}", "on": "Top",
                "diameter": dia, "height": 10,
                "offset": [size + 10 + pitch / 2 + i * pitch,
                           pitch / 2 + j * pitch], "mode": "cut"}}})
    return calls


def _plate_body(ctx):
    n = _plate_n(ctx)
    ctx.timed("batch", "batch", calls=_plate_calls(n))
    ex, _ = regen_executed(ctx)
    ctx.note("executed", ex)
    bodies = ctx.call("list_elements", max_faces=100000)["bodies"]
    holes = [sum(1 for f in b["faces"] if f.get("kind") == "cylinder"
                 and abs((f.get("radius") or 0) - 1.0) < 1e-6)
             for b in bodies]
    ctx.expect(holes == [n * n, n * n],
               f"hole faces per plate {holes}, not [{n * n}, {n * n}]")


# step -------------------------------------------------------------------------

STEP_FILES = {
    "linkrods": "~/Documents/CAD/OCCT/data/step/linkrods.step",
    "usb3stacked": "~/Documents/CAD/kicad-source-mirror/demos/cm5_minima/"
                   "CM5IO.3dshapes/USB3stacked.STEP",
}


def _bodies(ctx):
    return len(get_state(ctx).get("bodies", []))


def _step_import(name, path):
    def body(ctx):
        ctx.timed("import", "import_step", path=os.path.expanduser(path))
        n = _bodies(ctx)
        ctx.note("bodies", n)
        ctx.expect(n > 0, "the import made no body")
        first = ctx.state.setdefault("bodies", n)
        ctx.expect(n == first, f"{n} bodies, where the first import made "
                   f"{first}")

    return Workload(
        f"step/import/{name}", f"import_step of {path}", "headless", body,
        _new_project, reset=_new_project, inputs=(path,), estimate_s=20,
        setup_s=2)


def _step_export(name, path):
    def setup(ctx):
        ctx.call("new_project")
        ctx.call("import_step", path=os.path.expanduser(path))
        ctx.state["bodies"] = _bodies(ctx)
        ctx.state["out"] = os.path.join(ctx.work, "export.step")

    def body(ctx):
        r = ctx.timed("export", "export_step", path=ctx.state["out"])
        ctx.expect(os.path.getsize(ctx.state["out"]) > 0,
                   "the export wrote nothing")
        ctx.note("bodies", len(r.get("bodies", [])) if isinstance(
            r.get("bodies"), list) else r.get("bodies"))

    def finish(ctx):
        ctx.call("new_project")
        ctx.call("import_step", path=ctx.state["out"])
        n = _bodies(ctx)
        ctx.expect(n == ctx.state["bodies"],
                   f"the export reads back as {n} bodies, not "
                   f"{ctx.state['bodies']}")

    return Workload(
        f"step/export/{name}", f"export_step of what {path} imported",
        "headless", body, setup, finish=finish, inputs=(path,),
        estimate_s=20, setup_s=10)


# render -----------------------------------------------------------------------

RENDER_FIXTURES = ("om615", "chassis")


def _first_capture(fx):
    def setup(ctx):
        if "copy" not in ctx.state:
            ctx.working_copy()
        ctx.call("new_project")

    def body(ctx):
        mark = ctx.log_mark()
        ctx.timed("open", "open_project", path=ctx.state["copy"])
        ctx.timed("capture", "screenshot",
                  path=os.path.join(ctx.work, "first.png"))
        ph = sync_phases(ctx.log_since(mark))
        ctx.expect(bool(ph), "no \"... synced\" line was logged: the "
                   "capture did not project the scene")
        if not ph:
            ctx.note("sync_ms", None)  # missing, not zero
        for k, v in ph.items():
            ctx.note(k, v)

    return Workload(
        f"render/first-capture/{fx}",
        f"open {fx} in a fresh --offscreen instance, then the first "
        "screenshot, which projects the scene", "offscreen", body, setup,
        fixture=fx, fresh_instance=True, set=FIXTURES[fx].set,
        estimate_s=60, setup_s=10)


def _passes(fx):
    def setup(ctx):
        path = ctx.working_copy()
        ctx.call("open_project", path=path)
        ctx.call("screenshot", path=os.path.join(ctx.work, "warm.png"))

    def body(ctx):
        n = 30 if ctx.scale == "full" else 3
        ctx.call("frame_stats", reset=True)
        r = ctx.timed("renders", "frame_stats", renders=n)
        gpu = r.get("gpu") or {}
        passes = {f"gpu_{k}": v for k, v in gpu.items()
                  if k.endswith("_ms") and isinstance(v, (int, float))}
        ctx.expect(gpu.get("supported") is True and bool(passes),
                   f"no GPU pass times in frame_stats: {gpu}")
        for k, v in passes.items():
            ctx.note(k, v)

    return Workload(
        f"render/passes/{fx}", f"frame_stats renders on {fx}: the GPU "
        "pass split", "offscreen", body, setup, fixture=fx,
        set=FIXTURES[fx].set, estimate_s=20, setup_s=10)


# assembly / motion / cam / fem ---------------------------------------------

def _open_copy(ctx):
    ctx.call("open_project", path=ctx.state["copy"])


def _assembly_setup(ctx):
    ctx.working_copy()
    _assembly_reset(ctx)


def _activate_assembly(ctx):
    """Make the project's (first) assembly the active tab."""
    asms = [p for p in ctx.call("list_parts")["parts"]
            if p.get("type") == "assembly"]
    if not ctx.expect(bool(asms), f"{ctx.fixture.name} has no assembly"):
        raise RuntimeError(f"{ctx.fixture.name} has no assembly")
    ctx.call("set_active_part", part=asms[0]["name"])


def _assembly_reset(ctx):
    _open_copy(ctx)
    _activate_assembly(ctx)


def _assembly_body(ctx):
    s = ctx.timed("solve", "solve_assembly")
    ctx.expect(not s.get("errored_mates"),
               f"{s.get('errored_mates')} mate(s) errored")
    # The sweep stops at a deadline (20 s by default) and says what it left
    # unchecked: a slower build would check fewer pairs in about the same
    # time and read as no change. So no deadline here, and a truncated
    # sweep fails the workload.
    r = ctx.timed("interference", "interference", deadline_s=3600.0)
    clashes = r.get("clashes", r.get("pairs", []))
    ctx.note("pairs_checked", r.get("pairs_checked"))
    ctx.expect(not r.get("truncated"),
               f"the sweep was truncated: {r.get('truncation')}")
    ctx.expect(not clashes, f"the sweep found {len(clashes)} clash(es): "
               + ", ".join(str(c.get("a", c)) + "/" + str(c.get("b", ""))
                           for c in clashes[:4]))


def _motion_setup(ctx):
    ctx.working_copy()
    _open_copy(ctx)
    _activate_assembly(ctx)
    studies = ctx.call("list_motion_studies").get("studies", [])
    ctx.expect(bool(studies), "the hinge box has no motion study")
    sid = studies[0].get("id")
    ctx.state["study"] = sid
    saved = ctx.call("run_motion_study", study=sid)
    ctx.state["saved_clean"] = saved.get("clean")
    ctx.expect(saved.get("reused") is True,
               "the saved study's result was not reused, so there is no "
               "saved verdict to hold the run to")


def _motion_body(ctx):
    r = ctx.timed("run", "run_motion_study", study=ctx.state["study"],
                  recompute=True)
    ctx.note("pairs_checked", r.get("pairs_checked"))
    ctx.note("app_seconds", r.get("seconds"))
    ctx.expect(r.get("clean") is True and ctx.state["saved_clean"] is True,
               f"the swing is not clean (run {r.get('clean')}, saved "
               f"{ctx.state['saved_clean']})")


def _cam_setup(ctx):
    ctx.working_copy()
    _open_copy(ctx)


def _cam_body(ctx):
    setups = ctx.call("list_cam_setups").get("setups", [])
    results = {}
    for s in setups:
        sid = s.get("id", s.get("name"))
        results[str(sid)] = ctx.timed(f"setup_{sid}", "cam_toolpath",
                                      setup=sid)
    got = cam_moves(ctx.client, results)
    want = ctx.fixture_meta.get("probes", {}).get("cam_moves")
    ctx.note("moves", sum(sum(m or 0 for m in ms) for ms in got.values()))
    ctx.expect(got == want, f"move counts {got}, the fixture made {want}")


def _fem_setup(ctx):
    ctx.working_copy()
    _open_copy(ctx)
    studies = ctx.call("list_studies").get("studies", [])
    ctx.expect(bool(studies), "the hook has no study")
    sid = studies[0].get("id")
    ctx.state["study"] = sid
    saved = ctx.call("run_study", id=sid)
    ctx.state["saved_fos"] = _fos(saved)
    ctx.expect(saved.get("reused") is True,
               "the saved result was not reused: no saved factor of safety "
               "to hold the run to")


def _fos(r):
    for k in ("min_factor_of_safety", "factor_of_safety", "min_fos", "fos"):
        if isinstance(r.get(k), (int, float)):
            return float(r[k])
    res = r.get("result") or r.get("results") or {}
    if isinstance(res, dict):
        return _fos(res) if res is not r else None
    return None


def _fem_body(ctx):
    r = ctx.timed("solve", "run_study", id=ctx.state["study"], recompute=True)
    fos, saved = _fos(r), ctx.state["saved_fos"]
    ctx.note("factor_of_safety", fos)
    ctx.expect(fos is not None and saved is not None
               and math.isclose(fos, saved, rel_tol=0.01),
               f"factor of safety {fos}, the saved study read {saved}")


# mesh -----------------------------------------------------------------------

# The organic fixtures (tests/mesh_organic.h) as STL, at the sizes the mesh
# budgets are set from (docs/performance/README.md#mesh-workloads): a sculpt
# download (10k, 300k) and a phone scan (2M). Written once by
# build/mesh_fixture_writer into ~/.cache/lobole/bench/meshes/, keyed by the generator's own source, with
# the fixture's known answers beside each as <file>.json.
MESH_SIZES = (10000, 300000, 2000000)
MESH_SRC = [os.path.join(ROOT, "tests", f) for f in
            ("mesh_organic.h", "mesh_organic.cpp", "mesh_fixture_writer.cpp")]
MESH_CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME")
                          or os.path.expanduser("~/.cache"),
                          "lobole", "bench", "meshes")


def mesh_fixture(name, triangles):
    """(path, answers) of the organic fixture `name` at about `triangles`,
    written on first use."""
    h = hashlib.sha256()
    for f in MESH_SRC:
        with open(f, "rb") as fh:
            h.update(fh.read())
    path = os.path.join(MESH_CACHE,
                        f"{name}-{triangles}-{h.hexdigest()[:12]}.stl")
    if not os.path.isfile(path + ".json"):
        os.makedirs(MESH_CACHE, exist_ok=True)
        writer = os.path.join(ROOT, "build", "mesh_fixture_writer")
        subprocess.run([os.path.join(ROOT, "tools", "nix-run"),
                        "ninja -C build mesh_fixture_writer"], cwd=ROOT,
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([writer, name, str(triangles), path], check=True)
    with open(path + ".json") as f:
        return path, json.load(f)


def _mesh_import(ctx, name, n):
    path, answers = mesh_fixture(name, n)
    ctx.state["answers"] = answers
    ctx.call("new_project")
    # keep_origin: the answers are in the file's coordinates. Past the facet
    # import's triangle limit it is let through: what it costs is the row.
    return ctx.call("import_step", path=path, keep_origin=True,
                    allow_large_mesh=True)


def _mesh_size_label(n):
    return f"{n // 1000}k" if n < 1000000 else f"{n // 1000000}M"


def _mesh_rows():
    rows = []
    for n in MESH_SIZES:
        label = _mesh_size_label(n)
        heavy = n >= 300000
        # The rows past 10k are mesh bodies (`auto`) and still the slow
        # ones: one repeat after the warm-up.
        reps = 1 if heavy else 3

        def import_body(ctx, n=n):
            path, answers = mesh_fixture("figurine", n)
            r = ctx.timed("import", "import_step", path=path,
                          keep_origin=True, allow_large_mesh=True)
            mesh = r.get("mesh") or {}
            ctx.note("triangles", mesh.get("triangles"))
            ctx.note("expected_s", mesh.get("expected_s"))
            # Recognised (the default preference) or sewn as facets, and why.
            ctx.note("fell_back", (r.get("recognition") or {}).get(
                "fell_back"))
            ctx.expect(_bodies(ctx) == 1,
                       f"the import made {_bodies(ctx)} bodies, not 1")

        rows.append(Workload(
            f"mesh/import/figurine-{label}",
            f"import_step of the {label}-triangle metaball figure (STL)",
            "headless", import_body, _new_project, reset=_new_project,
            repeats=reps, set="mesh", estimate_s=5 if not heavy else 3600,
            setup_s=10))

        def sync_setup(ctx, n=n):
            _mesh_import(ctx, "figurine", n)

        def sync_body(ctx):
            mark = ctx.log_mark()
            ctx.timed("capture", "screenshot",
                      path=os.path.join(ctx.work, "sync.png"))
            ph = sync_phases(ctx.log_since(mark))
            ctx.expect(bool(ph), "no \"... synced\" line was logged: the "
                       "capture did not project the figure")
            for k, v in ph.items():
                ctx.note(k, v)

        rows.append(Workload(
            f"mesh/render-sync/figurine-{label}",
            f"the first screenshot after importing the {label} figure, "
            "which projects it into the renderers (tessellation, outline "
            "adjacency, picker)", "offscreen", sync_body, sync_setup,
            reset=sync_setup, repeats=reps, set="mesh",
            estimate_s=10 if not heavy else 3600, setup_s=10))

        def mass_setup(ctx, n=n):
            _mesh_import(ctx, "sphere", n)

        def mass_body(ctx):
            r = ctx.timed("mass", "mass_properties")
            a = ctx.state["answers"]
            vol = sum(b.get("volume_mm3", 0.0) for b in r.get("bodies", []))
            ctx.note("volume_mm3", vol)
            ctx.expect(abs(vol - a["volume_mm3"]) <= a["volume_tolerance_mm3"],
                       f"volume {vol} mm3, the sphere's is "
                       f"{a['volume_mm3']} +- {a['volume_tolerance_mm3']}")

        rows.append(Workload(
            f"mesh/mass/sphere-{label}",
            f"mass_properties of the {label}-triangle noisy sphere, held to "
            "its analytic volume", "headless", mass_body, mass_setup,
            repeats=reps, set="mesh", estimate_s=5 if not heavy else 600,
            setup_s=10 if not heavy else 3600))

        def section_setup(ctx, n=n):
            _mesh_import(ctx, "figurine", n)
            ctx.call("screenshot", path=os.path.join(ctx.work, "warm.png"))

        def section_body(ctx):
            ctx.timed("set_section", "set_section", base_plane="xy",
                      offset=50.0)
            ctx.timed("capture", "screenshot",
                      path=os.path.join(ctx.work, "section.png"))
            r = ctx.timed("section", "section", base_plane="xy", offset=50.0)
            ctx.note("area_mm2", r.get("area_mm2"))
            ctx.expect((r.get("area_mm2") or 0) > 0,
                       "the cut through the figure's body has no area")

        def section_reset(ctx):
            ctx.call("set_section", enabled=False)

        rows.append(Workload(
            f"mesh/section/figurine-{label}",
            f"a section through the {label} figure's body: the clip and its "
            "cap drawn, and the section's area", "offscreen", section_body,
            section_setup, reset=section_reset, repeats=reps, set="mesh",
            estimate_s=10 if not heavy else 1800,
            setup_s=10 if not heavy else 3600))
    return rows


# the table -----------------------------------------------------------------

def _all():
    rows = [_startup("headless"), _startup("offscreen")]
    for fx in FIXTURES:
        rows.append(_open_baked(fx))
    for fx in FIXTURES:
        rows.append(_open_rebuild(fx))
    rows.append(Workload(
        "history/om615-head",
        "on the OM615 cylinder head: rollback to the start and back, an "
        "early Extrude's distance edited, undo, redo; every return must "
        "execute nothing",
        "headless", _history_body, _history_setup, fixture="om615",
        estimate_s=200, setup_s=40))
    rows.append(Workload(
        "boolean/plate-holes",
        "one batch: a plate with a 20 x 20 Hole pattern, and a plate of 400 "
        "separate Cylinder cuts (3 x 3 at --scale small)", "headless",
        _plate_body, _new_project, reset=_new_project, repeats=3,
        estimate_s=240, setup_s=0))
    for name, path in STEP_FILES.items():
        rows.append(_step_import(name, path))
        rows.append(_step_export(name, path))
    for fx in RENDER_FIXTURES:
        rows.append(_first_capture(fx))
    for fx in RENDER_FIXTURES:
        rows.append(_passes(fx))
    rows.append(Workload(
        "assembly/engine", "solve_assembly, then the no-pair interference "
        "sweep, on the engine", "headless", _assembly_body, _assembly_setup,
        reset=_assembly_reset, fixture="engine", estimate_s=20))
    rows.append(Workload(
        "motion/hinge-swing", "run_motion_study (recompute) of the hinge "
        "box's 0-180 degree swing", "headless", _motion_body, _motion_setup,
        fixture="hinge_box", estimate_s=10))
    rows.append(Workload(
        "cam/chess-board", "cam_toolpath of every setup of the chess "
        "board's job", "headless", _cam_body, _cam_setup, fixture="chess",
        estimate_s=40))
    rows.append(Workload(
        "fem/hook", "run_study (recompute) of the hook's study at its saved "
        "mesh", "headless", _fem_body, _fem_setup, fixture="hook",
        estimate_s=150))
    rows += _mesh_rows()
    return rows


_ROWS = _all()


def workloads(which="default"):
    """The rows of a set: default (fixtures of the default set and no
    fixture), big (adds the clock and planetary), mesh (the organic
    meshes, by size), all."""
    if which == "all":
        return list(_ROWS)
    if which == "big":
        return [w for w in _ROWS if w.set in ("default", "big")]
    if which == "mesh":
        return [w for w in _ROWS if w.set == "mesh"]
    return [w for w in _ROWS if w.set == "default"]
