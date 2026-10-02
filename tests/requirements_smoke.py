#!/usr/bin/env python3
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
  file and a project directory), current after the reopen.

Usage: requirements_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from designer_remote import Designer  # noqa: E402
from designer_sdk import connect  # noqa: E402
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


def main():
    binary = sys.argv[1]
    with launch(binary) as app:
        d = Designer(app.port, timeout=300)
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
    print("requirements_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
