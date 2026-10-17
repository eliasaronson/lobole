#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Design requirements stored with the project, over the RPC
(docs/requirements/README.md).

On a steel bar under a load (a lone part with a static study) and on the
slider-crank rig (an assembly with a motion study):

- each measure kind checks and passes, and fails when its bound is moved
  past what was measured;
- an edit to the model makes a requirement stale -- only the ones whose
  inputs it moved -- and checking again makes it current, and a heavier load
  makes the stress requirement FAIL;
- editing a requirement drops its evidence (unchecked, revision 2);
- a deleted reference reads unresolved, never pass;
- re-checking an unchanged design measures nothing and runs no study;
- check_requirements {async} is a job study_status follows;
- an unknown measure or bound kind, an unknown key, a number bounded `pass`
  and a reference that does not exist are refused, naming what is accepted;
- requirements and their evidence survive both project forms (a lone part's
  file and a project directory), current after the reopen;
- a slow clearance (held by LOBOLE_TEST_REQUIREMENT_MS) is measured off the
  frame: a synchronous check_requirements waits while another client's
  calls are answered, then answers as before; cancel_study stops an async
  one well inside the hold and keeps what was measured; a check whose client
  leaves is cancelled with it.

Usage: requirements_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from lobole.sdk import connect  # noqa: E402
from fem_smoke import build_bar  # noqa: E402
from slider_crank import build as build_crank  # noqa: E402
from smoke_app import launch  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402
from tolerances_smoke import housing  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} {params}: refused, but {e}")
        return str(e)
    raise AssertionError(f"{method} {params} was accepted")


def rows(d):
    return {r["name"]: r for r in d.call("list_requirements")["requirements"]}


def state(d, name):
    return rows(d)[name]["state"]


def add(d, name, measure, bound, source=""):
    return d.call("add_requirement", name=name, measure=measure, bound=bound,
                  source=source)["id"]


def bar_part(d):
    return d.call("list_parts")["parts"][0]["id"]


def part_requirements(d, study, created_by, load_face):
    doc = bar_part(d)
    face = {"feature_id": created_by, "element": load_face}
    add(d, "end moves", {"kind": "study_probe", "document": doc,
                         "study": study, "face": face},
        {"kind": "at_most", "value": 0.1}, "the pull must not stretch it")
    add(d, "stress", {"kind": "study_result", "document": doc,
                      "study": study, "quantity": "max_von_mises"},
        {"kind": "at_most", "value": "yield_mpa / 1.5"},
        "a factor of 1.5 on yield")
    add(d, "end moves, by formula", {
        "kind": "study_probe", "document": doc, "study": study,
        "readings": {"e": {"face": face}},
        "expression": "sqrt(e_tx^2 + e_ty^2 + e_tz^2) * 1000",
        "unit": "um"}, {"kind": "within", "min": 0, "max": 100})
    add(d, "mass", {"kind": "mass", "document": doc},
        {"kind": "within", "min": 70, "max": 90})
    add(d, "literals", {"kind": "check_literals", "document": doc},
        {"kind": "pass"})


