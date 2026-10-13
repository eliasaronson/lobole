#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Hostile parameters: every method, every key, every value that is the right
type and still wrong (docs/scripting/rpc.md#the-hostile-sweep).

refused_calls_smoke sends the wrong TYPE; this sends `1e400` (parsed as
infinity), `1e308`, `-0.0`, `2^64`, `-1`, a 64 KB string, `[]` where a point
goes, nesting 64 deep -- the values that pass a type check and reach a
handler and the kernel. Each method of `describe` (sharded by a stable hash of
its name), each key of its row, one value at a time:

  - alone, `{key: value}`, and inside a VALID baseline where the method has
    one (the outside-the-document calls refused_calls_smoke holds, then one
    built from a typed row's `request_schema` with its ids taken from the
    fixture; a baseline counts only if it is accepted as written);
  - each fixture feature's fields through `modify_feature`, and sketch
    entities through `add_feature`;
  - the whole value as params, hostile `batch` calls and `$ref`s, and raw
    lines on a bare socket.

A failure is: the instance died (a crash: the case, the app's stack and its
crash report are printed) or did not answer in 30 s (a hang: killed by its
pid); an answer that is not one object with the request's id and exactly one
of result/error; `diagnostic.code: internal` (the handler fell over); a
`nonfinite` field (NaN or infinity sent as null); a refusal that changed the
world; a refusal with no measured condition. An accepted hostile call is not
a failure -- `1e308` is a legal depth -- and the world is put back.

Usage:
  hostile_params_smoke.py <binary> [--shard K/N] [--full] [--only m1,m2]
  hostile_params_smoke.py <binary> --self-test
--self-test drives `debug_fault` and `debug_crash` (LOBOLE_ALLOW_DEBUG_CRASH=1)
through the same judge and requires each planted failure to be reported as
its class: the sweep detects what it claims.
"""
import argparse
import json
import os
import shutil
import socket
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from smoke_app import launch_app  # noqa: E402
from smoke_world import (World, build_fixture, changed,  # noqa: E402
                         outside_baselines)

TIMEOUT_S = 30.0
# An answer may echo a value nested 10 000 deep, which json.loads walks
# recursively.
sys.setrecursionlimit(100000)

# Findings recorded in docs/known-bugs.md and not fixed yet: reported, not
# failed. Each entry names the doc that says why; delete it with the fix.
KNOWN = {}


class Raw:
    """JSON text sent as written: what Python's json cannot say (1e400) or
    should not (nesting deeper than its recursion limit)."""

    def __init__(self, text, label=None):
        self.text = text
        self.label = label or (text if len(text) < 40 else text[:20] + "...")

    def __repr__(self):
        return self.label


def enc(v):
    if isinstance(v, Raw):
        return v.text
    if isinstance(v, dict):
        return "{" + ",".join(json.dumps(k) + ":" + enc(x)
                              for k, x in v.items()) + "}"
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(enc(x) for x in v) + "]"
    return json.dumps(v)


def show(v, n=80):
    s = repr(v) if isinstance(v, Raw) else enc(v)
    return s if len(s) <= n else s[:n] + f"...({len(s)} chars)"


def nest(depth):
    return Raw("[" * depth + "]" * depth, f"nesting {depth} deep")


# The compact set runs on every build; --full adds the rest. A finding only
# --full reached adds its value here, so the sweep that runs everywhere pins
# it: 1e-308 (a deflection BRepMesh refused by throwing) and "1/0" (a
# variable prefix that hung extract_variables).
COMPACT = [Raw("1e400"), 1e308, 1e-308, -1, 0, -0.0, 2 ** 64, "", "1/0",
           "x" * 65536, [], {}, None, nest(64)]
FULL = [-1e308, 2 ** 53 + 1, -2 ** 63, 1.5, "nope", "(((1",
        "NaN", "inf", Raw('"\U0001F600 emoji"', "an emoji"),
        Raw('"\\ud800"', "a lone surrogate"), "\u0000",
        "y" * (1 << 20), [1e308, -1e308], [0, 0, 0], [[0, 0], [0, 0]], [{}],
        {"$ref": 1}, nest(10000), 999999]


def is_path_key(k):
    return k == "path" or k.endswith("_path") or k in ("dir", "directory",
                                                      "file", "folder_path")


def path_values(scratch):
    """Hostile paths, every one inside the scratch dir or relative to the
    app's cwd (which is the scratch dir): never $HOME, never a system
    path. "/" is the one absolute exception, and it is read-only."""
    return ["", ".", "/", scratch, os.path.join(scratch, "no", "such", "x"),
            os.path.join(scratch, "n" * 5000), "nul\u0000byte"]


def extremes(schema):
    """Values a typed key's own contract ACCEPTS, at its edges: they pass the
    validator, so they test the handler."""
    out = []
    for alt in schema.get("anyOf", [schema]):
        t = alt.get("type")
        if "enum" in alt:
            for lit in alt["enum"]:
                if isinstance(lit, str) and lit:
                    out.append(lit[:-1] + ("x" if lit[-1] != "x" else "y"))
        elif t == "integer":
            lo = alt.get("minimum", alt.get("exclusiveMinimum", -1) + 1)
            out += [int(lo), 2 ** 31, min(2 ** 63 - 1,
                                         int(alt.get("maximum", 2 ** 63 - 1)))]
        elif t == "number":
            if alt.get("exclusiveMinimum", None) == 0 or alt.get("minimum") == 0:
                out += [1e-300]
            out += [1e308]
    return out


def sample(schema):
    """A value the schema accepts (rpc_contracts_smoke's reading)."""
    if "anyOf" in schema:
        return sample(schema["anyOf"][0])
    if "enum" in schema:
        return schema["enum"][0]
    t = schema.get("type")
    if t == "integer":
        return int(schema.get("minimum",
                              schema.get("exclusiveMinimum", -1) + 1))
    if t == "number":
        return float(schema.get("minimum",
                                schema.get("exclusiveMinimum", 0) + 1))
    if t == "object":
        if "properties" in schema:
            return {k: sample(schema["properties"][k])
                    for k in schema.get("required", [])}
        return {}
    if t == "array":
        return [sample(schema.get("items", {}))] * schema.get("minItems", 0)
    return {"string": "x", "boolean": True, "null": None}.get(t, 0)


class Died(Exception):
    pass


class Hung(Exception):
    pass


class Conn:
    """A bare line client: the sweep reads the answer as sent, so it can
    judge its shape, and it sends text no client would."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port),
                                             timeout=TIMEOUT_S)
        self.buf = b""
        self.id = 0

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_line(self, data):
        try:
            self.sock.sendall(data + b"\n")
        except OSError as e:
            raise Died(f"send failed: {e}")

    def read_line(self, timeout=TIMEOUT_S):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(1 << 16)
            except socket.timeout:
                raise Hung(f"no answer in {timeout:g} s")
            except OSError as e:
                raise Died(f"recv failed: {e}")
            if not chunk:
                raise Died("the connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def request(self, method, params_text):
        """Send one request; return (raw answer dict, problems with its
        shape)."""
        self.id += 1
        rid = self.id
        self.send_line(('{"id":%d,"method":%s,"params":%s}' % (
            rid, json.dumps(method), params_text)).encode("utf-8",
                                                          "surrogatepass"))
        while True:
            line = self.read_line()
            try:
                msg = json.loads(line)
            except ValueError:
                return None, [f"an answer that is not JSON: {line[:200]!r}"]
            if isinstance(msg, dict) and msg.get("id") == rid:
                return msg, shape_problems(msg)
            # A line the server could not parse (1e400 overflows its
            # parser) is answered with a null id: it cannot know ours.
            if isinstance(msg, dict) and msg.get("id") is None and \
                    str(msg.get("error", "")).startswith("parse:"):
                return msg, shape_problems(msg)
            if isinstance(msg, dict) and "id" not in msg:
                continue  # a notification
            return msg, [f"an answer to another id: {str(msg)[:200]}"]

    def call(self, method, **params):
        msg, bad = self.request(method, enc(params))
        if bad or "error" in msg:
            raise RuntimeError(f"{method}: {bad or msg['error']}")
        return msg["result"]


def shape_problems(msg):
    out = []
    if not isinstance(msg, dict):
        return [f"an answer that is not an object: {str(msg)[:200]}"]
    if ("result" in msg) == ("error" in msg):
        out.append("an answer without exactly one of result/error: "
                   f"{sorted(msg)}")
    if "error" in msg and not isinstance(msg["error"], str):
        out.append(f"an error that is not a sentence: {msg['error']!r:.200}")
    return out


def internal(diag):
    """`internal` anywhere in a diagnostic: a batch carries its call's under
    failures[]. Iterative: a diagnostic can echo a value nested 10 000 deep."""
    stack = [diag]
    while stack:
        d = stack.pop()
        if isinstance(d, dict):
            if d.get("code") == "internal":
                return d
            stack.extend(d.values())
        elif isinstance(d, list):
            stack.extend(d)
    return None


class Sweep:
    def __init__(self, binary, work, env=None, extra_args=()):
        self.binary = binary
        self.work = work
        self.env = env
        self.extra_args = tuple(extra_args)
        self.out = os.path.join(work, "out")
        os.makedirs(self.out, exist_ok=True)
        os.makedirs(os.path.join(work, "fp"), exist_ok=True)
        self.failures = []
        self.counts = dict(calls=0, accepted=0, dispatcher=0, handler=0,
                           resets=0, relaunches=0)
        self.app = None
        self.conn = None
        self.d = None
        self.dead = []  # scratch dirs of instances that died: their reports
        self.keep_reports = True  # copy a crash report out of the scratch
        self.known = []  # findings docs/known-bugs.md already records
        self.launch()

    # --- the instance -----------------------------------------------------
    def launch(self, state_home=None):
        env = dict(os.environ if self.env is None else self.env)
        if state_home:
            env["XDG_STATE_HOME"] = state_home
        self.app = launch_app(self.binary, self.extra_args, env=env)
        self.state_home = state_home or os.path.join(self.app.tmp, "state")
        from designer_remote import Designer
        self.d = Designer.wait_for(self.app.port)
        self.d.strict = False  # the sweep judges `nonfinite` itself
        self.conn = Conn(self.app.port)
        self.cam = self.d.call("ping").get("features", {}).get("cam", True)
        self.build()

    def build(self):
        d = self.d
        self.part, self.occ, self.setup = build_fixture(d, self.cam, rich=True)
        self.tabs = [p["name"] for p in d.call("list_parts")["parts"]]
        self.fixture = os.path.join(self.work, "fixture.dsgn")
        d.call("save_project", path=self.fixture)
        self.features = d.call("get_state")["features"]
        self.world = World(d, self.app, self.work)
        self.tab = self.part["name"]
        self.quick_before = self.quick()

    def stop(self):
        for c in (self.conn, self.d):
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass
        if self.app is not None:
            self.app.terminate()
            try:
                self.app._proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                self.app.kill()
                self.app._proc.wait(timeout=10)

    def close(self):
        self.stop()
        if self.app is not None:
            shutil.rmtree(self.app._tmp, ignore_errors=True)
        for tmp in self.dead:
            shutil.rmtree(tmp, ignore_errors=True)

    def recover(self, kind, label):
        """The instance died or hung on `label`: record it with what the app
        said, kill it by its pid if it is still there, and start again on
        the dead one's state dir, so its crash marker becomes a crash
        report (docs/architecture.md#crashes-and-stalls)."""
        log = self.app.log()
        pid = self.app.pid
        if kind == "hang":
            self.app.kill()
        try:
            self.app._proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            self.app.kill()
        lines = log.splitlines()
        stall = [ln for ln in lines if "stalled" in ln or "resumed" in ln]
        tail = "\n      ".join((stall or lines)[-25:])
        state = self.state_home
        self.dead.append(self.app._tmp)
        self.app = None
        self.counts["relaunches"] += 1
        self.launch(state_home=state)
        report = ""
        try:
            reps = self.d.call("crash_reports")["reports"]
            if reps:
                report = os.path.join(reps[0]["path"], "report.txt")
            if reps and self.keep_reports:
                keep = os.path.join(
                    os.environ.get("XDG_STATE_HOME",
                                   os.path.expanduser("~/.local/state")),
                    "lobole", "smoke-failures",
                    time.strftime("%Y%m%d-%H%M%S") + f"-hostile-{pid}")
                shutil.copytree(reps[0]["path"], keep, dirs_exist_ok=True)
                report = os.path.join(keep, "report.txt")
        except Exception as e:  # noqa: BLE001
            report = f"(no crash report: {e})"
        self.fail(kind, label, f"pid {pid}; crash report: {report or 'none'}"
                  f"\n      {tail}")

    def fail(self, kind, label, said):
        self.failures.append(f"[{kind}] {label}: {said}")

    # --- the world --------------------------------------------------------
    def quick(self):
        """One round trip: what every call is checked against."""
        r = self.conn.call("batch", calls=[
            {"method": "ping"}, {"method": "validation_revision"},
            {"method": "list_parts"}, {"method": "background_jobs"}])
        ping, rev, parts, jobs = (c["result"] for c in r["results"])
        return {
            # Not each part's `version`: it moves on every regeneration,
            # and a rolled-back edit regenerates twice. `dirty` (against the
            # fixture, saved clean) is what says the document moved.
            "dirty": ping.get("dirty"), "project": ping.get("project"),
            "revision": [{k: v for k, v in r.items() if k != "version"}
                         for r in rev["revision"]],
            "missing": rev.get("missing_references"), "parts": parts,
            "jobs": sorted((j["kind"], j["id"]) for j in jobs["jobs"]
                           if j["kind"] != "cam"),
            # Not the autosave beside the fixture: it is written on a timer,
            # whatever the call in flight (docs/versioning/README.md).
            "cwd": sorted(n for n in os.listdir(self.work)
                          if not n.endswith(".autosave.d")),
            "out": sorted(os.listdir(self.out)),
        }

    def reset(self):
        """Put the fixture back after an accepted call."""
        self.counts["resets"] += 1
        d = self.d
        for j in d.call("background_jobs")["jobs"]:
            if j["kind"] != "cam":
                try:
                    d.call("cancel_background_job", kind=j["kind"])
                except Exception:  # noqa: BLE001
                    pass
        for name in os.listdir(self.work):
            if name not in ("fp", "out", "fixture.dsgn") and \
                    not name.endswith(".autosave.d"):
                p = os.path.join(self.work, name)
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) \
                    else os.unlink(p)
        shutil.rmtree(self.out, ignore_errors=True)
        os.makedirs(self.out)
        try:
            self.open_fixture()
            if [p["name"] for p in d.call("list_parts")["parts"]] != self.tabs:
                raise RuntimeError("tabs moved")
        except Exception:  # noqa: BLE001
            self.build()
        self.tab = None
        self.quick_before = None

    def open_fixture(self):
        """open_project the fixture and wait out the jobs the open itself
        starts. Every open warms the job tabs' boxes ("job-warm",
        docs/cam/gui.md#job-tab-frame-cost); one still running when the world
        is read and gone at the next read -- or the other way round, at the
        end of the shard, where two opens are compared -- read as a refusal
        or the shard having moved the world. On a loaded machine the warm
        outlived the read about once a suite run (`--self-test` reproduces it
        with --gui-test-job-latency-ms)."""
        self.d.call("open_project", path=self.fixture)
        self.wait_jobs()

    def wait_jobs(self, timeout=TIMEOUT_S):
        """Until no job but the CAM passes runs (they are the job's derived
        toolpaths, which the world does not read)."""
        deadline = time.time() + timeout
        while True:
            running = [j["kind"] for j in
                       self.d.call("background_jobs")["jobs"]
                       if j["kind"] != "cam"]
            if not running:
                return
            if time.time() > deadline:
                raise RuntimeError(f"background jobs {running} still running "
                                   f"{timeout:g} s after the fixture opened")
            time.sleep(0.05)

    def end_drift(self):
        """The whole world, once: against itself after the fixture is opened
        again. A refusal that moved what the cheap check does not read."""
        self.reset()
        self.use_tab(self.part["name"])
        self.world.settle()
        end = self.world.cheap()
        self.open_fixture()
        self.world.settle()
        return [x for x in changed(end, self.world.cheap())
                if not x.startswith(("app:", "cwd:"))]

    def use_tab(self, tab):
        if tab != self.tab:
            self.d.call("set_active_part", part=tab)
            self.tab = tab
            self.quick_before = None

    # --- one call ---------------------------------------------------------
    def probe(self, label, method, params, tab, outside=False):
        """Send `params` (a value, or Raw text) to `method` on `tab` and
        judge the answer. Returns "accepted", "refused" or "died"."""
        self.use_tab(tab)
        if self.quick_before is None:
            self.quick_before = self.quick()
        before = self.quick_before
        full_before = self.world.cheap() if outside else None
        self.counts["calls"] += 1
        text = enc(params)
        try:
            msg, bad = self.conn.request(method, text)
        except Died as e:
            self.recover("crash", f"{label}: {method} {show(params, 300)} "
                         f"({e})")
            return "died"
        except Hung as e:
            self.recover("hang", f"{label}: {method} {show(params, 300)} "
                         f"({e})")
            return "died"
        for b in bad:
            self.fail("answer", f"{label}: {method} {show(params)}", b)
        if msg is None or not isinstance(msg, dict):
            return "refused"
        if "nonfinite" in msg:
            self.fail("nonfinite", f"{label}: {method} {show(params)}",
                      f"non-finite numbers sent as null at "
                      f"{msg['nonfinite'][:5]}")
        hit = internal(msg.get("diagnostic"))
        if hit:
            said = f"{hit.get('observed')}: {str(msg.get('error'))[:300]}"
            where = f"{label}: {method} {show(params)}"
            keys = set(params) if isinstance(params, dict) else set()
            if keys & KNOWN.get(method, {}).get("keys", set()):
                self.known.append(f"[internal] {where}: {said} -- "
                                  f"{KNOWN[method]['doc']}")
            else:
                self.fail("internal", where, said)
        if "error" not in msg:
            self.counts["accepted"] += 1
            after = self.quick()
            if {k: v for k, v in after.items() if k != "jobs"} != \
                    {k: v for k, v in before.items() if k != "jobs"} or \
                    set(after["jobs"]) - set(before["jobs"]):
                self.reset()
            else:
                self.quick_before = after
            return "accepted"
        said = msg["error"]
        diag = msg.get("diagnostic") or {}
        if diag.get("code") in ("unknown_argument", "invalid_argument",
                                "missing_argument", "stale_revision"):
            self.counts["dispatcher"] += 1
        else:
            self.counts["handler"] += 1
        if not said.strip() or "(see log)" in said:
            self.fail("message", f"{label}: {method} {show(params)}",
                      f"a refusal without its measured condition: {said!r}")
        after = self.quick()
        diff = changed(before, after)
        # A job that ENDED is not the call's doing (a job tab's warm-up
        # finishing); one that appeared is.
        diff = [x for x in diff if not x.startswith("jobs:")]
        if set(after["jobs"]) - set(before["jobs"]):
            diff.append(f"jobs started: "
                        f"{sorted(set(after['jobs']) - set(before['jobs']))}")
        if outside and not diff:
            diff = changed(full_before, self.world.cheap())
        if diff:
            self.fail("changed", f"{label}: {method} {show(params)}",
                      f"REFUSED ({said[:160]}) but changed: "
                      f"{'; '.join(diff)}")
            self.reset()
        else:
            self.quick_before = after
        return "refused"


# --- which tab a method belongs on ---------------------------------------
def tab_for(name, sweep):
    n = name
    if n.startswith("cam_") or "_cam_" in n or n.startswith(("add_cam",
                                                             "set_job",
                                                             "add_job",
                                                             "remove_job",
                                                             "list_job")) \
            or n in ("export_setup_sheet", "add_keep_out", "remove_keep_out",
                     "add_keep_face", "remove_keep_face", "move_cam_op",
                     "list_cam_setups", "set_cam_op", "set_cam_setup",
                     "remove_cam_op", "remove_cam_setup"):
        return "Job" if sweep.cam else sweep.part["name"]
    if any(w in n for w in ("occurrence", "mate", "relation", "explode",
                            "motion", "bom", "solve_assembly",
                            "interference", "contact", "fastener",
                            "bearing", "retaining_ring", "layout_balloons",
                            "edit_in_context", "check_seat", "stack_up")):
        return "Asm"
    if "drawing" in n or n in ("layout_balloons", "set_drawing_sheet"):
        return "Dwg"
    return sweep.part["name"]


def shard_of(name, n):
    return zlib.crc32(name.encode()) % n


def ids_for(sweep):
    """A valid value for each id-shaped key, from the fixture."""
    feats = {f["name"]: f["id"] for f in sweep.features}
    return {
        "feature_id": feats["Plate"], "sketch_id": feats["Profile"],
        "body_name": "Plate", "body_id": feats["Plate"],
        "part": sweep.part["name"], "setup": sweep.setup,
        "occurrence": sweep.occ["id"], "source": sweep.part["id"],
    }


def baselines_for(sweep, rows):
    """{method: (tab, params)} for every method with a valid baseline: the
    outside-the-document calls first, then one per typed row built from
    its request_schema. Each is run once, and kept only if accepted."""
    out = {}
    for tab, method, base in outside_baselines(sweep.part, sweep.setup,
                                               sweep.out, sweep.cam):
        if method in ("delete_part", "set_active_part"):
            continue  # they rearrange what every later call relies on
        out[method] = (tab, base)
    ids = ids_for(sweep)
    for name, row in rows.items():
        if name in out or name in SKIP_BASELINE:
            continue
        schema = row.get("request_schema")
        if not schema or not schema.get("properties"):
            continue
        props = schema["properties"]
        base = {}
        for k in schema.get("required", []):
            base[k] = ids[k] if k in ids else sample(props[k])
        for k in props:
            if k in ids and k not in base:
                base[k] = ids[k]
        if "async" in props:
            base["async"] = True
        out[name] = (tab_for(name, sweep), base)
    return out


# Methods whose baseline would take the fixture apart for every later call,
# or that run long enough to dominate a shard when accepted.
SKIP_BASELINE = {"new_project", "open_project", "clone_project",
                 "history_restore", "checkpoint_restore", "branch_switch",
                 "debug_crash", "debug_fault", "batch", "delete_part"}


def sweep_method(sw, row, values, baseline, scratch):
    name = row["name"]
    keys = row.get("params") or []
    tab = baseline[0] if baseline else tab_for(name, sw)
    lead = {"async": True} if "async" in keys else {}
    schema = (row.get("request_schema") or {}).get("properties", {})
    reached = False
    for k in keys:
        vals = list(values)
        if is_path_key(k):
            vals = path_values(scratch)
        vals += extremes(schema.get(k, {}))
        for v in vals:
            if sw.probe("alone", name, {**lead, k: v}, tab) == "died":
                pass
            if baseline:
                sw.probe("baseline", name, {**baseline[1], k: v},
                         baseline[0], outside=bool(row.get("_outside")))
                reached = True
    # The whole value as params: refused as "params must be an object".
    for v in values[:6]:
        sw.probe("params", name, v, tab)
    return reached


def sweep_features(sw, values, types):
    """Each fixture feature of `types` with its fields set to each value
    through modify_feature (the world is put back after an accepted one)."""
    covered = set()
    tab = sw.part["name"]
    for f in list(sw.features):
        if f["type"] not in types:
            continue
        covered.add(f["type"])
        spec = {k: v for k, v in f.items() if k not in ("index", "warning",
                                                        "error")}
        for field in spec:
            if field in ("id", "type"):
                continue
            for v in values:
                sw.probe(f"field {f['type']}.{field}", "modify_feature",
                         {"feature_id": f["id"], "feature": {**spec,
                                                             field: v}},
                         tab)
    return covered


def sweep_sketches(sw, values):
    """The sketch entities add_feature takes, degenerate, and hostile
    sketch planes."""
    tab = sw.part["name"]
    plane = {"origin": [0, 0, 30], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}
    entities = [
        [{"type": "line", "a": [0, 0], "b": [0, 0]}],
        [{"type": "circle", "center": [0, 0], "radius": 0}],
        [{"type": "point", "p": [1, 1]}, {"type": "point", "p": [1, 1]}],
        [{"type": "line", "a": [1e308, 0], "b": [-1e308, 1e308]}],
        [{"type": "circle", "center": [1e308, 1e308], "radius": 1e308}],
        [{"type": "arc", "center": [0, 0], "radius": 5, "start_angle": 0,
          "end_angle": 0}],
        [{"type": "rect", "c1": [0, 0], "c2": [0, 0]}],
        [{"type": "spline", "poles": [[0, 0], [0, 0], [0, 0]]}],
        [{"type": "ellipse", "center": [0, 0], "r_major": 0, "r_minor": 0,
          "rotation": 0}],
        [{"type": "text", "text": "", "height": 0, "anchor": [0, 0]}],
    ]
    for ents in entities:
        sk = {"type": "Sketch", "name": "Hostile", "plane": plane,
              "entities": ents}
        sw.probe("sketch", "add_feature", {"feature": sk}, tab)
        for k in ("plane",):
            for v in values:
                sw.probe("sketch", "add_feature",
                         {"feature": {**sk, k: v}}, tab)
    for p in ({"origin": [0, 0, 0], "normal": [0, 0, 0], "uAxis": [1, 0, 0]},
              {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [0, 0, 1]},
              {"origin": [1e308, 0, 0], "normal": [0, 0, 1],
               "uAxis": [1, 0, 0]}):
        sw.probe("sketch plane", "add_feature", {"feature": {
            "type": "Sketch", "name": "Hostile", "plane": p,
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [5, 5]}]}},
            tab)