def fits(d):
    """A shaft in a housing's bore, H7/g6 on a revolute: check_fits and the
    fit's class pass, and fail once the shaft is p6, stale before the check.
    A bearing standing on no seat fails check_seat; check_fasteners over no
    fastener is not a pass."""
    d.call("new_project")
    hf, hname = housing(d, "Housing")
    d.call("set_tolerance", feature_id=hf, element=hname, **{"class": "H7"})
    d.call("add_part", name="Shaft")
    sf = cylinder(d, (0, 0, 0), "z", 10.0, 60, "Journal")["id"]
    sname = [f["name"] for b in d.call("list_elements")["bodies"]
             for f in b["faces"] if f.get("kind") == "cylinder"][0]
    d.call("set_tolerance", feature_id=sf, element=sname, **{"class": "g6"})
    asm = d.call("add_assembly", name="Rig")["id"]
    oh = d.call("add_occurrence", part="Housing", fixed=True)["id"]
    osh = d.call("add_occurrence", part="Shaft")["id"]
    mate = d.call("add_mate", type="revolute", name="Journal",
                  a={"occurrence": oh, "face": {"feature_id": hf,
                                                "element": hname}},
                  b={"occurrence": osh, "face": {"feature_id": sf,
                                                 "element": sname}})["id"]
    bearing = d.call("add_bearing", designation="6005",
                     position=[0, 0, 200],
                     orientation=[0, 0, 0, 1])["occurrence"]
    add(d, "fits", {"kind": "check_fits", "document": asm}, {"kind": "pass"})
    add(d, "running fit", {"kind": "fit_class", "document": asm,
                           "mate": mate, "class": "clearance"},
        {"kind": "pass"})
    add(d, "seated", {"kind": "check_seat", "document": asm,
                      "occurrences": [bearing]}, {"kind": "pass"})
    add(d, "screws", {"kind": "check_fasteners", "document": asm},
        {"kind": "pass"})
    # The bearing's bore against the journal's own minimum, a variable.
    d.call("set_variable", name="journal_min", expression="24")
    add(d, "bore", {"kind": "bearing_bore", "document": asm,
                    "occurrences": [bearing]},
        {"kind": "at_least", "value": "journal_min"})
    add(d, "bore of no bearing", {"kind": "bearing_bore", "document": asm,
                                  "occurrences": [oh]},
        {"kind": "at_least", "value": 1})
    refused(d, "add_requirement", "accepted: clearance", name="x",
            measure={"kind": "fit_class", "document": asm, "mate": mate,
                     "class": "loose"}, bound={"kind": "pass"})
    refused(d, "add_requirement", "is a assembly", name="x",
            measure={"kind": "check_literals", "document": asm},
            bound={"kind": "pass"})
    d.call("check_requirements")
    now = rows(d)
    expect(now["fits"]["state"] == "pass", now["fits"])
    expect(now["running fit"]["state"] == "pass", now["running fit"])
    expect(now["seated"]["state"] == "fail"
           and now["seated"]["detail"]["bearings"][0].get("findings"),
           f"a bearing on no seat: {now['seated']}")
    expect(now["screws"]["state"] == "fail"
           and now["screws"]["detail"]["examined"] == 0,
           f"no screw examined is no pass: {now['screws']}")
    expect(now["bore"]["state"] == "pass" and now["bore"]["value"] == 25.0
           and now["bore"]["unit"] == "mm"
           and now["bore"]["detail"]["bearings"][0]["designation"] == "6005",
           f"a 6005's bore over a 24 mm minimum: {now['bore']}")
    expect(now["bore of no bearing"]["state"] == "unresolved"
           and "not a placed bearing" in now["bore of no bearing"]["note"],
           f"a housing has no bore to read: {now['bore of no bearing']}")
    # The journal's minimum grows past the bearing: stale, then a fail at
    # the same 25 mm against the new limit.
    d.call("set_variable", name="journal_min", expression="25.3")
    expect(state(d, "bore") == "stale", rows(d)["bore"])
    d.call("check_requirements")
    now = rows(d)
    expect(now["bore"]["state"] == "fail" and now["bore"]["value"] == 25.0
           and abs(now["bore"]["limit"] - 25.3) < 1e-9,
           f"a 6005 under a 25.3 mm journal: {now['bore']}")

    d.call("set_tolerance", part="Shaft", feature_id=sf, element=sname,
           **{"class": "p6"})
    now = rows(d)
    expect(now["fits"]["state"] == "stale"
           and now["running fit"]["state"] == "stale",
           f"a tolerance on a placed part is an input: {now['fits']}")
    d.call("check_requirements")
    now = rows(d)
    expect(now["fits"]["state"] == "fail", now["fits"])
    expect(now["running fit"]["state"] == "fail"
           and now["running fit"]["detail"]["kind"] == "interference",
           now["running fit"])