def sweep_batch(sw, values):
    tab = sw.part["name"]
    for v in values:
        sw.probe("batch calls", "batch", {"calls": v}, tab)
    many = Raw("[" + ",".join(['{"method":"ping"}'] * 10000) + "]",
               "10000 pings")
    sw.probe("batch calls", "batch", {"calls": many}, tab)
    sw.probe("batch calls", "batch", {"calls": [{"params": {}}]}, tab)
    box = {"method": "add_feature", "params": {"feature": {
        "type": "Box", "name": "B", "on": "Top", "width": 2, "depth": 2,
        "height": 2}}}
    for ref in (0, -1, 99999, "x", 2, None, 1.5, [1]):
        sw.probe("batch $ref", "batch", {"calls": [
            box, {"method": "delete_feature",
                  "params": {"feature_id": {"$ref": ref}}}]}, tab)
    sw.probe("batch $ref", "batch", {"calls": [
        box, {"method": "delete_feature",
              "params": {"feature_id": {"$ref": 1, "key": "nothing"}}}]}, tab)
    sw.probe("batch $ref", "batch", {"calls": [
        {"method": "delete_feature", "params": {"feature_id": {"$ref": 2}}},
        box]}, tab)


def sweep_raw(sw):
    """Lines no client would send, on a bare socket of their own. The answer
    must be a `parse:` error or a normal answer, and the instance stays up."""
    lines = [b"NaN", b"Infinity", b"\xff\xfe{}", b'{"method":"\xff"}',
             b'"\xc3"', b"x" * (8 << 20), b'{"method":"ping","params":{}}',
             b'{"id":1,"method":"ping"}{"id":2,"method":"ping"}',
             b'{"id":3,"method":"ping"} {"id":4,"method":"ping"}',
             b"[]", b"5", b"null", b'{"id":{"a":1},"method":"ping"}',
             b'{"id":5,"method":5}', b'{"id":6,"method":"ping","params":5}',
             b'{"id":7}', b'{"id":8,"method":""}']
    for data in lines:
        sw.counts["calls"] += 1
        label = f"raw line {data[:60]!r}" + (
            f"...({len(data)} bytes)" if len(data) > 60 else "")
        c = None
        try:
            c = Conn(sw.app.port)
            c.send_line(data)
            # Not every line earns an answer: one with no id gets one with
            # a null id, one we cannot tell. A ping after it must be
            # answered, on this connection, and the instance must be alive.
            c.send_line(b'{"id":424242,"method":"ping"}')
            while True:
                line = c.read_line()
                try:
                    msg = json.loads(line)
                except ValueError:
                    sw.fail("answer", label, f"not JSON: {line[:200]!r}")
                    continue
                if isinstance(msg, dict) and msg.get("id") == 424242:
                    break
                if isinstance(msg, dict) and "error" in msg and not (
                        str(msg["error"]).startswith("parse:") or
                        msg.get("id") is not None):
                    pass
                for b in shape_problems(msg):
                    sw.fail("answer", label, b)
        except (Died, Hung) as e:
            sw.recover("crash" if isinstance(e, Died) else "hang",
                       f"{label} ({e})")
        finally:
            if c:
                c.close()