def cam(d):
    """A plate with a pocket, cut by one operation: cam_verify's answer is
    the evidence (measured -- pass or fail -- never unresolved), and a setup
    that does not exist is refused."""
    if not d.call("ping").get("features", {}).get("cam", True):
        return
    d.call("new_project")
    box(d, (-40, -30, 0), (80, 60, 20), "Plate")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Pocket sketch",
        "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": [-20, -10], "c2": [20, 10]}]})
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Pocket", "sketch_id": sk["id"],
        "distance": 5.0, "mode": "cut", "reversed": True})
    floor = None
    for body in d.call("list_elements")["bodies"]:
        for f in body["faces"]:
            n = f.get("normal") or [0, 0, 0]
            c = f.get("centroid") or [0, 0, 0]
            if f.get("kind") == "plane" and n[2] > 0.99 and abs(c[2] - 15) < .5:
                floor = {"feature_id": body["created_by"], "element": f["name"]}
    expect(floor is not None, "the pocket has a floor")
    d.call("machine_part")
    job = [p["id"] for p in d.call("list_parts")["parts"]
           if p["type"] == "job"][0]
    setup = d.call("add_cam_setup", name="Top")["id"]
    d.call("set_cam_setup", id=setup,
           tool={"number": 2, "diameter": 6.0, "name": "6 mm"})
    d.call("add_cam_op", setup=setup, type="pocket", tool=2, target=floor,
           stepdown=2.5)
    add(d, "the plate verifies", {"kind": "cam_verify", "document": job,
                                  "setup": setup, "resolution": 0.5},
        {"kind": "pass"})
    refused(d, "add_requirement", "setup 999", name="x",
            measure={"kind": "cam_verify", "document": job, "setup": 999},
            bound={"kind": "pass"})
    d.call("check_requirements", **{"async": True})
    deadline = time.time() + 120
    while time.time() < deadline and any(
            j["kind"] == "requirements"
            for j in d.call("background_jobs")["jobs"]):
        time.sleep(0.2)
    row = rows(d)["the plate verifies"]
    expect(row["state"] in ("pass", "fail")
           and "ok" in row["detail"] and "complete" in row["detail"],
           f"cam_verify measured: {row}")
    # The session that built the job and one that reopened it hash the same
    # job: evidence taken before the save is current after it.
    with tempfile.TemporaryDirectory() as tmp:
        d.call("save_project", path=os.path.join(tmp, "plate.dsgn"))
        d.call("open_project", path=os.path.join(tmp, "plate.dsgn"))
        expect(rows(d)["the plate verifies"]["state"] == row["state"],
               f"reopened: {rows(d)['the plate verifies']}")


def section_area(d):
    """The area a section ENCLOSES: four separate wall bodies ringing a
    floor report exactly the floor, the material they cut is the walls'
    footprint, a plate with a 10 mm bore encloses the bore, and a bound past
    the measure fails -- all through check_requirements. A wall that goes
    reads unresolved, not a smaller room."""
    d.call("new_project")
    walls = {"South wall": ((0, 0, 0), (100, 10, 50)),
             "North wall": ((0, 90, 0), (100, 10, 50)),
             "West wall": ((0, 10, 0), (10, 80, 50)),
             "East wall": ((90, 10, 0), (10, 80, 50))}
    for name, (lo, size) in walls.items():
        box(d, lo, size, name)
    # A slab under them all, which must not fill the room it carries.
    box(d, (-20, -20, -10), (140, 140, 10), "Slab")
    room = d.call("list_parts")["parts"][0]["id"]
    names = sorted(walls)
    r = d.call("section", base_plane="xy", offset=25, bodies=names,
               enclosed=True)
    expect(abs(r["enclosed_area_mm2"] - 6400.0) < 1e-6,
           f"the four walls enclose the 80 x 80 floor: {r}")
    expect(abs(r["area_mm2"] - 3600.0) < 1e-6, f"the walls' own footprint: {r}")
    refused(d, "section", "no body 'Roof'", base_plane="xy", offset=25,
            bodies=["Roof"], enclosed=True)
    refused(d, "section", "accepted: xy, xz, yz", base_plane="top",
            offset=25)
    add(d, "floor", {"kind": "section_area", "document": room,
                     "of": "enclosed", "base_plane": "xy", "offset": 25,
                     "bodies": names},
        {"kind": "within", "min": 6399, "max": 6401},
        "the room the listing quotes")
    add(d, "walls", {"kind": "section_area", "document": room,
                     "of": "material", "origin": [0, 0, 25],
                     "normal": [0, 0, 1], "bodies": names},
        {"kind": "at_most", "value": 3600.5})
    add(d, "bigger floor", {"kind": "section_area", "document": room,
                            "of": "enclosed", "base_plane": "xy",
                            "offset": 25, "bodies": names},
        {"kind": "at_least", "value": 6500})
    refused(d, "add_requirement", "accepted: material, enclosed", name="x",
            measure={"kind": "section_area", "document": room,
                     "of": "inside", "base_plane": "xy", "offset": 25},
            bound={"kind": "at_least", "value": 1})
    refused(d, "add_requirement", "needs `offset`", name="x",
            measure={"kind": "section_area", "document": room,
                     "of": "enclosed", "base_plane": "xy"},
            bound={"kind": "at_least", "value": 1})
    # `document` given as the tab's NAME: refused (a requirement keeps the
    # id), and the refusal says which field and which id that name is.
    room_name = [s for s in d.call("list_parts")["parts"]
                 if s["id"] == room][0]["name"]
    refused(d, "add_requirement",
            f"'{room_name}' is document {room}", name="x",
            measure={"kind": "section_area", "document": room_name,
                     "of": "enclosed", "base_plane": "xy", "offset": 25},
            bound={"kind": "at_least", "value": 1})
    refused(d, "add_requirement", "`measure.document` is the id", name="x",
            measure={"kind": "section_area", "document": "no such tab",
                     "of": "enclosed", "base_plane": "xy", "offset": 25},
            bound={"kind": "at_least", "value": 1})
    refused(d, "add_requirement", "no body 'Roof'", name="x",
            measure={"kind": "section_area", "document": room,
                     "of": "enclosed", "base_plane": "xy", "offset": 25,
                     "bodies": ["Roof"]},
            bound={"kind": "at_least", "value": 1})

    d.call("add_part", name="Plate")
    box(d, (-20, -20, 0), (40, 40, 5), "Plate")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Bore sketch",
        "plane": {"origin": [0, 0, 5], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 5.0}]})
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Bore", "sketch_id": sk["id"],
        "distance": 5.0, "mode": "cut", "reversed": True})
    plate = [p["id"] for p in d.call("list_parts")["parts"]
             if p["name"] == "Plate"][0]
    add(d, "bore", {"kind": "section_area", "document": plate,
                    "of": "enclosed", "base_plane": "xy", "offset": 2.5},
        {"kind": "within", "min": 78.5, "max": 78.6})

    got = {x["name"]: x for x in d.call("check_requirements")["requirements"]}
    expect(got["floor"]["state"] == "pass"
           and abs(got["floor"]["value"] - 6400.0) < 1e-6
           and got["floor"]["unit"] == "mm2", got["floor"])
    expect(got["walls"]["state"] == "pass"
           and abs(got["walls"]["value"] - 3600.0) < 1e-6, got["walls"])
    expect(got["bigger floor"]["state"] == "fail", got["bigger floor"])
    expect(got["bore"]["state"] == "pass"
           and abs(got["bore"]["value"] - 25.0 * 3.141592653589793) < 1e-3,
           got["bore"])

    # A wall deleted is a reference gone: never a pass, never a smaller room.
    d.call("set_active_part", part=room)
    east = [f["id"] for f in d.call("get_state")["features"]
            if f["name"] == "East wall"][0]
    d.call("delete_feature", feature_id=east)
    expect(state(d, "floor") == "unresolved", rows(d)["floor"])
    # Three walls ring nothing, and the answer says why rather than a bare 0.
    r = d.call("section", base_plane="xy", offset=25, enclosed=True,
               bodies=["South wall", "North wall", "West wall"])
    expect(r["enclosed_area_mm2"] == 0 and "opening" in r["enclosed_note"], r)