def self_test(binary, work):
    """Plant each failure through debug_fault/debug_crash and require the
    judge to report it as its class (and to report nothing for the
    deliberate refusals)."""
    env = dict(os.environ, LOBOLE_ALLOW_DEBUG_CRASH="1")
    sw = Sweep(binary, work, env=env)
    sw.keep_reports = False  # read in place: a planted crash is no finding
    problems = []
    try:
        tab = sw.part["name"]
        want = {"out_of_range": "internal", "json_out_of_range": "internal",
                "bad_optional_access": "internal",
                "standard_failure": "internal", "refusal": None,
                "type_error": None, "edit_then_out_of_range": "internal",
                "nonfinite": "nonfinite", "write_then_refuse": "changed",
                # a fault inside an OCC scope is that call's failure, not a
                # crash, on the handler's thread and on a worker
                "occ_fault": None}
        for kind, cls in want.items():
            n = len(sw.failures)
            sw.probe("self-test", "debug_fault", {"kind": kind}, tab)
            got = [f for f in sw.failures[n:]]
            classes = {f[1:f.index("]")] for f in got}
            if cls is None and got:
                problems.append(f"{kind}: a deliberate refusal was reported: "
                                f"{got}")
            elif cls is not None and cls not in classes:
                problems.append(f"{kind}: expected [{cls}], got {got}")
            if kind == "edit_then_out_of_range" and "changed" in classes:
                problems.append(f"{kind}: the edit was not rolled back: {got}")
        # A kernel throw inside a batch rolls the whole batch back.
        n = len(sw.failures)
        sw.probe("self-test", "batch", {"calls": [
            {"method": "add_feature", "params": {"feature": {
                "type": "Box", "name": "B", "on": "Top", "width": 2,
                "depth": 2, "height": 2}}},
            {"method": "debug_fault", "params": {"kind": "standard_failure"}}
        ]}, tab)
        classes = {f[1:f.index("]")] for f in sw.failures[n:]}
        if classes != {"internal"}:
            problems.append(f"batch + standard_failure: expected [internal] "
                            f"alone (rolled back), got {sw.failures[n:]}")
        # A crash is reported, with the crash report it left.
        n = len(sw.failures)
        sw.probe("self-test", "debug_crash", {"kind": "segv"}, tab)
        got = sw.failures[n:]
        if not got or not got[0].startswith("[crash]") or \
                "report.txt" not in got[0] or "SIGSEGV" not in open(
                    got[0].split("crash report: ")[1].split("\n")[0]).read():
            problems.append(f"debug_crash segv: expected [crash] with a "
                            f"report naming SIGSEGV, got {got}")
        else:
            print("self-test: the crash left " +
                  got[0].split("crash report: ")[1].split("\n")[0])
        # and the sweep goes on, on a fresh instance with the fixture.
        n = len(sw.failures)
        sw.probe("self-test", "debug_fault", {"kind": "refusal"}, tab)
        if sw.failures[n:]:
            problems.append(f"after the crash: {sw.failures[n:]}")
    finally:
        sw.close()
    calls = sw.counts["calls"]
    # Jobs as slow as a loaded machine makes them: the fixture's job-tab warm
    # outlives every read, and the end-of-shard comparison of two opens must
    # still see a world that did not move.
    slow = os.path.join(work, "slow-jobs")
    os.makedirs(slow)
    sw = Sweep(binary, slow, extra_args=("--gui-test-job-latency-ms", "3000"))
    try:
        drift = sw.end_drift()
        if drift:
            problems.append(f"slow jobs: two opens of the fixture differ: "
                            f"{'; '.join(drift)}")
    finally:
        sw.close()
    if problems:
        print("hostile_params_smoke --self-test: FAILED")
        for p in problems:
            print("  - " + p)
        return 1
    print(f"hostile_params_smoke --self-test: every planted failure was "
          f"reported as its class ({calls} calls)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--shard", default="0/1")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    binary = os.path.abspath(args.binary)
    work = tempfile.mkdtemp(prefix="designer_hostile_")
    os.chdir(work)  # the app's cwd: a relative path lands where we look
    try:
        if args.self_test:
            return self_test(binary, work)
        return run(binary, work, args)
    finally:
        os.chdir(HERE)
        shutil.rmtree(work, ignore_errors=True)