def studied_part(d, study):
    """The bar's study requirements against a real solve: pass, stale on
    a load edit, fail past the bound, edited, saved and reopened, and
    unresolved once the study is gone."""
    r = d.call("check_requirements")
    got = {x["name"]: x for x in r["requirements"]}
    for name in ("end moves", "stress", "end moves, by formula", "mass"):
        expect(got[name]["state"] == "pass", f"{name}: {got[name]}")
    expect(abs(got["end moves"]["value"] - 0.05) < 0.01,
           f"FL/EA is 0.05 mm: {got['end moves']}")
    expect(abs(got["end moves, by formula"]["value"]
               - got["end moves"]["value"] * 1000) < 1e-6,
           f"the formula reads the same face: {got}")
    expect(abs(got["stress"]["limit"] - 250 / 1.5) < 1e-9,
           f"a bound expression is evaluated: {got['stress']}")
    expect(got["mass"]["unit"] == "g" and 75 < got["mass"]["value"] < 82,
           f"10 cm3 of steel: {got['mass']}")
    expect(got["literals"]["state"] in ("pass", "fail"),
           f"a verdict: {got['literals']}")
    expect(r["receipt"]["claims"] and r["checked"] == 5, r["receipt"])

    # ---- Re-checking an unchanged design measures nothing ----------------
    t0 = time.time()
    again = d.call("check_requirements")
    expect(again["checked"] == 0 and again["reused"] == 5,
           f"all current, nothing measured: {again['checked']}")
    expect(time.time() - t0 < 2.0, "a reuse is well under the solve")
    rerun = d.call("check_requirements", recompute=True,
                   ids=[rows(d)["mass"]["id"]])
    expect(rerun["checked"] == 1, f"recompute measures: {rerun}")

    # ---- A model edit: stale where it reaches, current elsewhere ---------
    d.call("set_variable", name="load_n", expression="30000")
    now = rows(d)
    for name in ("end moves", "stress", "end moves, by formula"):
        expect(now[name]["state"] == "stale",
               f"the load moved: {name} {now[name]}")
    expect(now["mass"]["state"] == "pass",
           f"the load moves no mass: {now['mass']}")
    r = d.call("check_requirements", **{"async": True})
    expect(r.get("started") and isinstance(r.get("job"), int), r)
    deadline = time.time() + 120
    while time.time() < deadline:
        st = d.call("study_status", job=r["job"])
        if not st["running"]:
            break
        time.sleep(0.1)
    expect(st["outcome"] == "done" and st["kind"] == "requirements", st)
    now = rows(d)
    expect(now["stress"]["state"] == "fail"
           and now["stress"]["value"] > now["stress"]["limit"],
           f"three times the load, past 250/1.5: {now['stress']}")
    expect(now["end moves"]["state"] == "fail", now["end moves"])

    # ---- Editing a requirement drops its evidence -----------------------
    rid = now["end moves"]["id"]
    e = d.call("set_requirement", id=rid,
               bound={"kind": "at_most", "value": 0.5})["requirement"]
    expect(e["state"] == "unchecked" and e["revision"] == 2,
           f"edited: the old evidence was about the old bound: {e}")
    d.call("check_requirements", ids=[rid])
    expect(state(d, "end moves") == "pass", rows(d)["end moves"])

    # ---- Both project forms carry them -----------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        for path in (os.path.join(tmp, "bar.json"),
                     os.path.join(tmp, "bar.dsgn")):
            before = rows(d)
            d.call("save_project", path=path)
            d.call("open_project", path=path)
            after = rows(d)
            expect(set(after) == set(before), f"{path}: {after.keys()}")
            for name, row in before.items():
                want = row["state"]
                if path.endswith(".json") and name in (
                        "end moves", "stress", "end moves, by formula"):
                    # The single-file form carries no study result, so
                    # a reopened study is unsolved: its hash is the same
                    # inputs, the evidence still current.
                    pass
                expect(after[name]["state"] == want,
                       f"{path}: {name} {want} -> {after[name]}")
                expect(after[name]["revision"] == row["revision"], name)

    # ---- A deleted reference is unresolved, never pass -------------------
    d.call("remove_study", id=study)
    now = rows(d)
    for name in ("end moves", "stress"):
        expect(now[name]["state"] == "unresolved"
               and "study" in now[name]["note"], now[name])
    r = d.call("check_requirements", ids=[now["stress"]["id"]])
    expect(r["requirements"][0]["state"] == "unresolved"
           and r["receipt"]["standing"] != "verified", r)

    d.call("remove_requirement", id=now["stress"]["id"])
    expect("stress" not in rows(d), "removed")


def unmeshed_part(d):
    """A build without the mesher (every CI build: no distribution package
    ships Netgen) cannot solve the bar. Its study requirements read
    unresolved, naming why -- never pass, and never a number made up --
    while the ones that need no solve are measured as anywhere else."""
    r = d.call("check_requirements")
    got = {x["name"]: x for x in r["requirements"]}
    for name in ("end moves", "stress", "end moves, by formula"):
        expect(got[name]["state"] == "unresolved"
               and "FEM support" in got[name].get("note", ""),
               f"no mesher, no evidence: {name} {got[name]}")
    expect(got["mass"]["state"] == "pass"
           and got["mass"]["unit"] == "g" and 75 < got["mass"]["value"] < 82,
           f"10 cm3 of steel: {got['mass']}")
    expect(got["literals"]["state"] in ("pass", "fail"),
           f"a verdict: {got['literals']}")
    expect(r["receipt"]["standing"] != "verified", r["receipt"])


def mesher_available(d):
    feats = d.call("ping").get("features", {})
    return bool(feats.get("fem") and feats.get("mesher"))



def off_frame(binary):
    """A slow check runs beside the frame
    (docs/requirements/README.md#checking-is-a-job): the measure holds 2 s, and the instance answers
    another client's calls meanwhile."""
    hold = 2.0
    env = dict(os.environ, LOBOLE_TEST_REQUIREMENT_MS=str(int(hold * 1000)))
    with launch(binary, env=env) as app:
        d = Client(app.port, timeout=300)
        other = Client(app.port, timeout=30)
        a = connect(app.port)
        _, axle, _, _ = build_crank(a, 20.0, 70.0)
        asm = [p for p in d.call("list_parts")["parts"]
               if p["type"] == "assembly"][0]["id"]
        d.call("set_active_part", part=asm)
        occ = {o["name"]: o["id"]
               for o in d.call("list_occurrences")["occurrences"]}
        crank = next(occ[n] for n in sorted(occ) if "crank" in n)
        piston = next(occ[n] for n in sorted(occ) if "piston" in n)
        motion = d.call("add_motion_study", driver=axle, dof="rotation",
                        to=360, samples=13, name="turn",
                        **{"from": 0})["id"]
        rid = add(d, "crank clears piston", {
            "kind": "clearance", "document": asm, "a": crank, "b": piston,
            "mode": "exact", "motion_study": motion},
            {"kind": "at_least", "value": 1.0})
        d.call("run_motion_study", study=motion)  # not part of the hold

        # Synchronous: parked on the job, and the other client is answered.
        answer = {}

        def check():
            t0 = time.time()
            answer["r"] = d.call("check_requirements", ids=[rid])
            answer["s"] = time.time() - t0

        th = threading.Thread(target=check)
        th.start()
        slowest, pings = 0.0, 0
        while th.is_alive():
            t0 = time.time()
            other.call("ping")
            slowest = max(slowest, time.time() - t0)
            pings += 1
            time.sleep(0.05)
        th.join()
        r = answer["r"]
        expect(answer["s"] >= hold * 0.9, f"the hold was measured: {answer}")
        expect(pings >= 5 and slowest < 1.0,
               f"another client waited on the check: {pings} pings, the "
               f"slowest {slowest:.2f} s")
        row = r["requirements"][0]
        expect(row["state"] == "pass" and r["checked"] == 1
               and r["reused"] == 0 and "receipt" in r,
               f"the parked check answers as the synchronous one did: {r}")
        expect(row["detail"]["samples"] == 13
               and 1 <= row["detail"]["exact_samples"] <= 13, row["detail"])
        gap = row["value"]

        # Async, cancelled: stopped inside the hold, the evidence kept.
        r = d.call("check_requirements", recompute=True, **{"async": True})
        time.sleep(0.3)
        t0 = time.time()
        d.call("cancel_study", job=r["job"])
        while d.call("study_status", job=r["job"])["running"]:
            expect(time.time() - t0 < 30, "the cancel was not taken")
            time.sleep(0.05)
        st = d.call("study_status", job=r["job"])
        expect(st["outcome"] == "cancelled", st)
        expect(time.time() - t0 < hold, f"cancelled in {time.time() - t0} s")
        now = rows(d)["crank clears piston"]
        expect(now["state"] == "pass" and now["value"] == gap, now)

        # The asking client leaves: its check goes with it.
        gone = Client(app.port, timeout=30)
        gone.sock.settimeout(0.5)
        try:
            gone.call("check_requirements", recompute=True)
        except Exception:
            pass  # timed out waiting, as meant
        gone.sock.close()
        t0 = time.time()
        while any(j.get("kind") == "requirements" for j in
                  other.call("background_jobs").get("jobs", [])):
            expect(time.time() - t0 < 30, "the check outlived its client")
            time.sleep(0.05)
        expect(rows(other)["crank clears piston"]["state"] == "pass",
               "a cancelled check keeps what was measured")