def run(binary, work, args):
    k, n = (int(x) for x in args.shard.split("/"))
    values = COMPACT + (FULL if args.full else [])
    t0 = time.time()
    sw = Sweep(binary, work)
    try:
        described = sw.d.call("describe")
        rows = {m["name"]: m for m in described["methods"]}
        feature_types = [t["type"] for t in described["feature_types"]]
        fixture_types = {f["type"] for f in sw.features}
        only = set(filter(None, args.only.split(",")))
        outside = {b[1] for b in outside_baselines(sw.part, sw.setup,
                                                   sw.out, sw.cam)}
        for name in outside:
            if name in rows:
                rows[name]["_outside"] = True
        mine = sorted(m for m in rows if shard_of(m, n) == k
                      and (not only or m in only))
        # The pseudo-methods: each its own unit of the shard hash.
        # Each fixture feature type's fields is a unit of its own, so the
        # resets they cost spread over the shards.
        units = sorted({f"fields:{f['type']}" for f in sw.features}) + [
            "sketch:entities", "batch:hostile", "raw:lines"]
        extra = [u for u in units
                 if shard_of(u, n) == k and (not only or u in only)]
        bases = baselines_for(sw, {m: rows[m] for m in mine})
        valid = {}
        for m, (tab, base) in bases.items():
            sw.use_tab(tab)
            try:
                sw.d.request(m, base, timeout=TIMEOUT_S)
                valid[m] = (tab, base)
            except Exception:  # noqa: BLE001 -- not a valid baseline
                pass
            sw.reset()
        reached = 0
        swept = 0
        for m in mine:
            row = rows[m]
            if row.get("params_free_form"):
                continue
            swept += 1
            if sweep_method(sw, row, values, valid.get(m), work):
                reached += 1
        covered = sweep_features(sw, values, {
            u.split(":", 1)[1] for u in extra if u.startswith("fields:")})
        if "sketch:entities" in extra:
            sweep_sketches(sw, values)
        if "batch:hostile" in extra:
            sweep_batch(sw, values)
        if "raw:lines" in extra:
            sweep_raw(sw)
        diff = sw.end_drift()
        if diff:
            sw.fail("changed", "end of shard",
                    f"the world drifted: {'; '.join(diff)}")
    finally:
        sw.close()
    dt = time.time() - t0
    c = sw.counts
    print(f"hostile_params_smoke {k}/{n}{' --full' if args.full else ''}: "
          f"{swept} methods, {c['calls']} calls in {dt:.1f} s: "
          f"{c['accepted']} accepted, {c['dispatcher']} refused by the "
          f"dispatcher, {c['handler']} by the handler; {c['resets']} resets, "
          f"{c['relaunches']} relaunches")
    print(f"  hostile values reached {reached} of {swept} handlers through a "
          f"valid baseline")
    if covered:
        print(f"  feature fields: {', '.join(sorted(covered))} here; the "
              f"fixture covers {len(fixture_types)} of "
              f"{len(feature_types)} feature types")
    for f in sw.known:
        print("  known: " + f)
    if sw.failures:
        print(f"hostile_params_smoke {k}/{n}: FAILED ({len(sw.failures)})")
        for f in sw.failures:
            print("  - " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