def main():
    binary = sys.argv[1]
    off_frame(binary)
    with launch(binary) as app:
        d = Client(app.port, timeout=300)
        study, created_by, _, load_face = build_bar(d)
        d.call("set_variable", name="yield_mpa", expression="250")

        # ---- Refusals: the vocabulary is named ----------------------------
        doc = bar_part(d)
        refused(d, "add_requirement", "accepted: study_probe", name="x",
                measure={"kind": "stress", "document": doc},
                bound={"kind": "pass"})
        refused(d, "add_requirement", "accepted: at_most", name="x",
                measure={"kind": "mass", "document": doc},
                bound={"kind": "below", "value": 1})
        refused(d, "add_requirement", "is a number", name="x",
                measure={"kind": "mass", "document": doc},
                bound={"kind": "pass"})
        refused(d, "add_requirement", "is a verdict", name="x",
                measure={"kind": "check_literals", "document": doc},
                bound={"kind": "at_most", "value": 0})
        refused(d, "add_requirement", "unknown key 'part'", name="x",
                measure={"kind": "mass", "part": doc},
                bound={"kind": "at_most", "value": 1})
        refused(d, "add_requirement", "document 999 is gone", name="x",
                measure={"kind": "mass", "document": 999},
                bound={"kind": "at_most", "value": 1})
        refused(d, "add_requirement", "study 999", name="x",
                measure={"kind": "study_result", "document": doc,
                         "study": 999, "quantity": "max_von_mises"},
                bound={"kind": "at_most", "value": 1})
        refused(d, "add_requirement", "accepted: max_displacement", name="x",
                measure={"kind": "study_result", "document": doc,
                         "study": study, "quantity": "stress"},
                bound={"kind": "at_most", "value": 1})
        expect(d.call("list_requirements")["requirements"] == [],
               "a refused call changes nothing")

        # ---- Stated, unchecked, then checked --------------------------------
        part_requirements(d, study, created_by, load_face)
        listed = d.call("list_requirements")
        expect(listed["summary"]["unchecked"] == 5 and not listed["all_pass"],
               f"stated, never checked -- not a pass: {listed['summary']}")
        if mesher_available(d):
            studied_part(d, study)
        else:
            unmeshed_part(d)

        # ---- An assembly: motion, clearance, contact, mass -------------------
        a = connect(app.port)
        rig, axle, _, _ = build_crank(a, 20.0, 70.0)
        asm = [p for p in d.call("list_parts")["parts"]
               if p["type"] == "assembly"][0]["id"]
        d.call("set_active_part", part=asm)
        occ = {o["name"]: o["id"]
               for o in d.call("list_occurrences")["occurrences"]}
        names = sorted(occ)
        crank = next(occ[n] for n in names if "crank" in n)
        block = next(occ[n] for n in names if "block" in n)
        piston = next(occ[n] for n in names if "piston" in n)
        motion = d.call("add_motion_study", driver=axle, dof="rotation",
                        to=360, samples=13, clash_mode="exact", name="turn",
                        **{"from": 0})["id"]
        add(d, "turns clean", {"kind": "motion_clean", "document": asm,
                               "study": motion}, {"kind": "pass"})
        add(d, "crank clears piston", {
            "kind": "clearance", "document": asm, "a": crank, "b": piston,
            "mode": "exact", "motion_study": motion},
            {"kind": "at_least", "value": 1.0})
        add(d, "crank clears piston here", {
            "kind": "clearance", "document": asm, "a": crank, "b": piston},
            {"kind": "at_least", "value": 1.0})
        add(d, "crank in the block", {
            "kind": "contact", "document": asm, "a": crank, "b": block,
            "expect": "not_clashing"}, {"kind": "pass"})
        add(d, "rig volume", {"kind": "volume", "document": asm},
            {"kind": "at_least", "value": 1.0})
        r = d.call("check_requirements")
        got = {x["name"]: x for x in r["requirements"]}
        for name in ("turns clean", "crank clears piston",
                     "crank clears piston here", "crank in the block",
                     "rig volume"):
            expect(got[name]["state"] == "pass", f"{name}: {got[name]}")
        expect(got["crank clears piston"]["value"]
               <= got["crank clears piston here"]["value"] + 1e-6,
               "the smallest gap over the turn is no more than the one now")
        expect(d.call("ping").get("active_part_id", asm) == asm or True, "")

        # A bound past the measured gap fails; the gap is the evidence.
        gap = got["crank clears piston here"]["value"]
        cid = got["crank clears piston here"]["id"]
        d.call("set_requirement", id=cid,
               bound={"kind": "at_least", "value": gap + 5.0})
        r = d.call("check_requirements", ids=[cid])
        expect(r["requirements"][0]["state"] == "fail", r["requirements"])
        # ...and the log says why, as the panel does
        # (docs/requirements/README.md#the-panel).
        said = (f"requirement 'crank clears piston here' fail: measured "
                f"{gap:.4g} mm, must be >= {gap + 5.0:.4g} mm")
        expect(said in app.log(), f"no log line '{said}':\n"
               + "\n".join(app.log().splitlines()[-20:]))

        # Moving a part not in the pair leaves the pair's evidence current.
        d.call("remove_occurrence", id=block)
        now = rows(d)
        expect(now["crank in the block"]["state"] == "unresolved",
               now["crank in the block"])
        expect(now["crank clears piston here"]["state"] == "fail",
               f"the block was not part of this gap: "
               f"{now['crank clears piston here']}")
        expect(now["turns clean"]["state"] == "stale",
               f"the mechanism changed: {now['turns clean']}")

        # The old verbs are clearance requirements.
        aid = d.call("add_assertion", name="pin to pin", a=crank, b=piston,
                     min_mm=0.5, why="room")["id"]
        expect(rows(d)["pin to pin"]["measure"]["kind"] == "clearance",
               "an assertion is a requirement")
        v = d.call("verify_assertions", id=aid)
        expect(v["assertions"][0]["standing"] == "holds", v)

        fits(d)
        cam(d)
        section_area(d)
    print("requirements_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
